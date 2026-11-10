/* vforkstress — exercises the CLONE_VFORK path (musl posix_spawn) under
 * smp_sched, to verify the vfork wait in sys_clone (now sched_block_locked).
 *
 * No userland used posix_spawn before, so the vfork path was otherwise
 * unexercised by the test infra. musl's posix_spawn does clone(CLONE_VM|
 * CLONE_VFORK) then execve in the child; the parent blocks in the kernel's
 * vfork wait until the child execs (or dies). This loops it hard so a
 * lost-wakeup in that wait would hang (no OK line → harness timeout).
 *
 * Two stress phases plus focused launch regressions:
 *   1. SPAWN SUCCESS — posix_spawn /bin/true REPS times (child execs → wakes
 *      the vfork parent). The common path.
 *   2. SPAWN FAIL-BEFORE-EXEC — posix_spawn with a file_action that dup2's a
 *      bad fd, so the child _exit()s BEFORE execve → the vfork-die-before-exec
 *      wake path (sched_exit→SIGCHLD), the rare lost-wakeup window the fix
 *      targets. The Aegis musl build reports pre-exec errors through shared
 *      memory, not a write requiring a capability in the independent child.
 *   3. Missing executable (ENOENT), addopen (ENOCAP, not a hang), and a valid
 *      target exiting 127 (successful spawn, not a pre-exec error).
 * Every case checks the exact result and that all children have been reaped.
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
#include <errno.h>

#define REPS 400
/* Aegis currently exposes capability denial as EPERM in the musl ABI. */
#ifndef ENOCAP
#define ENOCAP EPERM
#endif

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

static pid_t reap(pid_t pid, int *st, int flags)
{
    pid_t r;
    do r = waitpid(pid, st, flags); while (r < 0 && errno == EINTR);
    return r;
}

static int check_spawn(const char *name, const char *path, char *const argv[],
                       const posix_spawn_file_actions_t *fa,
                       int expected_error, int expected_exit)
{
    pid_t pid = -1;
    int r = posix_spawn(&pid, path, fa, 0, argv, environ);
    int good = r == expected_error;
    if (r == 0) {
        int st = 0;
        if (pid <= 0 || reap(pid, &st, 0) != pid ||
            !WIFEXITED(st) || WEXITSTATUS(st) != expected_exit)
            good = 0;
    } else if (pid != -1) {
        good = 0; /* Failure must not publish *res. */
    }
    /* This test owns all its children. A failure-return child must already be
     * reaped by libc, not just dead with status 127. */
    int st;
    pid_t extra = reap(-1, &st, WNOHANG);
    if (extra != -1 || errno != ECHILD) good = 0;
    while (extra > 0) extra = reap(-1, &st, WNOHANG);
    if (!good) {
        char b[160];
        snprintf(b, sizeof(b), "[VFORKSTRESS] FAIL %s rc=%d expected=%d pid=%d\n",
                 name, r, expected_error, (int)pid);
        con(b);
    }
    return good;
}

int main(int argc, char **argv)
{
    /* A real, successfully exec'd target with exit status 127. No shell or
     * pre-exec diagnostics can stand in for this distinction. */
    if (argc == 2 && strcmp(argv[1], "--spawn-exit127") == 0) return 127;
    if (!vfork_stress_on_cmdline())
        return 0;

    int c = open("/dev/console", O_WRONLY);
    if (c >= 0) { dup2(c, 1); if (c != 1) close(c); }

    con("[VFORKSTRESS] start\n");

    char *const true_argv[] = { (char *)"/bin/true", 0 };
    int ok = 0, failed = 0;
    int errors = 0;

    /* Phase 1: posix_spawn success path (child execs → wakes vfork parent). */
    for (int i = 0; i < REPS; i++) {
        if (check_spawn("success", "/bin/true", true_argv, 0, 0, 0)) ok++;
        else errors++;
    }

    /* Phase 2: child dies BEFORE exec (file_action dup2 of a bad fd fails in the
     * child → it _exit()s pre-execve → the vfork-die-before-exec wake path). */
    close(999); /* Known closed before any spawn; adddup2 accepts positive fds. */
    for (int i = 0; i < REPS; i++) {
        posix_spawn_file_actions_t fa;
        if (posix_spawn_file_actions_init(&fa)) return 1;
        if (posix_spawn_file_actions_adddup2(&fa, 999, 1)) {
            posix_spawn_file_actions_destroy(&fa);
            return 1;
        }
        if (check_spawn("bad dup", "/bin/true", true_argv, &fa, EBADF, 0)) failed++;
        else errors++;
        posix_spawn_file_actions_destroy(&fa);
    }

    if (!check_spawn("missing executable", "/__vforkstress_missing__/exec",
                     true_argv, 0, ENOENT, 0)) errors++;

    posix_spawn_file_actions_t fa;
    if (posix_spawn_file_actions_init(&fa)) return 1;
    if (posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0)) {
        posix_spawn_file_actions_destroy(&fa);
        return 1;
    }
    if (!check_spawn("unprivileged addopen", "/bin/true", true_argv,
                     &fa, ENOCAP, 0)) errors++;
    posix_spawn_file_actions_destroy(&fa);

    char *const exit_argv[] = {
        (char *)"/bin/vforkstress", (char *)"--spawn-exit127", 0
    };
    if (!check_spawn("target exit127", exit_argv[0], exit_argv, 0, 0, 127)) errors++;

    char b[64];
    if (errors) {
        snprintf(b, sizeof(b), "[VFORKSTRESS] FAIL errors=%d\n", errors);
        con(b);
        return 1;
    }
    con("[VFORKSTRESS] spawn regressions OK (ENOENT/ENOCAP/exit127/reaped)\n");
    snprintf(b, sizeof(b), "[VFORKSTRESS] OK %d/%d\n", ok, failed);
    con(b);
    return 0;
}
