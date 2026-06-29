/* vforkstress — exercises the CLONE_VFORK path (musl posix_spawn) under
 * smp_sched, to verify the vfork wait in sys_clone (now sched_block_locked).
 *
 * No userland used posix_spawn before, so the vfork path was otherwise
 * unexercised by the test infra. musl's posix_spawn does clone(CLONE_VM|
 * CLONE_VFORK) then execve in the child; the parent blocks in the kernel's
 * vfork wait until the child execs (or dies). This loops it hard so a
 * lost-wakeup in that wait would hang (no OK line → harness timeout).
 *
 * Two phases:
 *   1. SPAWN SUCCESS — posix_spawn /bin/true REPS times (child execs → wakes
 *      the vfork parent). The common path.
 *   2. SPAWN FAIL-BEFORE-EXEC — posix_spawn with a file_action that dup2's a
 *      bad fd, so the child _exit()s BEFORE execve → the vfork-die-before-exec
 *      wake path (sched_exit→SIGCHLD), the rare lost-wakeup window the fix
 *      targets. posix_spawn reports the child's failure via its status pipe.
 *
 * Gated on the `vfork_stress` cmdline token (oracle-safe, like smpstress).
 * Prints:  [VFORKSTRESS] OK <succeeded>/<failed>   on completion (a hang = bug).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <spawn.h>
#include <sys/wait.h>

#define REPS 400

extern char **environ;

static int vfork_stress_on_cmdline(void)
{
    int fd = open("/proc/cmdline", O_RDONLY);
    if (fd < 0) return 0;
    char buf[512];
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = 0;
    return strstr(buf, "vfork_stress") != 0;
}

static void con(const char *s) { write(1, s, strlen(s)); }

int main(void)
{
    if (!vfork_stress_on_cmdline())
        return 0;

    int c = open("/dev/console", O_WRONLY);
    if (c >= 0) { dup2(c, 1); if (c != 1) close(c); }

    con("[VFORKSTRESS] start\n");

    char *const argv[]  = { (char *)"/bin/true", 0 };
    int ok = 0, failed = 0;

    /* Phase 1: posix_spawn success path (child execs → wakes vfork parent). */
    for (int i = 0; i < REPS; i++) {
        pid_t pid;
        if (posix_spawn(&pid, "/bin/true", 0, 0, argv, environ) == 0) {
            int st = 0;
            waitpid(pid, &st, 0);
            ok++;
        } else {
            failed++;
        }
    }

    /* Phase 2: child dies BEFORE exec (file_action dup2 of a bad fd fails in the
     * child → it _exit()s pre-execve → the vfork-die-before-exec wake path). */
    for (int i = 0; i < REPS; i++) {
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_adddup2(&fa, 999 /* bad fd */, 1);
        pid_t pid;
        int r = posix_spawn(&pid, "/bin/true", &fa, 0, argv, environ);
        posix_spawn_file_actions_destroy(&fa);
        if (r == 0) {
            int st = 0;
            waitpid(pid, &st, 0);
        }
        /* Either way the vfork parent must have been woken (no hang). */
        failed++;
    }

    char b[64];
    snprintf(b, sizeof(b), "[VFORKSTRESS] OK %d/%d\n", ok, failed);
    con(b);
    return 0;
}
