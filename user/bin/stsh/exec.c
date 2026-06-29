#include "stsh.h"
#include <errno.h>  /* explicit, even though stsh.h pulls it in — strerror in exec_cmd */

/* sys_setfg — tell kernel the given PID is the foreground process group
 * for signal routing. Uses the libc syscall(3) wrapper so the same
 * source builds on both x86-64 and aarch64. */
static long
sys_setfg(long pid)
{
    return syscall(SYS_SETFG, pid);
}

/*
 * exec_cmd — exec the command. Never returns on success.
 */
static void
exec_cmd(cmd_t *cmd, char **envp)
{
    char full[256];
    if (cmd->argv[0][0] != '/') {
        snprintf(full, sizeof(full), "/bin/%s", cmd->argv[0]);
        execve(full, cmd->argv, envp);
        /* Bare name not in /bin: try the system app-bundle tree, so
         * `calculator` works from a terminal. /apps is root-controlled
         * like /bin (kernel cap-policy trusted-path anchor). */
        if (errno == ENOENT) {
            snprintf(full, sizeof(full), "/apps/%s/%s",
                     cmd->argv[0], cmd->argv[0]);
            execve(full, cmd->argv, envp);
        }
    } else {
        execve(cmd->argv[0], cmd->argv, envp);
    }
    /* execve returned — print errno so the user knows whether it was
     * ENOENT, EACCES, ENOEXEC, EAGAIN (process limit), etc.  Without
     * this, every failure looks like "not found" and root-causes get
     * conflated. */
    fprintf(stderr, "%s: %s\n", cmd->argv[0], strerror(errno));
    _exit(127);
}

/*
 * run_pipeline — execute an N-stage pipeline.
 *
 * Creates N-1 pipes, forks N children with dup2 redirects, then:
 *   1. Parent closes all pipe fds.
 *   2. Parent waits for all children in order.
 *   3. Sets *last_exit from the last child's exit status.
 */
void
run_pipeline(cmd_t *cmds, int n, char **envp, int *last_exit)
{
    int pipes[MAX_PIPELINE - 1][2];
    int i, j;

    /* Create n-1 pipes */
    for (i = 0; i < n - 1; i++) {
        if (pipe(pipes[i]) < 0) {
            perror("pipe");
            for (j = 0; j < i; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }
            *last_exit = 1;
            return;
        }
    }

    pid_t pids[MAX_PIPELINE];

    for (i = 0; i < n; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            for (j = i; j < n - 1; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }
            int st;
            for (j = 0; j < i; j++)
                waitpid(pids[j], &st, 0);
            *last_exit = 1;
            return;
        }

        if (pid == 0) {
            /* ── Child ── */

            /* Wire stdin from previous pipe */
            if (i > 0)
                dup2(pipes[i - 1][0], STDIN_FILENO);

            /* Wire stdout to next pipe */
            if (i < n - 1)
                dup2(pipes[i][1], STDOUT_FILENO);

            /* < stdin redirect */
            if (cmds[i].stdin_file) {
                int fd = open(cmds[i].stdin_file, O_RDONLY);
                if (fd < 0) {
                    fprintf(stderr, "%s: cannot open\n",
                            cmds[i].stdin_file);
                    _exit(1);
                }
                dup2(fd, STDIN_FILENO);
                close(fd);
            }

            /* > or >> stdout redirect */
            if (cmds[i].stdout_file) {
                int flags = O_WRONLY | O_CREAT;
                flags |= cmds[i].stdout_append ? O_APPEND : O_TRUNC;
                int fd = open(cmds[i].stdout_file, flags, 0644);
                if (fd < 0) {
                    fprintf(stderr, "%s: cannot open for writing\n",
                            cmds[i].stdout_file);
                    _exit(1);
                }
                dup2(fd, STDOUT_FILENO);
                close(fd);
            }

            /* 2>&1 */
            if (cmds[i].stderr_to_stdout)
                dup2(STDOUT_FILENO, STDERR_FILENO);

            /* Close all pipe fds in child */
            for (j = 0; j < n - 1; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }

            /* Put child in its own process group for TTY job control.
             * Parent also calls setpgid as a race guard. */
            setpgid(0, 0);
            exec_cmd(&cmds[i], envp);
            _exit(127);
        }

        /* Race guard: parent also sets child's pgid in case the
         * child hasn't run yet when we reach sys_setfg below. */
        setpgid(pid, pid);
        pids[i] = pid;
    }

    /* Parent: close all pipe fds */
    for (j = 0; j < n - 1; j++) {
        close(pipes[j][0]);
        close(pipes[j][1]);
    }

    /* Set last stage as foreground */
    sys_setfg((long)pids[n - 1]);

    /* Forward SIGINT to children so Ctrl-C kills the foreground command */
    static pid_t s_child_pids[MAX_PIPELINE];
    static int   s_nchildren;
    s_nchildren = n;
    for (i = 0; i < n; i++) s_child_pids[i] = pids[i];

    struct sigaction sa_int, sa_old;
    memset(&sa_int, 0, sizeof(sa_int));
    sa_int.sa_handler = SIG_DFL;
    sigaction(SIGINT, &sa_int, &sa_old);

    /* Wait for all children */
    int status;
    for (i = 0; i < n; i++)
        waitpid(pids[i], &status, 0);

    /* Restore shell's SIGINT handling */
    sigaction(SIGINT, &sa_old, NULL);

    /* Clear foreground */
    sys_setfg(0);

    if (WIFEXITED(status))
        *last_exit = WEXITSTATUS(status);
    else if (WIFSIGNALED(status))
        *last_exit = 128 + WTERMSIG(status);
    else
        *last_exit = 1;
}

