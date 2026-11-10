#include "stsh.h"

/* Internal descriptors must not alias standard descriptors, even when the
 * shell was launched with a closed stdin/stdout/stderr. */
static int
private_fd(int fd)
{
    if (fd < 0 || fd > 2) return fd;
    int next = dup(fd);
    int result = next < 0 ? -1 : private_fd(next);
    int error = errno;
    close(fd);
    errno = error;
    return result;
}

int
sh_save_fd(int fd)
{
    return private_fd(dup(fd));
}

int
heredoc_stdin(const char *body)
{
    size_t len = strlen(body);
    /* Aegis pipes hold 4040 bytes. No capless fork-writer, tempfile, blocking
     * prefill, short-write acceptance, or silent truncation. */
    if (len > 4000) { errno = E2BIG; return -1; }
    int p[2];
    if (pipe(p) < 0) return -1;
    size_t done = 0;
    while (done < len) {
        ssize_t n = write(p[1], body + done, len - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            int error = n == 0 ? EIO : errno;
            close(p[0]); close(p[1]); errno = error;
            return -1;
        }
        done += n;
    }
    close(p[1]);
    return private_fd(p[0]);
}

void
redir_close(int fds[3])
{
    for (int i = 0; i < 3; i++) {
        if (fds[i] >= 0) close(fds[i]);
        fds[i] = -1;
    }
}

/* Parent-only lexical evaluation. Each entry owns a snapshot of the effective
 * standard descriptor; `2>&1 >file` must differ from `>file 2>&1`. */
int
redir_prepare(const cmd_t *cmd, int fds[3], int input, int output)
{
    fds[0] = fds[1] = fds[2] = -1;
    if (input >= 0 && (fds[0] = private_fd(dup(input))) < 0) goto fail;
    if (output >= 0 && (fds[1] = private_fd(dup(output))) < 0) goto fail;
    for (int i = 0; i < cmd->nredirs; i++) {
        const redir_t *r = &cmd->redirs[i];
        char expanded[LINE_SIZE];
        const char *text=r->text;
        if (text && r->kind!=R_HERE) {
            if (run_expand_redirect(text,expanded,sizeof expanded)<0) goto fail;
            text=expanded;
        }
        int target = 1, fd = -1;
        switch (r->kind) {
        case R_IN: target = 0; fd = open(text, O_RDONLY); break;
        case R_HERE: target = 0; fd = heredoc_stdin(text); break;
        case R_OUT: case R_APPEND: case R_ERR:
            target = r->kind == R_ERR ? 2 : 1;
            fd = open(text, O_WRONLY | O_CREAT |
                      (r->kind == R_APPEND ? O_APPEND : O_TRUNC), 0644);
            break;
        case R_ERR_OUT: case R_DUP: {
            char *end = NULL;
            long source = r->kind == R_ERR_OUT ? 1 : strtol(text, &end, 10);
            target = r->kind == R_ERR_OUT ? 2 : 1;
            /* Never let syntax discover private pipe/bootstrap descriptors. */
            if (source < 0 || source > 2 ||
                (r->kind == R_DUP && (!*text || *end))) {
                errno = EBADF;
                goto fail;
            }
            fd = dup(fds[source] >= 0 ? fds[source] : (int)source);
            break;
        }
        }
        if (fd < 0 || (fd = private_fd(fd)) < 0) {
            dprintf(fds[2]>=0 ? fds[2] : STDERR_FILENO, "stsh: redirect %s: %s\n",
                    r->kind==R_HERE ? "heredoc" : text ? text : "2>&1", strerror(errno));
            goto cleanup;
        }
        if (fds[target] >= 0) close(fds[target]);
        fds[target] = fd;
    }
    return 0;
fail:
    dprintf(fds[2]>=0 ? fds[2] : STDERR_FILENO,
            "stsh: prepare descriptors: %s\n",strerror(errno));
cleanup:
    redir_close(fds);
    return -1;
}

int
redir_install(const int fds[3])
{
    for (int i = 0; i < 3; i++)
        if (fds[i] >= 0 && dup2(fds[i], i) < 0) return -1;
    return 0;
}

/* Child code cannot write diagnostics. Reserve 125 for pre-exec setup failure
 * and 127 for exec failure. A command returning either gets the same generic
 * parent diagnostic; do not claim these statuses uniquely identify exec. */
static int
child_status(int status, const char *name)
{
    int rc = WIFEXITED(status) ? WEXITSTATUS(status) :
             WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 1;
    if (rc == 125 || rc == 127)
        fprintf(stderr, "stsh: %s: %s (status %d)\n", name,
                rc == 125 ? "child setup failed or command returned 125" :
                            "exec failed or command returned 127", rc);
    return rc;
}

int
sh_wait(pid_t pid, const char *name)
{
    int status;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) continue;
        perror("stsh: waitpid"); return 1;
    }
    return child_status(status,name);
}

void
sh_reap_background(void)
{
    int status;
    for (;;) {
        pid_t pid=waitpid(-1,&status,WNOHANG);
        if (pid<0 && errno==EINTR) continue;
        if (pid<=0) break;
        child_status(status,"background job");
    }
}

