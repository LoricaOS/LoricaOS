/* futexstress — reproducer for the futex slot-recycle lost-wakeup bug
 * (kernel/syscall/futex.c).  See research/net/repros/futexstress.c for the
 * original analysis (net agent).
 *
 * Bug: futex_wake_addr FREES the waiter's pool slot (in_use=0) when waking it.
 * The WAIT side detects its wake by checking `!w->in_use` after sched_block().
 * Race: waker wakes W + frees slot i; before W rechecks, another thread's
 * FUTEX_WAIT grabs the lowest-free slot i and blocks in it; W then sees
 * in_use=1 (the new occupant's), concludes "not woken", and re-blocks FOREVER
 * with its wake already consumed. Underlies all contended musl pthread sync.
 *
 * This binary runs two high-pressure phases to widen that 3-way window:
 *   1. MUTEX HAMMER — NTHREADS threads pound one mutex (tiny critical section
 *      → maximal FUTEX_WAIT/WAKE churn + slot recycle).
 *   2. CONDVAR BROADCAST — NTHREADS threads pthread_cond_wait on one condvar;
 *      main broadcasts repeatedly. Each broadcast FUTEX_WAKEs ALL waiters at
 *      once → many slots freed simultaneously → they re-contend the mutex →
 *      maximal simultaneous recycle pressure (net's suggested amplifier).
 *
 * A lost-woken thread hangs in pthread_mutex_lock / pthread_cond_wait →
 * pthread_join never returns → no OK line → harness times out (= the bug).
 * On a correct kernel every thread finishes and the totals are exact.
 *
 * Prints exactly one summary line per phase:
 *   FUTEXSTRESS_MUTEX_OK <total>
 *   FUTEXSTRESS_COND_OK <rounds>
 *   FUTEXSTRESS_OK            — both phases completed
 *
 * Gated on the `futex_stress` cmdline token (like smpstress): inert in
 * production / the boot oracle, so the service can ship in every rootfs.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>

#define NTHREADS 32
#define ITERS    50000
#define BCAST_ROUNDS 400

static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static volatile long counter;

/* ---- phase 1: mutex hammer ---- */
static void *mtx_worker(void *arg)
{
    (void)arg;
    for (int i = 0; i < ITERS; i++) {
        pthread_mutex_lock(&mtx);
        counter++;
        pthread_mutex_unlock(&mtx);
    }
    return 0;
}

/* ---- phase 2: condvar broadcast ---- */
static pthread_mutex_t cmtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  cond = PTHREAD_COND_INITIALIZER;
static volatile long   gen;        /* broadcast generation */
static volatile int    stop;
static volatile int    parked;     /* threads currently waiting */

static void *cond_worker(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&cmtx);
    while (!stop) {
        long g = gen;
        parked++;
        while (g == gen && !stop)
            pthread_cond_wait(&cond, &cmtx);   /* FUTEX_WAIT on cond futex */
        parked--;
    }
    pthread_mutex_unlock(&cmtx);
    return 0;
}

static int
futex_stress_on_cmdline(void)
{
    int fd = open("/proc/cmdline", O_RDONLY);
    if (fd < 0) return 0;
    char buf[512];
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = 0;
    return strstr(buf, "futex_stress") != 0;
}

static void con(const char *s) { write(1, s, strlen(s)); }

int main(void)
{
    if (!futex_stress_on_cmdline())
        return 0;

    /* console for serial visibility (vigil oneshot stdio isn't wired). */
    int c = open("/dev/console", O_WRONLY);
    if (c >= 0) { dup2(c, 1); if (c != 1) close(c); }

    con("[FUTEXSTRESS] start\n");

    /* Phase 1: mutex hammer. */
    {
        pthread_t th[NTHREADS];
        int made = 0;
        for (int i = 0; i < NTHREADS; i++)
            if (pthread_create(&th[i], 0, mtx_worker, 0) == 0) made++;
        for (int i = 0; i < made; i++)
            pthread_join(th[i], 0);   /* a lost-woken thread hangs here */
        char b[64];
        long expect = (long)made * ITERS;
        snprintf(b, sizeof(b), "[FUTEXSTRESS] MUTEX_OK %ld/%ld\n", counter, expect);
        con(b);
    }

    /* Phase 2: condvar broadcast churn. */
    {
        pthread_t th[NTHREADS];
        int made = 0;
        for (int i = 0; i < NTHREADS; i++)
            if (pthread_create(&th[i], 0, cond_worker, 0) == 0) made++;
        for (int r = 0; r < BCAST_ROUNDS; r++) {
            pthread_mutex_lock(&cmtx);
            gen++;
            pthread_cond_broadcast(&cond);   /* FUTEX_WAKE all → mass slot free */
            pthread_mutex_unlock(&cmtx);
            sched_yield();
        }
        pthread_mutex_lock(&cmtx);
        stop = 1;
        pthread_cond_broadcast(&cond);
        pthread_mutex_unlock(&cmtx);
        for (int i = 0; i < made; i++)
            pthread_join(th[i], 0);          /* a lost-woken cond waiter hangs */
        char b[64];
        snprintf(b, sizeof(b), "[FUTEXSTRESS] COND_OK %d\n", BCAST_ROUNDS);
        con(b);
    }

    con("[FUTEXSTRESS] OK\n");
    return 0;
}
