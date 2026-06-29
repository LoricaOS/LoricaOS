/* dltest — TCP download-throughput + integrity probe.
 *
 * Gated behind the `dltest` kernel cmdline token (inert in every other boot,
 * exactly like smpstress / lumen-probe), so the vigil oneshot service can ship
 * in the rootfs without firing in production or breaking the boot oracle.
 *
 * When armed: wait for DHCP, open TCP to DLTEST_HOST:80, GET /ttest.bin with
 * HTTP/1.0 + Connection: close, drain the whole body, and report on
 * /dev/console (→ serial):
 *   [DLTEST] bytes=B ms=T bps=Bps fnv=0xH
 * bytes/fnv let the harness prove byte-exactness against the origin; bps is the
 * measured receive throughput (this exercises exactly the kernel TCP rx ring +
 * window + ACK path we are tuning). vigil does not wire a oneshot's stdio to the
 * console, so we open /dev/console ourselves.
 */
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>

#define SYS_NETCFG 500
#define DLTEST_HOST 0x0A0A0A28u   /* 10.10.10.40 (CT112 nginx) */
#define DLTEST_PORT 80

typedef struct {
    uint8_t  mac[6];
    uint8_t  pad[2];
    uint32_t ip;
    uint32_t mask;
    uint32_t gateway;
} netcfg_info_t;

static int g_con = 2;

static void con(const char *s) { write(g_con, s, strlen(s)); }

static void con_u64(uint64_t n)
{
    char b[24]; int i = 22; b[23] = 0;
    if (n == 0) { con("0"); return; }
    while (n > 0 && i >= 0) { b[i--] = (char)('0' + (int)(n % 10)); n /= 10; }
    con(&b[i + 1]);
}

static void con_hex64(uint64_t n)
{
    static const char hx[] = "0123456789abcdef";
    char b[17]; int i;
    for (i = 15; i >= 0; i--) { b[i] = hx[n & 0xf]; n >>= 4; }
    b[16] = 0;
    con(b);
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
    return strstr(buf, "dltest") != 0;
}

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

int
main(void)
{
    if (!armed())
        return 0;

    int c = open("/dev/console", O_WRONLY);
    if (c >= 0) g_con = c;

    /* Wait up to 30s for DHCP. */
    netcfg_info_t info;
    int i;
    for (i = 0; i < 30; i++) {
        memset(&info, 0, sizeof(info));
        (void)syscall(SYS_NETCFG, 1, (long)&info, 0, 0);
        if (info.ip != 0) break;
        sleep(1);
    }
    if (info.ip == 0) { con("[DLTEST] no IP after 30s\n"); return 1; }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { con("[DLTEST] socket failed\n"); return 1; }

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family      = AF_INET;
    dst.sin_port        = htons(DLTEST_PORT);
    dst.sin_addr.s_addr = htonl(DLTEST_HOST);
    if (connect(fd, (struct sockaddr *)&dst, sizeof(dst)) < 0) {
        con("[DLTEST] connect failed\n"); close(fd); return 1;
    }

    const char *req =
        "GET /ttest.bin HTTP/1.0\r\n"
        "Host: herald.byexec.com\r\n"
        "User-Agent: Aegis/dltest\r\n"
        "Connection: close\r\n"
        "\r\n";
    int rlen = (int)strlen(req);
    if ((int)send(fd, req, (size_t)rlen, 0) != rlen) {
        con("[DLTEST] send failed\n"); close(fd); return 1;
    }

    /* Drain the response. Strip headers (up to the first CRLFCRLF), then count
     * and FNV-1a-hash every body byte and time the body transfer. */
    static uint8_t buf[16384];
    uint64_t fnv   = 0xcbf29ce484222325ULL;
    const uint64_t prime = 0x100000001b3ULL;
    uint64_t body  = 0;
    int      in_body = 0;
    int      hdr_match = 0;   /* state machine over \r\n\r\n */
    uint64_t t0 = 0;
    int n;
    while ((n = (int)recv(fd, buf, sizeof(buf), 0)) > 0) {
        int j = 0;
        if (!in_body) {
            for (; j < n; j++) {
                char ch = (char)buf[j];
                if (ch == '\r') continue;
                if (ch == '\n') {
                    if (++hdr_match == 2) { in_body = 1; j++; break; }
                } else {
                    hdr_match = 0;
                }
            }
            if (in_body) t0 = now_ms();
        }
        if (in_body) {
            for (; j < n; j++) {
                fnv = (fnv ^ buf[j]) * prime;
                body++;
            }
        }
    }
    uint64_t t1 = now_ms();
    close(fd);

    uint64_t ms  = (t0 && t1 > t0) ? (t1 - t0) : 1;
    uint64_t bps = body * 1000u / ms;

    con("[DLTEST] bytes="); con_u64(body);
    con(" ms=");            con_u64(ms);
    con(" bps=");           con_u64(bps);
    con(" fnv=0x");         con_hex64(fnv);
    con("\n[DLTEST] DONE\n");
    return 0;
}
