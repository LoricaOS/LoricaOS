#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>   /* for SIGCHLD, struct sigaction, sigaction() */

static long
sys_setfg(long pid)
{
#ifdef __aarch64__
    /* ARM64: Aegis custom syscall 360 (x86 number, passed directly) */
    register long x8 __asm__("x8") = 360;
    register long x0 __asm__("x0") = pid;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory");
    return x0;
#else
    long ret;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "0"(360L), "D"(pid)
        : "rcx", "r11", "memory"
    );
    return ret;
#endif
}

#define MAX_PIPELINE 6    /* max pipeline stages; (PROC_MAX_FDS-3)/2 = 6 */
#define MAX_ARGV     16   /* max args per command */
#define LINE_SIZE    512  /* input line buffer */

typedef struct {
    char *argv[MAX_ARGV + 1]; /* NULL-terminated */
    char *stdin_file;         /* path for < redirect, NULL if none */
    char *stdout_file;        /* path for > redirect (ignored until Phase 19) */
    int   stderr_to_stdout;   /* 1 if 2>&1 was specified */
} cmd_t;

static char *g_envp[] = { NULL };

/*
 * parse_command — tokenize a single command segment, pulling out redirects.
 * Modifies seg in-place (inserts NUL terminators).
 * Returns argc (number of argv entries, not counting the NULL terminator).
 */
static int
parse_command(char *seg, cmd_t *cmd)
{
    cmd->argv[0]        = NULL;
    cmd->stdin_file     = NULL;
    cmd->stdout_file    = NULL;
    cmd->stderr_to_stdout = 0;

    int argc = 0;
    char *p  = seg;

    while (*p) {
        /* Skip whitespace */
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '\n') break;

        /* 2>&1 — must check before single '>' */
        if (p[0]=='2' && p[1]=='>' && p[2]=='&' && p[3]=='1') {
            cmd->stderr_to_stdout = 1;
            p += 4;
            continue;
        }

        /* >> (append) — store path, mark no-op until Phase 19 */
        if (p[0] == '>' && p[1] == '>') {
            p += 2;
            while (*p == ' ' || *p == '\t') p++;
            cmd->stdout_file = p;
            while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
            if (*p) *p++ = '\0';
            continue;
        }

        /* > (truncate) */
        if (*p == '>') {
            p++;
            while (*p == ' ' || *p == '\t') p++;
            cmd->stdout_file = p;
            while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
            if (*p) *p++ = '\0';
            continue;
        }

        /* < (stdin redirect) */
        if (*p == '<') {
            p++;
            while (*p == ' ' || *p == '\t') p++;
            cmd->stdin_file = p;
            while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
            if (*p) *p++ = '\0';
            continue;
        }

        /* Regular argument */
        if (argc >= MAX_ARGV) break;
        cmd->argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
        if (*p) *p++ = '\0';
    }
    cmd->argv[argc] = NULL;
    return argc;
}

/*
 * parse_pipeline — split line on '|' into up to max cmd_t entries.
 * Modifies line in-place. Returns number of stages (0 if line is empty).
 */
static int
parse_pipeline(char *line, cmd_t *cmds, int max)
{
    int n    = 0;
    char *p  = line;
    char *seg = line;

    while (*p) {
        if (*p == '|') {
            *p++ = '\0';
            if (n < max) {
                if (parse_command(seg, &cmds[n]) > 0)
                    n++;
            }
            seg = p;
        } else {
            p++;
        }
    }
    /* Last (or only) segment */
    if (n < max && parse_command(seg, &cmds[n]) > 0)
        n++;

    return n;
}

/*
 * exec_cmd — exec the command in cmds[i]. Never returns on success.
 * If the binary is not found, prints an error and calls _exit(127).
 */
static void
exec_cmd(cmd_t *cmd)
{
    char full[64];
    if (cmd->argv[0][0] != '/') {
        snprintf(full, sizeof(full), "/bin/%s", cmd->argv[0]);
        execve(full, cmd->argv, g_envp);
    } else {
        execve(cmd->argv[0], cmd->argv, g_envp);
    }
    fprintf(stderr, "%s: not found\n", cmd->argv[0]);
    _exit(127);
}

/*
 * run_pipeline — execute an N-stage pipeline.
 *
 * Creates N-1 pipes, forks N children with dup2 redirects, then:
 *   1. Parent closes all pipe fds (must happen before waitpid — otherwise
 *      parent holds write ends open and last-stage reader never gets EOF).
 *   2. Parent waits for all children in order.
 *
 * CRITICAL: every child closes ALL pipe fds after its dup2 redirects.
 * If a child retains the write end of a pipe it is also reading from,
 * write_refs stays > 1 and the child will never see EOF from that pipe.
 */
