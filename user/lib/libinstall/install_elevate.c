/* install_elevate.c — obtain a sudo-style admin session for the installer.
 *
 * Raw whole-disk writes (li_blkdev_io → sys_blkdev_io, syscall 511) require
 * CAP_KIND_DISK_ADMIN, which the kernel grants only to a process whose
 * admin_session flag is set (see kernel/cap/cap_policy.c). admin_session is
 * handed out by exactly one trusted authenticator: /bin/login -elevate, which
 * verifies the SEPARATE admin credential (/etc/aegis/admin) and asks the
 * kernel to elevate ITS DIRECT PARENT. So the installer fork/exec's
 * `login -elevate`; on success the kernel elevates the installer (the forking
 * process) and re-derives its caps, after which DISK_ADMIN writes succeed.
 */
#include "libinstall.h"

#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>

extern char **environ;

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
                dup2(pfd[0], 0);
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
        /* Write the password followed by a newline; login's readline stops at
         * the newline. Best-effort: a short write still lets login fail
         * cleanly and we report -1 below. */
        size_t len = 0;
        while (admin_pw[len]) len++;
        if (len)
            (void)write(pfd[1], admin_pw, len);
        (void)write(pfd[1], "\n", 1);
        close(pfd[1]);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0)
        return -1;

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        return 0;
    return -1;
}
