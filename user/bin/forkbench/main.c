/* forkbench — measure fork() and fork+exec latency over a large address space.
 *
 * The COW-fork win scales with the number of resident writable pages: eager
 * fork memcpy's every one at fork() time; COW fork only flips PTEs and copies
 * lazily on write. A text-mode boot's init forks are tiny (page-table-walk
 * bound), so they don't show the marquee. This builds a deliberately large
 * resident anonymous region, then times:
 *   (1) fork()+immediate child _exit  — pure duplication cost.
 *   (2) fork()+child execve(/bin/true) — the fork+exec pattern (child discards
 *       the clone immediately; the case COW was historically feared to regress).
 *
 * Pair with the kernel `perfbench_mm` flag to also see [PERFMM] dup-cycle lines.
 * Run eager vs `cow_fork` and compare. Gated on the `forkbench` cmdline token
 * (oracle-safe, inert otherwise — exactly like smpstress).
 *
 * Writes straight to /dev/console (→ serial): vigil does not wire a oneshot's
 * stdio to the console.
 *
 * ponytail: fixed sizes/iterations — tune via rebuild, it's a probe.
 */
#include <unistd.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>
#include <stdint.h>

#define REGION_MB 64
#define ITERS     20

static int g_con = 2;

static void con(const char *s) { write(g_con, s, strlen(s)); }

static void con_num(uint64_t n)
{
    char b[24]; int i = 22; b[23] = 0;
    if (n == 0) { con("0"); return; }
    while (n > 0 && i >= 0) { b[i--] = (char)('0' + n % 10); n /= 10; }
    con(&b[i + 1]);
}

static int forkbench_on_cmdline(void)
{
    int fd = open("/proc/cmdline", O_RDONLY);
    if (fd < 0) return 0;
    char buf[512];
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = 0;
    return strstr(buf, "forkbench") != 0;
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

int main(void)
{
    if (!forkbench_on_cmdline())
        return 0;

    int c = open("/dev/console", O_WRONLY);
    if (c >= 0) g_con = c;

    con("[FORKBENCH] start region=");
    con_num(REGION_MB); con("MB iters="); con_num(ITERS); con("\n");

    /* Build a large RESIDENT writable anonymous region: mmap then touch every
     * page so they are all present+writable in this address space. */
    size_t len = (size_t)REGION_MB * 1024 * 1024;
    unsigned char *p = mmap(0, len, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == (void *)-1) { con("[FORKBENCH] mmap failed\n"); return 1; }
    /* Touch every page: each first-touch is a lazy-anon fault → kernel allocs
     * + ZEROES the frame (vmm_zero_page). Times the anon-fault populate path. */
    uint64_t tt0 = now_ns();
    for (size_t i = 0; i < len; i += 4096)
        p[i] = (unsigned char)(i >> 12);
    uint64_t tt = now_ns() - tt0;
    con("[FORKBENCH] touch-fault "); con_num(REGION_MB); con("MB avg_ns/pg=");
    con_num(tt / (len / 4096)); con(" total_us="); con_num(tt / 1000); con("\n");

    /* (1) fork-only: pure address-space duplication cost. */
    uint64_t total = 0, worst = 0;
    for (int it = 0; it < ITERS; it++) {
        uint64_t t0 = now_ns();
        pid_t pid = fork();
        if (pid == 0) _exit(0);
        if (pid > 0) {
            uint64_t dt = now_ns() - t0;   /* fork() return time in parent */
            int st; waitpid(pid, &st, 0);
            total += dt;
            if (dt > worst) worst = dt;
        }
    }
    con("[FORKBENCH] fork-only avg_us="); con_num((total / ITERS) / 1000);
    con(" worst_us="); con_num(worst / 1000); con("\n");

    /* (2) fork+exec: child immediately execs /bin/true and discards the clone. */
    static const char *child = "/bin/true";
    char *const argv[] = { (char *)child, 0 };
    total = 0; worst = 0;
    for (int it = 0; it < ITERS; it++) {
        uint64_t t0 = now_ns();
        pid_t pid = fork();
        if (pid == 0) { execve(child, argv, 0); _exit(127); }
        if (pid > 0) {
            uint64_t dt = now_ns() - t0;
            int st; waitpid(pid, &st, 0);
            total += dt;
            if (dt > worst) worst = dt;
        }
    }
    con("[FORKBENCH] fork+exec avg_us="); con_num((total / ITERS) / 1000);
    con(" worst_us="); con_num(worst / 1000); con("\n");

    /* (3) file-mmap latency: time mmap() of a large file (the dynamic linker
     * libc.so). Eager file-backed mmap copies the whole file at mmap() time;
     * lazy (cmdline `lazyfile`) returns immediately and reads pages on fault.
     * This isolates the deferral — the win for any process that maps a library
     * but executes only part of it. Reported in ns (sub-us with lazyfile). */
    int ffd = open("/lib/libc.so", O_RDONLY);
    if (ffd >= 0) {
        struct stat st;
        if (fstat(ffd, &st) == 0 && st.st_size > 0) {
            size_t flen = ((size_t)st.st_size + 4095) & ~(size_t)4095;
            uint64_t mt = 0, mw = 0;
            for (int it = 0; it < ITERS; it++) {
                uint64_t t0 = now_ns();
                void *m = mmap(0, flen, PROT_READ, MAP_PRIVATE, ffd, 0);
                uint64_t dt = now_ns() - t0;
                mt += dt;
                if (dt > mw) mw = dt;
                if (m != (void *)-1) munmap(m, flen);
            }
            con("[FORKBENCH] file-mmap size="); con_num((uint64_t)st.st_size);
            con("B avg_ns="); con_num(mt / ITERS);
            con(" worst_ns="); con_num(mw); con("\n");
        }
        close(ffd);
    }

    con("[FORKBENCH] done\n");
    return 0;
}
