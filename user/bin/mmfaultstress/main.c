/* mmfaultstress — RED→GREEN reproducer for the concurrent lazy-fault double-map
 * panic fixed in 0f83b4c (mm: serialize lazy demand-fault).
 *
 * THE BUG: mm_populate_fault did check-absent / pmm_alloc_page / vmm_map_user_page
 * non-atomically. Two threads of one process (shared PML4 + shared vma_table)
 * faulting the SAME lazy anonymous mmap page on different CPUs both pass the
 * present-check, both alloc a frame, and both vmm_map_user_page — the second hits
 * vmm_map_user_page's double-map invariant and panic_halts the kernel
 * ("[VMM] FAIL: vmm_map_user_page double-map").
 *
 * This probe maximizes that race: it mmaps a lazy anon region, spawns NTHREADS
 * threads that spin-barrier together and then ALL first-touch the SAME pages in
 * lockstep (every thread writes byte 0 of page p at the same time), so many CPUs
 * are inside mm_populate_fault for one page simultaneously. After each round it
 * munmaps + re-mmaps so the pages are lazy again. Under -smp 4 smp_sched on the
 * BASELINE kernel this panics within a few rounds; with 0f83b4c it completes all
 * rounds and prints "[MMFAULT] all rounds clean".
 *
 * Gated on the `mm_fault_stress` cmdline token (mirrors smpstress's smp_stress
 * gate) so the service ships inert in every rootfs. Writes to /dev/console →
 * serial (vigil oneshot stdio is not wired to the console).
 *
 * ponytail: fixed NTHREADS/ROUNDS/PAGES — it's a probe, tune by rebuild.
 */
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <sys/mman.h>

/* NTHREADS == the VM's CPU count (-smp 4): a hard-spin barrier with MORE threads
 * than CPUs over-subscribes — on-CPU threads burn their quantum spinning for
 * off-CPU threads that only run at 100Hz preemption granularity, so rounds crawl
 * and it looks hung. Matching CPU count gives true simultaneity (all CPUs hit the
 * SAME page at once — the double-map race window) with no scheduling churn. */
#define NTHREADS 4
#define ROUNDS   200
#define PAGES    32
#define PAGESZ   4096

static int g_con = 2;

static void con(const char *s) { write(g_con, s, strlen(s)); }
static void con_num(int n)
{
    char b[12]; int i = 10; b[11] = 0;
    if (n == 0) { con("0"); return; }
    while (n > 0 && i >= 0) { b[i--] = (char)('0' + n % 10); n /= 10; }
    con(&b[i + 1]);
}

static int mm_fault_stress_on_cmdline(void)
{
    int fd = open("/proc/cmdline", O_RDONLY);
    if (fd < 0) return 0;
    char buf[512];
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = 0;
    return strstr(buf, "mm_fault_stress") != 0;
}

/* Shared state for one round. */
static volatile uint8_t *g_region;      /* the lazy anon mapping under test */
/* g_go starts at -1 (NOT 0): round numbers are 0..ROUNDS-1, so a 0-init would
 * collide with round 0 — a worker could pass barrier-arrival before main resets
 * g_arrived for round 0, corrupting the count → barrier livelock. -1 means "no
 * round yet": workers wait until main publishes the first real round. */
static volatile int      g_go = -1;     /* round-start gate */
static volatile int      g_arrived;     /* threads parked at the gate */
static volatile int      g_done;        /* threads finished the round */
static volatile int      g_stop;        /* tell threads to exit */

static void *
worker(void *arg)
{
    (void)arg;
    int last_round = -1;
    for (;;) {
        /* Spin until the main thread opens the gate for a new round (or stop). */
        int g;
        while ((g = g_go) == last_round && !g_stop)
            __asm__ volatile("pause");
        if (g_stop)
            return 0;
        last_round = g;

        /* Barrier: announce arrival, then spin until ALL threads have arrived,
         * so every thread leaves the barrier in the same instant — all CPUs hit
         * the race windows concurrently. */
        __atomic_add_fetch(&g_arrived, 1, __ATOMIC_SEQ_CST);
        while (__atomic_load_n(&g_arrived, __ATOMIC_SEQ_CST) < NTHREADS)
            __asm__ volatile("pause");

        /* RACE 1 (#5, concurrent mmap SYSCALLS): each thread does its OWN
         * mmap+touch+munmap. Siblings share mmap_base (copied at clone) so they
         * tend to select the SAME/overlapping VAs at once → on baseline,
         * concurrent vma_insert array-shifts corrupt the shared table and/or
         * produce overlapping VMAs → double-map panic when the regions overlap.
         * The vma_table lock + overlap-reject reservation must serialize them. */
        void *mine = mmap(0, (size_t)PAGES * PAGESZ, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mine != MAP_FAILED) {
            for (int p = 0; p < PAGES; p++)
                ((volatile uint8_t *)mine)[(uint64_t)p * PAGESZ] = (uint8_t)(p + 1);
            munmap(mine, (size_t)PAGES * PAGESZ);
        }

        /* RACE 2 (lazy-fault double-map, b97fa12): all threads first-touch the
         * SAME shared lazy region in lockstep — maximal same-page concurrent
         * populate. */
        for (int p = 0; p < PAGES; p++)
            g_region[(uint64_t)p * PAGESZ] = (uint8_t)(p + 1);

        __atomic_add_fetch(&g_done, 1, __ATOMIC_SEQ_CST);
    }
}

int
main(void)
{
    if (!mm_fault_stress_on_cmdline())
        return 0;

    int c = open("/dev/console", O_WRONLY);
    if (c >= 0) g_con = c;

    con("[MMFAULT] start (threads="); con_num(NTHREADS);
    con(" rounds="); con_num(ROUNDS); con(" pages="); con_num(PAGES); con(")\n");

    pthread_t th[NTHREADS];
    for (int i = 0; i < NTHREADS; i++) {
        if (pthread_create(&th[i], 0, worker, 0) != 0) {
            con("[MMFAULT] pthread_create failed\n");
            return 1;
        }
    }
    for (int r = 0; r < ROUNDS; r++) {
        /* Fresh lazy anon region each round: MAP_ANONYMOUS|MAP_PRIVATE is lazy
         * (no present PTEs until first touch), so every round re-arms the race. */
        void *m = mmap(0, (size_t)PAGES * PAGESZ, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (m == MAP_FAILED) { con("[MMFAULT] mmap failed\n"); break; }
        g_region = (volatile uint8_t *)m;
        g_arrived = 0;
        g_done = 0;

        /* Open the gate (publish region first via the SEQ_CST store on g_go). */
        __atomic_store_n(&g_go, r, __ATOMIC_SEQ_CST);

        /* Wait for all threads to finish this round's sweep. */
        while (__atomic_load_n(&g_done, __ATOMIC_SEQ_CST) < NTHREADS)
            __asm__ volatile("pause");

        munmap(m, (size_t)PAGES * PAGESZ);

        if (((r + 1) % 50) == 0) {
            con("[MMFAULT] round "); con_num(r + 1); con("/");
            con_num(ROUNDS); con("\n");
        }
    }

    g_stop = 1;
    __atomic_store_n(&g_go, ROUNDS + 1, __ATOMIC_SEQ_CST);
    for (int i = 0; i < NTHREADS; i++)
        pthread_join(th[i], 0);

    con("[MMFAULT] all rounds clean\n");
    return 0;
}
