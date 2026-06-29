/* contresume — regression test for kill(pid, SIGCONT) not resuming a process
 * stopped via kill(pid, SIGSTOP) (kernel/signal/signal.c signal_send_pid).
 *
 * Bug: signal_send_pid (the kill(pid>0,...) path) only woke a TASK_BLOCKED
 * target; a process in TASK_STOPPED (stopped by a directed kill SIGSTOP/SIGTSTP)
 * was left in the run-queue-less STOPPED state, so kill(pid,SIGCONT) set the
 * pending bit but NEVER ran the task to act on it — the process stayed stopped
 * forever. (signal_send_pgrp already woke STOPPED, so terminal Ctrl-Z+fg worked;
 * only directed kill(pid,...) was broken.) Fix: wake STOPPED||BLOCKED, matching
 * signal_send_pgrp.
 *
 * Repro: parent forks a child that increments a *shared-via-pipe* heartbeat by
 * writing a byte every ~200ms. Parent reads heartbeats, then kill(child,SIGSTOP)
 * — heartbeats must stop. Then kill(child,SIGCONT) — heartbeats must RESUME.
 * On the buggy kernel no byte arrives after SIGCONT and the read times out
 * (poll), so we print FAIL; on the fixed kernel a post-cont byte arrives → OK.
 *
 * Prints exactly one summary line:
 *   CONTRESUME_OK                — child resumed after SIGCONT
 *   CONTRESUME_FAIL: <why>       — setup error OR no resume (the bug)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>
#include <sys/wait.h>

static void fail(const char *why)
{
    printf("CONTRESUME_FAIL: %s\n", why);
    fflush(stdout);
    _exit(1);
}

/* Read one heartbeat byte from fd within `ms` (poll). Returns 1 if a byte
 * arrived, 0 on timeout, -1 on error. */
static int beat_within(int fd, int ms)
{
    struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };
    int r = poll(&pfd, 1, ms);
    if (r < 0) return -1;
    if (r == 0) return 0;
    char c;
    int n = (int)read(fd, &c, 1);
    return n == 1 ? 1 : -1;
}

int main(void)
{
    int hb[2];
    if (pipe(hb) < 0) fail("pipe");

    pid_t child = fork();
    if (child < 0) fail("fork");

    if (child == 0) {
        /* Child: emit a heartbeat byte forever. */
        close(hb[0]);
        for (;;) {
            char c = '.';
            if (write(hb[1], &c, 1) != 1) _exit(0);  /* parent gone */
            usleep(200 * 1000);
        }
        _exit(0);
    }

    /* Parent. */
    close(hb[1]);

    /* Confirm the child is alive and beating. */
    if (beat_within(hb[0], 3000) != 1) fail("no initial heartbeat");

    /* Stop the child via a DIRECTED kill (the broken path). */
    if (kill(child, SIGSTOP) < 0) fail("kill SIGSTOP");

    /* Drain any in-flight byte, then confirm heartbeats actually stopped:
     * over a 1.5s window we must see NO new byte once the pipe is drained. */
    while (beat_within(hb[0], 50) == 1) { /* drain backlog */ }
    if (beat_within(hb[0], 1200) == 1)
        fail("child still beating while stopped");

    /* Continue the child via a DIRECTED kill — THE case the bug broke. */
    if (kill(child, SIGCONT) < 0) fail("kill SIGCONT");

    /* A heartbeat must resume within a couple seconds. On the buggy kernel the
     * child never runs again, so this times out. */
    int r = beat_within(hb[0], 3000);
    if (r == 1) {
        printf("CONTRESUME_OK\n");
        fflush(stdout);
        kill(child, SIGKILL);
        return 0;
    }
    /* No resume — the bug. */
    kill(child, SIGKILL);
    fail("no heartbeat after SIGCONT (stopped task not resumed)");
    return 1;
}
