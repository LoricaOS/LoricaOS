/* perfbench-ipc — local IPC throughput probe (pipe + AF_UNIX stream).
 *
 * Gated behind the `perfbench_ipc` kernel cmdline token (inert in every other
 * boot, exactly like dltest / smpstress), so the vigil oneshot can ship in the
 * rootfs without firing in production or perturbing the boot oracle.
 *
 * When armed: pump a fixed number of bytes through (1) an anonymous pipe and
 * (2) an AF_UNIX SOCK_STREAM socketpair, child writes / parent reads, and report
 * on /dev/console (→ serial):
 *   [PERFIPC] pipe  bytes=B ms=T MBps=M
 *   [PERFIPC] unix  bytes=B ms=T MBps=M
 *   [PERFIPC] DONE
 * These paths are local (no NIC/SLIRP), so MB/s is a clean, repeatable measure
 * of the kernel pipe + AF_UNIX copy/lock/wakeup path. vigil does not wire a
 * oneshot's stdio to the console, so we open /dev/console ourselves.
 */
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <time.h>

#define TOTAL_BYTES  (64u * 1024u * 1024u)   /* 64 MiB per channel */
#define CHUNK        (64u * 1024u)           /* user-side read/write size */

static int g_con = 2;
static void con(const char *s) { write(g_con, s, strlen(s)); }
static void con_u64(uint64_t n)
{
    char b[24]; int i = 22; b[23] = 0;
    if (n == 0) { con("0"); return; }
    while (n > 0 && i >= 0) { b[i--] = (char)('0' + (int)(n % 10)); n /= 10; }
    con(&b[i + 1]);
}

static int armed(void)
{
    int fd = open("/proc/cmdline", O_RDONLY);
    if (fd < 0) return 0;
    char buf[512];
    int n = (int)read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = 0;
    return strstr(buf, "perfbench_ipc") != 0;
}

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static uint8_t g_src[CHUNK];
static uint8_t g_dst[CHUNK];

/* Child: write TOTAL_BYTES into wfd in CHUNK writes, then exit. */
static void writer_child(int wfd)
{
    uint32_t left = TOTAL_BYTES;
    while (left > 0) {
        uint32_t want = left < CHUNK ? left : CHUNK;
        uint32_t off  = 0;
        while (off < want) {
            int n = (int)write(wfd, g_src + off, want - off);
            if (n <= 0) _exit(1);
            off  += (uint32_t)n;
        }
        left -= want;
    }
    _exit(0);
}

/* Parent: read TOTAL_BYTES from rfd, return elapsed ms over the transfer. */
static uint64_t reader_drain(int rfd)
{
    uint64_t got = 0;
    uint64_t t0  = now_ms();
    while (got < TOTAL_BYTES) {
        int n = (int)read(rfd, g_dst, CHUNK);
        if (n <= 0) break;
        got += (uint64_t)n;
    }
    uint64_t t1 = now_ms();
    return (t1 > t0) ? (t1 - t0) : 1;
}

static void report(const char *label, uint64_t bytes, uint64_t ms)
{
    uint64_t mbps = bytes / 1048576u * 1000u / ms;   /* MiB/s */
    con("[PERFIPC] "); con(label);
    con(" bytes="); con_u64(bytes);
    con(" ms=");    con_u64(ms);
    con(" MBps=");  con_u64(mbps);
    con("\n");
}

/* Run one producer/consumer transfer over the given fd pair (rfd,wfd). */
static void run_channel(const char *label, int rfd, int wfd)
{
    pid_t pid = fork();
    if (pid == 0) {
        close(rfd);
        writer_child(wfd);
        _exit(0);
    }
    close(wfd);
    uint64_t ms = reader_drain(rfd);
    close(rfd);
    int st;
    while (waitpid(pid, &st, 0) < 0) { }
    report(label, TOTAL_BYTES, ms);
}

int
main(void)
{
    if (!armed())
        return 0;
    int c = open("/dev/console", O_WRONLY);
    if (c >= 0) g_con = c;

    memset(g_src, 0x5a, sizeof(g_src));

    /* (1) anonymous pipe */
    {
        int fds[2];
        if (pipe(fds) == 0)
            run_channel("pipe", fds[0], fds[1]);
        else
            con("[PERFIPC] pipe: create failed\n");
    }

    /* (2) AF_UNIX SOCK_STREAM socketpair */
    {
        int sv[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0)
            run_channel("unix", sv[0], sv[1]);
        else
            con("[PERFIPC] unix: socketpair failed\n");
    }

    con("[PERFIPC] DONE\n");
    return 0;
}