static void
run_pipeline(cmd_t *cmds, int n)
{
    int pipes[MAX_PIPELINE - 1][2];
    int i, j;

    /* Create n-1 pipes */
    for (i = 0; i < n - 1; i++) {
        if (pipe(pipes[i]) < 0) {
            perror("pipe");
            /* Clean up already-opened pipes */
            for (j = 0; j < i; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }
            return;
        }
    }

    pid_t pids[MAX_PIPELINE];

    for (i = 0; i < n; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            /* Close remaining pipes and wait for already-forked children */
            for (j = i; j < n - 1; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }
            int st;
            for (j = 0; j < i; j++)
                waitpid(pids[j], &st, 0);
            return;
        }

        if (pid == 0) {
            /* ── Child process ── */

            /* Wire stdin from previous pipe */
            if (i > 0)
                dup2(pipes[i-1][0], STDIN_FILENO);

            /* Wire stdout to next pipe */
            if (i < n - 1)
                dup2(pipes[i][1], STDOUT_FILENO);

            /* < stdin redirect */
            if (cmds[i].stdin_file) {
                int fd = open(cmds[i].stdin_file, O_RDONLY);
                if (fd < 0) {
                    fprintf(stderr, "%s: cannot open\n", cmds[i].stdin_file);
                    _exit(1);
                }
                dup2(fd, STDIN_FILENO);
                close(fd);
            }

            /* > stdout redirect — create/truncate the output file */
            if (cmds[i].stdout_file && i == n - 1) {
                int fd = open(cmds[i].stdout_file,
                              O_WRONLY | O_CREAT | O_TRUNC, 0644);
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

            /* CRITICAL: close all pipe fds in the child.
             * Without this, a child reading from pipes[i-1][0] that also
             * holds pipes[i-1][1] open will never see EOF after the writer
             * exits (write_refs stays > 0 because the child holds it). */
            for (j = 0; j < n - 1; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }

            exec_cmd(&cmds[i]);
            _exit(127);   /* unreachable if exec succeeds */
        }

        pids[i] = pid;
    }

    /* ── Parent: close all pipe fds after all children are forked ──
     * Must happen before waitpid — if parent holds write ends open,
     * the last-stage reader (cat, etc.) never gets EOF and hangs. */
    for (j = 0; j < n - 1; j++) {
        close(pipes[j][0]);
        close(pipes[j][1]);
    }

    /* Register the last stage as foreground process so Ctrl-C sends SIGINT to it.
     * Use pids[n-1] (last stage) for single-command and for the terminal stage
     * of a pipeline (the stage the user interacts with). */
    sys_setfg((long)pids[n - 1]);

    /* Wait for all children in order */
    int status;
    for (i = 0; i < n; i++)
        waitpid(pids[i], &status, 0);

    /* Clear foreground process after all children exit */
    sys_setfg(0);
}

int
main(int argc, char **argv)
{
    char  line[LINE_SIZE];
    cmd_t cmds[MAX_PIPELINE];

    /* -c <command>: execute a single command string and exit.
     * Used by vigil start_service: execv("/bin/sh", ["/bin/sh", "-c", cmd, NULL]).
     * We do NOT print the shell banner in -c mode. */
    if (argc >= 3 && strcmp(argv[1], "-c") == 0) {
        /* Copy command string into mutable buffer for parse_pipeline */
        strncpy(line, argv[2], sizeof(line) - 1);
        line[sizeof(line) - 1] = '\0';
        int n = parse_pipeline(line, cmds, MAX_PIPELINE);
        if (n == 0)
            return 0;
        /* Handle exec builtin: replace shell with the target program */
        if (n == 1 && strcmp(cmds[0].argv[0], "exec") == 0 && cmds[0].argv[1]) {
            char full[256];
            char *prog = cmds[0].argv[1];
            /* Shift argv: exec's argv[1..] becomes the new argv[0..] */
            char **exec_argv = &cmds[0].argv[1];
            if (prog[0] != '/') {
                snprintf(full, sizeof(full), "/bin/%s", prog);
                execve(full, exec_argv, g_envp);
            } else {
                execve(prog, exec_argv, g_envp);
            }
            perror(prog);
            return 127;
        }
        run_pipeline(cmds, n);
        return 0;
    }

    printf("[SHELL] Aegis shell ready\n");

    /* Prevent SIGCHLD from terminating the shell.
     * signal_deliver already treats SIGCHLD SIG_DFL as ignore, but
     * install SIG_IGN explicitly as a defence-in-depth measure. */
    {
        struct sigaction sa_chld;
        __builtin_memset(&sa_chld, 0, sizeof(sa_chld));
        sa_chld.sa_handler = SIG_IGN;
        sigaction(SIGCHLD, &sa_chld, NULL);
    }

    for (;;) {
        write(1, "# ", 2);
        if (!fgets(line, sizeof(line), stdin))
            break;

        /* Strip trailing newline */
        int len = (int)strlen(line);
        if (len > 0 && line[len - 1] == '\n')
            line[len - 1] = '\0';

        int n = parse_pipeline(line, cmds, MAX_PIPELINE);
        if (n == 0)
            continue;

        /* Builtins: only for a single command with no stdin redirect.
         * A builtin in a pipeline (echo foo | cd) prints an error. */
        if (n == 1 && !cmds[0].stdin_file && !cmds[0].stderr_to_stdout) {
            if (strcmp(cmds[0].argv[0], "exit") == 0) {
                int code = cmds[0].argv[1] ? atoi(cmds[0].argv[1]) : 0;
                exit(code);
            }
            if (strcmp(cmds[0].argv[0], "cd") == 0) {
                const char *path = cmds[0].argv[1] ? cmds[0].argv[1] : "/";
                if (chdir(path) != 0)
                    perror(path);
                continue;
            }
            if (strcmp(cmds[0].argv[0], "help") == 0) {
                puts("Builtins: cd [path], exit [n], help");
                puts("Other commands are in /bin/");
                continue;
            }
        }

        run_pipeline(cmds, n);
    }
    return 0;
}
