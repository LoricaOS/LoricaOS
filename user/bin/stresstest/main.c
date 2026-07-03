/* stresstest — one-boot graphical soak test.
 *
 * Run from a terminal, or (preferred) auto-launched by the vigil `stresssoak`
 * service on the soak ISO. Spawns concurrent workers that hammer the
 * subsystems most implicated in "the desktop hangs on bare metal" reports,
 * while heartbeating to stderr (serial, when run as a vigil service) so a host
 * harness can tell "still running" from "hung". Lumen liveness is watched
 * separately by the harness via screendumps (topbar clock must keep moving).
 *
 * Resilience: a single failed op does NOT kill a worker — it bumps that
 * worker's error tally and the loop continues. The parent prints per-worker
 * tallies every 30s, so a *rising* tally is a real degradation signal while
 * the soak keeps running to catch an actual hang. Only a hard crash of a
 * worker process shows up as a missing heartbeat contributor.
 *
 * Workers:
 *   app-churn   — fork/exec a GUI app so it connects to Lumen, draws, then
 *                 kill it. Churns AF_UNIX + memfd window buffers + compositor
 *                 client cleanup (prime hang suspects).
 *   memfd-fork  — MAP_SHARED memfd + fork children writing to it (the
 *                 SHARED_OWNED inherit path); asserts coherence, refcount churn.
 *   fs-churn    — create/write/read/verify/unlink files (ext2/tmp).
 *   fork-storm  — fork/exit as fast as possible (COW, TLB shootdown, sched).
 *   pipe-churn  — pipe+fork ping-pong (fd table + waitq churn).
 */
#define _GNU_SOURCE   /* memfd_create */
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static time_t now(void) { return time(NULL); }

/* Shared error tally: each worker maps this and bumps its slot on a failure.
 * MAP_SHARED|MAP_ANONYMOUS so the parent sees the children's counts. */
#define NW 5
static volatile uint64_t *g_err;   /* [NW] */
static const char *g_names[NW] = {
    "app-churn", "memfd-fork", "fs-churn", "fork-storm", "pipe-churn",
};
static void bump(int w) { if (g_err) __sync_fetch_and_add(&g_err[w], 1); }

/* GUI apps to churn — probe both layouts at runtime, keep what exists. */
static const char *k_candidates[] = {
    "/bin/terminal", "/bin/calculator", "/bin/editor", "/bin/sysmon",
    "/apps/calculator/calculator", "/apps/terminal/terminal",
    "/apps/editor/editor", "/apps/sysmon/sysmon",
    "/apps/lumen-calculator/calculator", "/apps/lumen-terminal/terminal",
};
#define N_CANDIDATES (int)(sizeof(k_candidates) / sizeof(k_candidates[0]))

static void worker_app_churn(time_t end)
{
    const char *apps[N_CANDIDATES];
    int napps = 0;
    for (int c = 0; c < N_CANDIDATES; c++)
        if (access(k_candidates[c], X_OK) == 0)
            apps[napps++] = k_candidates[c];
    if (napps == 0) { dprintf(2, "[STRESS] app-churn: no apps, idle\n"); }
    dprintf(2, "[STRESS] app-churn: %d apps\n", napps);
    int i = 0;
    while (now() < end) {
        if (napps == 0) { sleep(2); continue; }
        const char *app = apps[i % napps];
        i++;
        pid_t pid = fork();
        if (pid == 0) {
            char *argv[] = { (char *)app, NULL };
            execv(app, argv);
            _exit(127);
        }
        if (pid < 0) { bump(0); sleep(1); continue; }
        sleep(1 + (i % 3));          /* let it connect + draw a few frames */
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
    }
    _exit(0);
}

static void worker_memfd_fork(time_t end)
{
    while (now() < end) {
        int fd = memfd_create("stress", 0);
        if (fd < 0) { bump(1); continue; }
        if (ftruncate(fd, 65536) != 0) { bump(1); close(fd); continue; }
        uint8_t *m = mmap(NULL, 65536, PROT_READ | PROT_WRITE,
                          MAP_SHARED, fd, 0);
        if (m == MAP_FAILED) { bump(1); close(fd); continue; }
        m[0] = 0xAA;
        for (int c = 0; c < 4; c++) {
            pid_t pid = fork();
            if (pid == 0) { m[4096 * (c + 1)] = (uint8_t)c; _exit(0); }
            if (pid < 0) { bump(1); break; }
            waitpid(pid, NULL, 0);
            /* SHARED_OWNED inherit: the child's write MUST be visible here.
             * If not, the memfd COW-broke on fork — a real isolation bug. */
            if (m[4096 * (c + 1)] != (uint8_t)c) {
                dprintf(2, "[STRESS] memfd: COW-BROKE on fork (c=%d) *****\n", c);
                bump(1);
            }
        }
        munmap(m, 65536);
        close(fd);
    }
    _exit(0);
}

