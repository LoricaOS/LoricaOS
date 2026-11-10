/* install_elevate.c — per-executable administrator entry.
 *
 * New callers reexec fixed-target login --admin-exec in place so the verified
 * admin principal can satisfy existing owner/DAC rules even for a nonzero user.
 * Legacy install_elevate is retained for source compatibility only: it elevates
 * the parent without changing identity, and is not the migration entry path.
 */
#include "install_elevate.h"
#include "admin_targets.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/syscall.h>

extern char **environ;

int install_admin_ready(const char *fixed_target)
{
    struct { uint32_t kind, rights; } caps[64];
    uint32_t required = admin_target_caps(fixed_target), present = 0;
    if (!required) return -1;
    long active = syscall(519);
    if (active < 0 || active > 1) return -1;
    if (!active || getuid() != 0 || getgid() != 0) return 0;
    long n = syscall(362, 0L, caps, sizeof(caps));
    if (n != 64) return -1;
    for (long i = 0; i < n; i++) {
        if (caps[i].kind > 23 || (caps[i].rights & ~7u) ||
            (!caps[i].kind && caps[i].rights))
            return -1; /* malformed/unknown capability ABI */
        /* WRITE is required for all targets; AUTH also needs credential READ. */
        uint32_t rights = caps[i].kind == 4 ? 3u : 2u;
        if (caps[i].kind < 32 && (caps[i].rights & rights) == rights)
            present |= 1u << caps[i].kind;
    }
    return (present & required) == required;
}

int install_admin_enter(const char *fixed_target, int *argc, char ***argv,
                        const char *admin_pw)
{
    if (!argc || !argv || !*argv || *argc < 1) return -1;
    int marked = *argc > 1 && !strcmp((*argv)[1], ADMIN_READY_MARKER);
    int ready = install_admin_ready(fixed_target);
    if (ready < 0 || (marked && !ready)) return -1;
    /* adminpw re-proves the credential on ordinary entry even if already
     * elevated. The marker is only a UX loop guard: live authority is checked
     * above regardless, and an already-authorized caller can forge the marker. */
    if (ready && (strcmp(fixed_target, "/bin/adminpw") || marked)) {
        if (marked) {
            for (int i = 1; i < *argc; i++) (*argv)[i] = (*argv)[i + 1];
            --*argc;
        }
        return 0;
    }
    if (*argc > 128) return -1;
    char *av[138], fd_arg[24];
    int count = 0, pfd[2] = {-1, -1};
    av[count++] = "login";
    av[count++] = "--admin-exec";
    av[count++] = (char *)fixed_target;
    if (admin_pw) {
        size_t len = strlen(admin_pw);
        /* Fits in an empty pipe without a concurrent writer or a fork. */
        if (!len || len > 127 || strchr(admin_pw, '\n') ||
            strchr(admin_pw, '\r') || pipe(pfd) != 0)
            return -1;
        if (pfd[0] < 3 || pfd[0] > 255) {
            close(pfd[0]); close(pfd[1]); return -1;
        }
        if (write(pfd[1], admin_pw, len) != (ssize_t)len ||
            write(pfd[1], "\n", 1) != 1) {
            close(pfd[0]); close(pfd[1]); return -1;
        }
        if (close(pfd[1]) != 0) { close(pfd[0]); return -1; }
        snprintf(fd_arg, sizeof(fd_arg), "%d", pfd[0]);
        av[count++] = "--password-fd";
        av[count++] = fd_arg;
    }
    av[count++] = "--";
    for (int i = 1; i < *argc; i++) av[count++] = (*argv)[i];
    av[count] = NULL;
    execve("/bin/login", av, environ);
    if (pfd[0] >= 0) close(pfd[0]);
    return -1;
}

int install_elevate(const char *admin_pw)
{
    int pfd[2];
    int use_pipe = (admin_pw != NULL);

    if (use_pipe) {
        if (pipe(pfd) < 0)
            return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        if (use_pipe) { close(pfd[0]); close(pfd[1]); }
        return -1;
    }

    if (pid == 0) {
        /* Child: become /bin/login -elevate. */
        if (use_pipe) {
            /* Feed the admin password on stdin so login reads it from the
             * pipe instead of a terminal (no tty in the GUI case). */
            close(pfd[1]);
            if (pfd[0] != 0) {
                if (dup2(pfd[0], 0) < 0)
                    _exit(127);
                close(pfd[0]);
            }
        }
        /* admin_pw == NULL: leave stdin alone so login prompts on the shared
         * tty (text installer). */
        char *av[] = { "login", "-elevate", (char *)0 };
        execve("/bin/login", av, environ);
        _exit(127); /* exec failed */
    }

    /* Parent (the installer). */
    if (use_pipe) {
        close(pfd[0]);
        /* A partial password must never count as a successful authorization. */
        size_t len = 0;
        while (admin_pw[len]) len++;
        size_t sent = 0;
        int failed = 0;
        while (sent < len) {
            ssize_t n = write(pfd[1], admin_pw + sent, len - sent);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { failed = 1; break; }
            sent += (size_t)n;
        }
        ssize_t n;
        do { n = failed ? -1 : write(pfd[1], "\n", 1); }
        while (n < 0 && !failed && errno == EINTR);
        if (n != 1) failed = 1;
        close(pfd[1]);
        if (failed) {
            while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
            return -1;
        }
    }

    int status = 0;
    pid_t waited;
    do { waited = waitpid(pid, &status, 0); }
    while (waited < 0 && errno == EINTR);
    if (waited != pid)
        return -1;

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        return 0;
    return -1;
}