/*
 * run_pipeline_bg — like run_pipeline but returns immediately without waiting.
 * SIGCHLD is SIG_IGN in the shell, so children are auto-reaped by the kernel.
 */
void
run_pipeline_bg(cmd_t *cmds, int n, char **envp)
{
    int pipes[MAX_PIPELINE - 1][2];
    int i, j;

    for (i = 0; i < n - 1; i++) {
        if (pipe(pipes[i]) < 0) {
            perror("pipe");
            for (j = 0; j < i; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }
            return;
        }
    }

    for (i = 0; i < n; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            for (j = i; j < n - 1; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }
            return;
        }

        if (pid == 0) {
            if (i > 0) dup2(pipes[i - 1][0], STDIN_FILENO);
            if (i < n - 1) dup2(pipes[i][1], STDOUT_FILENO);

            if (cmds[i].stdin_file) {
                int fd = open(cmds[i].stdin_file, O_RDONLY);
                if (fd >= 0) { dup2(fd, STDIN_FILENO); close(fd); }
            }
            if (cmds[i].stdout_file) {
                int flags = O_WRONLY | O_CREAT;
                flags |= cmds[i].stdout_append ? O_APPEND : O_TRUNC;
                int fd = open(cmds[i].stdout_file, flags, 0644);
                if (fd >= 0) { dup2(fd, STDOUT_FILENO); close(fd); }
            }
            if (cmds[i].stderr_to_stdout)
                dup2(STDOUT_FILENO, STDERR_FILENO);

            for (j = 0; j < n - 1; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }
            setpgid(0, 0);
            exec_cmd(&cmds[i], envp);
            _exit(127);
        }

        setpgid(pid, pid);
    }

    for (j = 0; j < n - 1; j++) {
        close(pipes[j][0]);
        close(pipes[j][1]);
    }
    /* Parent returns — SIGCHLD=SIG_IGN means children auto-reap */
}

/*
 * try_builtin — check if a single command is a builtin. Returns 1 if
 * handled, 0 if not. Updates *last_exit.
 */
int
try_builtin(cmd_t *cmds, int n, int *last_exit)
{
    /* Builtins only for a single command, no stdin redirect */
    if (n != 1 || cmds[0].stdin_file || cmds[0].stderr_to_stdout)
        return 0;

    char **argv = cmds[0].argv;
    if (!argv[0])
        return 0;

    if (strcmp(argv[0], "exit") == 0) {
        hist_save();
        int code = argv[1] ? atoi(argv[1]) : *last_exit;
        exit(code);
    }

    if (strcmp(argv[0], "cd") == 0) {
        const char *path = argv[1];
        if (!path) {
            path = env_get("HOME");
            if (!path)
                path = "/";
        }
        if (chdir(path) != 0) {
            perror(path);
            *last_exit = 1;
        } else {
            *last_exit = 0;
        }
        return 1;
    }

    if (strcmp(argv[0], "help") == 0) {
        puts("stsh builtins:");
        puts("  cd [path]       - change directory");
        puts("  exit [n]        - exit shell");
        puts("  export VAR=val  - set environment variable");
        puts("  env             - print environment");
        puts("  caps [pid]      - display capabilities");
        puts("  sandbox ...     - run with restricted caps");
        puts("  admin           - elevate this session (admin password) for herald");
        puts("  deadmin         - drop the admin session");
        puts("  help            - this message");
        *last_exit = 0;
        return 1;
    }

    if (strcmp(argv[0], "export") == 0) {
        if (!argv[1]) {
            env_print_all();
        } else {
            char *eq = strchr(argv[1], '=');
            if (eq) {
                *eq = '\0';
                env_set(argv[1], eq + 1);
                *eq = '='; /* restore */
            } else {
                fprintf(stderr, "export: usage: export VAR=value\n");
                *last_exit = 1;
                return 1;
            }
        }
        *last_exit = 0;
        return 1;
    }

    if (strcmp(argv[0], "env") == 0) {
        env_print_all();
        *last_exit = 0;
        return 1;
    }

    if (strcmp(argv[0], "caps") == 0) {
        int argc = 0;
        while (argv[argc]) argc++;
        *last_exit = caps_builtin(argc, argv);
        return 1;
    }

    if (strcmp(argv[0], "sandbox") == 0) {
        int argc = 0;
        while (argv[argc]) argc++;
        *last_exit = sandbox_builtin(argc, argv, env_as_array());
        return 1;
    }

    if (strcmp(argv[0], "grant") == 0) {
        int argc = 0;
        while (argv[argc]) argc++;
        *last_exit = grant_builtin(argc, argv);
        return 1;
    }

    if (strcmp(argv[0], "admin") == 0) {
        int argc = 0;
        while (argv[argc]) argc++;
        *last_exit = admin_builtin(argc, argv);
        return 1;
    }

    if (strcmp(argv[0], "deadmin") == 0) {
        *last_exit = deadmin_builtin();
        return 1;
    }

    return 0;
}