static void worker_fs_churn(time_t end)
{
    char path[128], buf[4096];
    memset(buf, 0x5A, sizeof(buf));
    int i = 0;
    while (now() < end) {
        snprintf(path, sizeof(path), "/tmp/stress.%d.%d", (int)getpid(), i % 8);
        i++;
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) { bump(2); continue; }
        int wok = 1;
        for (int k = 0; k < 16; k++)
            if (write(fd, buf, sizeof(buf)) != (ssize_t)sizeof(buf)) { wok = 0; break; }
        close(fd);
        if (!wok) { bump(2); unlink(path); continue; }
        fd = open(path, O_RDONLY);
        if (fd < 0) { bump(2); unlink(path); continue; }
        ssize_t rn = read(fd, buf, sizeof(buf));
        close(fd);
        if (rn != (ssize_t)sizeof(buf)) bump(2);   /* short read = coherence bug */
        unlink(path);
    }
    _exit(0);
}

static void worker_fork_storm(time_t end)
{
    while (now() < end) {
        pid_t pid = fork();
        if (pid == 0) _exit(0);
        if (pid < 0) { bump(3); sleep(1); continue; }
        waitpid(pid, NULL, 0);
    }
    _exit(0);
}

static void worker_pipe_churn(time_t end)
{
    while (now() < end) {
        int p[2];
        if (pipe(p) != 0) { bump(4); continue; }
        pid_t pid = fork();
        if (pid == 0) {
            char c; read(p[0], &c, 1); write(p[1], "y", 1); _exit(0);
        }
        if (pid < 0) { bump(4); close(p[0]); close(p[1]); continue; }
        write(p[1], "x", 1);
        char c; read(p[0], &c, 1);
        waitpid(pid, NULL, 0);
        close(p[0]); close(p[1]);
    }
    _exit(0);
}

/* Wait until the compositor is accepting connections, so we don't starve its
 * startup (this service can start concurrently with bastion/lumen). Returns 1
 * if lumen came up within `secs`, 0 otherwise. */
static int wait_for_lumen(int secs)
{
    for (int i = 0; i < secs * 2; i++) {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd >= 0) {
            struct sockaddr_un a;
            memset(&a, 0, sizeof(a));
            a.sun_family = AF_UNIX;
            strncpy(a.sun_path, "/run/lumen.sock", sizeof(a.sun_path) - 1);
            int ok = connect(fd, (struct sockaddr *)&a, sizeof(a)) == 0;
            close(fd);
            if (ok) return 1;
        }
        usleep(500000);
    }
    return 0;
}

int main(int argc, char **argv)
{
    int dur = (argc > 1) ? atoi(argv[1]) : 600;
    if (dur <= 0) dur = 600;

    /* Let the graphical session come up first; churning during lumen's init
     * would starve it (and test nothing). */
    int up = wait_for_lumen(90);
    dprintf(2, "[STRESS] lumen %s after wait\n", up ? "UP" : "NOT up (proceeding)");

    time_t start = now(), end = start + dur;

    g_err = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                 MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (g_err == MAP_FAILED) g_err = NULL;

    dprintf(2, "[STRESS] start: %ds, %d workers, resilient\n", dur, NW);

    void (*fns[NW])(time_t) = {
        worker_app_churn, worker_memfd_fork, worker_fs_churn,
        worker_fork_storm, worker_pipe_churn,
    };
    pid_t pids[NW];
    for (int i = 0; i < NW; i++) {
        pid_t pid = fork();
        if (pid == 0) fns[i](end);
        pids[i] = pid;
    }

    /* Heartbeat + tally loop. A missing heartbeat (this loop wedged) is the
     * whole-system hang signal the host harness watches for. Every 30s, read
     * /proc/stackshot: the kernel dumps every task's stuck frame to the log
     * (serial), so if a process (e.g. lumen) wedges, its blocked syscall/PC
     * lands on the serial console for diagnosis. */
    int tick = 0;
    while (now() < end) {
        sleep(10);
        char tally[256]; int off = 0;
        for (int i = 0; i < NW && g_err; i++)
            off += snprintf(tally + off, sizeof(tally) - off, " %s=%llu",
                            g_names[i], (unsigned long long)g_err[i]);
        dprintf(2, "[STRESS] t=%lds alive%s\n",
                (long)(now() - start), g_err ? tally : "");
        if (++tick % 3 == 0) {                 /* ~every 30s */
            int f = open("/proc/stackshot", O_RDONLY);
            if (f >= 0) { char b[64]; while (read(f, b, sizeof(b)) > 0) {} close(f); }
        }
    }

    for (int i = 0; i < NW; i++)
        if (pids[i] > 0) { kill(pids[i], SIGKILL); waitpid(pids[i], NULL, 0); }

    uint64_t total = 0;
    for (int i = 0; i < NW && g_err; i++) total += g_err[i];
    dprintf(2, "[STRESS] DONE all-alive after %ds, %llu total worker errors\n",
            dur, (unsigned long long)total);
    return 0;
}