static void
exec_cmd(cmd_t *cmd, char **envp)
{
    char full[512];
    if (cmd->bootstrap) {
        char *av[]={STSH_PATH,"--state-v1",cmd->bootstrap,NULL};
        execve(STSH_PATH,av,envp);
        _exit(127);
    }
    const char *name = cmd->argv[0];
    if (!name) _exit(125);
    if (strchr(name, '/')) {
        execve(name, cmd->argv, envp);
    } else {
        const char *path = NULL;
        for (int i = 0; envp && envp[i]; i++)
            if (!strncmp(envp[i], "PATH=", 5)) { path = envp[i] + 5; break; }
        if (!path || !*path) path = "/bin:/usr/bin";
        const char *p = path;
        while (*p) {
            const char *e = strchr(p, ':');
            size_t len = e ? (size_t)(e - p) : strlen(p);
            if (len && len + strlen(name) + 2 < sizeof(full)) {
                memcpy(full, p, len); full[len] = '/';
                strcpy(full + len + 1, name);
                execve(full, cmd->argv, envp);
            }
            if (!e) break;
            p = e + 1;
        }
        if (snprintf(full, sizeof(full), "/apps/%s/%s", name, name) < (int)sizeof(full))
            execve(full, cmd->argv, envp);
    }
    _exit(127);
}

static int
pipeline(cmd_t *cmds, int n, char **envp, int background)
{
    int pipes[MAX_PIPELINE - 1][2], np = 0, launched = 0;
    pid_t pids[MAX_PIPELINE];
    if (n < 1 || n > MAX_PIPELINE) return 1;
    for (; np < n - 1; np++) {
        int p[2];
        if (pipe(p) < 0) { perror("stsh: pipe"); goto fail; }
        pipes[np][0] = private_fd(p[0]);
        pipes[np][1] = private_fd(p[1]);
        if (pipes[np][0] < 0 || pipes[np][1] < 0) {
            if (pipes[np][0] >= 0) close(pipes[np][0]);
            if (pipes[np][1] >= 0) close(pipes[np][1]);
            perror("stsh: pipe descriptors"); goto fail;
        }
    }
    fflush(NULL);
    for (int i = 0; i < n; i++) {
        int fds[3];
        if (redir_prepare(&cmds[i], fds, i ? pipes[i-1][0] : -1,
                          i < n-1 ? pipes[i][1] : -1) < 0) goto fail;
        if (command_bootstrap(&cmds[i])<0) { redir_close(fds); goto fail; }
        pid_t pid = fork();
        if (pid == 0) {
            if (redir_install(fds) < 0) _exit(125);
            redir_close(fds);
            for (int j = 0; j < np; j++) {
                close(pipes[j][0]); close(pipes[j][1]);
            }
            /* Noninteractive fresh interpreters remain in their caller's job.
             * Every interactive pipeline stage belongs to one foreground group. */
            if (g_jobcontrol && setpgid(0, i ? pids[0] : 0) < 0) _exit(125);
            if (g_jobcontrol) {
                struct sigaction sa;
                memset(&sa,0,sizeof sa); sa.sa_handler=SIG_DFL;
                if (sigaction(SIGTTOU,&sa,NULL)<0) _exit(125);
            }
            if (g_tty_fd>=0) close(g_tty_fd);
            exec_cmd(&cmds[i], envp);
        }
        redir_close(fds);
        free(cmds[i].bootstrap); cmds[i].bootstrap=NULL;
        if (pid < 0) { perror("stsh: fork"); goto fail; }
        pids[launched++] = pid;
        if (g_jobcontrol && setpgid(pid, pids[0]) < 0 && errno != EACCES && errno != ESRCH) {
            perror("stsh: setpgid"); goto fail;
        }
    }
    for (int j = 0; j < np; j++) {
        close(pipes[j][0]); close(pipes[j][1]);
    }
    if (background) return 0;
    int tty = g_jobcontrol && g_tty_fd>=0;
    pid_t oldfg = tty ? tcgetpgrp(g_tty_fd) : -1;
    if (tty && tcsetpgrp(g_tty_fd, pids[0]) < 0)
        perror("stsh: foreground");
    int rc = 1;
    for (int i = 0; i < n; i++)
        rc = sh_wait(pids[i], cmds[i].argv[0] ? cmds[i].argv[0] : "pipeline");
    if (oldfg >= 0 && tcsetpgrp(g_tty_fd, oldfg) < 0)
        perror("stsh: restore foreground");
    return rc;
fail:
    for (int j = 0; j < np; j++) {
        close(pipes[j][0]); close(pipes[j][1]);
    }
    /* A half-built pipeline may still be reading the terminal or running
     * forever because its consumer was never started. Terminate before wait. */
    for (int j = 0; j < launched; j++)
        if (kill(pids[j], SIGKILL) < 0 && errno != ESRCH) perror("stsh: kill");
    for (int j = 0; j < launched; j++) sh_wait(pids[j], "aborted pipeline");
    return 1;
}

void
run_pipeline(cmd_t *cmds, int n, char **envp, int *last_exit)
{
    *last_exit = pipeline(cmds, n, envp, 0);
}

int
run_pipeline_bg(cmd_t *cmds, int n, char **envp)
{
    return pipeline(cmds, n, envp, 1);
}
