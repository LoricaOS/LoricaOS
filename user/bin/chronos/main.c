/* chronos — Simple SNTP time synchronization daemon for LoricaOS.
 *
 * Sends NTP requests to time.google.com (216.239.35.0:123),
 * extracts the transmit timestamp, and sets the kernel wall clock
 * via clock_settime(CLOCK_REALTIME). Re-syncs every hour.
 *
 * Retries on failure with 60-second backoff, so it naturally succeeds
 * once the network (DHCP) is up. */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define NTP_PORT       123
#define NTP_EPOCH_DIFF 2208988800UL
/* time.google.com primary */
#define NTP_SERVER     "216.239.35.0"

static uint32_t ntohl_manual(uint32_t n)
{
    return ((n & 0xFF) << 24) | ((n & 0xFF00) << 8) |
           ((n >> 8) & 0xFF00) | ((n >> 24) & 0xFF);
}

/* "Set Automatically" toggle: /etc/aegis/ntp = "off" disables time sync.
 * Missing file or any other content = enabled (the default). Written by
 * Settings via sys_set_ntp (admin-gated). */
static int ntp_enabled(void)
{
    int fd = open("/etc/aegis/ntp", O_RDONLY);
    if (fd < 0)
        return 1;                 /* no file → enabled by default */
    char buf[8] = {0};
    int n = (int)read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n >= 3 && buf[0] == 'o' && buf[1] == 'f' && buf[2] == 'f')
        return 0;
    return 1;
}

int main(void)
{
    /* Wait for the network before the first sync. DHCP writes /etc/resolv.conf
     * immediately after it configures the interface + gateway (SYS_NETCFG), so
     * the file's presence means a usable route exists. Firing an NTP request
     * before then makes the kernel spend ~5s on a doomed ARP resolve and log a
     * spurious "chronos: sendto failed" during boot. Bounded to ~30s so a box
     * that never gets a lease still falls through to the normal 60s retry loop. */
    for (int i = 0; i < 300 && access("/etc/resolv.conf", F_OK) != 0; i++) {
        struct timespec w = { 0, 100 * 1000 * 1000 };  /* 100 ms */
        nanosleep(&w, NULL);
    }

    for (;;) {
        if (!ntp_enabled()) {
            sleep(60);            /* automatic time sync disabled */
            continue;
        }

        int fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) {
            fprintf(stderr, "chronos: socket failed\n");
            sleep(60);
            continue;
        }

        struct sockaddr_in srv;
        memset(&srv, 0, sizeof(srv));
        srv.sin_family = AF_INET;
        srv.sin_port = htons(NTP_PORT);
        inet_aton(NTP_SERVER, &srv.sin_addr);

        /* NTP request: LI=0, VN=4, Mode=3 (client). Stamp a random 8-byte nonce
         * into the transmit-timestamp field (bytes 40-47); a genuine server
         * copies it verbatim into the response's ORIGINATE field (bytes 24-31).
         * Verifying that echo below is what makes an off-path spoofer have to
         * guess 64 bits — without it, chronos accepted ANY datagram and let an
         * attacker set the wall clock arbitrarily (which drives TLS validity
         * windows, cert/freshness checks, and log integrity). */
        unsigned char pkt[48];
        unsigned char nonce[8];
        memset(pkt, 0, sizeof(pkt));
        pkt[0] = 0x23;  /* VN=4, Mode=3 */
        {
            int rfd = open("/dev/urandom", O_RDONLY);
            if (rfd < 0 || read(rfd, nonce, sizeof(nonce)) != (ssize_t)sizeof(nonce)) {
                /* Fall back to a monotonic+pid nonce rather than a predictable
                 * all-zero one. */
                struct timespec mono;
                clock_gettime(CLOCK_MONOTONIC, &mono);
                uint64_t m = (uint64_t)mono.tv_nsec ^ ((uint64_t)mono.tv_sec << 20)
                             ^ ((uint64_t)getpid() << 40);
                memcpy(nonce, &m, sizeof(nonce));
            }
            if (rfd >= 0) close(rfd);
        }
        memcpy(&pkt[40], nonce, sizeof(nonce));

        if (sendto(fd, pkt, 48, 0,
                   (struct sockaddr *)&srv, sizeof(srv)) != 48) {
            fprintf(stderr, "chronos: sendto failed\n");
            close(fd);
            sleep(60);
            continue;
        }

        /* Read response, capturing the sender so we can reject datagrams that
         * did not come from the server we queried. */
        memset(pkt, 0, sizeof(pkt));
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        int n = (int)recvfrom(fd, pkt, 48, 0,
                              (struct sockaddr *)&from, &fromlen);
        close(fd);

        if (n < 48) {
            fprintf(stderr, "chronos: short NTP response (%d bytes)\n", n);
            sleep(60);
            continue;
        }

        /* Source must be the NTP server:123 we queried. */
        if (from.sin_addr.s_addr != srv.sin_addr.s_addr ||
            from.sin_port != srv.sin_port) {
            fprintf(stderr, "chronos: NTP response from unexpected source\n");
            sleep(60);
            continue;
        }

        /* Must be a server reply (Mode=4) and not an unsynchronized alarm
         * (LI=3), and the ORIGINATE field must echo our nonce. */
        if ((pkt[0] & 0x07) != 4 || (pkt[0] >> 6) == 3 ||
            memcmp(&pkt[24], nonce, sizeof(nonce)) != 0) {
            fprintf(stderr, "chronos: NTP response failed validation\n");
            sleep(60);
            continue;
        }

        /* Extract transmit timestamp (bytes 40-43 = seconds since 1900) */
        uint32_t ntp_sec;
        memcpy(&ntp_sec, &pkt[40], 4);
        ntp_sec = ntohl_manual(ntp_sec);

        /* Sanity-bound: reject a timestamp before the NTP epoch base or after a
         * far-future ceiling (~year 2100) so a validated-but-absurd reply can't
         * jump the clock wildly. 6.1e9 s past 1970 ≈ 2163. */
        if (ntp_sec < NTP_EPOCH_DIFF ||
            (uint64_t)ntp_sec - NTP_EPOCH_DIFF > 6100000000ULL) {
            fprintf(stderr, "chronos: invalid NTP timestamp\n");
            sleep(60);
            continue;
        }

        uint64_t unix_sec = (uint64_t)ntp_sec - NTP_EPOCH_DIFF;

        /* Set system clock */
        struct timespec ts;
        ts.tv_sec = (time_t)unix_sec;
        ts.tv_nsec = 0;
        clock_settime(CLOCK_REALTIME, &ts);

        /* Format time for display */
        {
            time_t t = (time_t)unix_sec;
            struct tm *tm = gmtime(&t);
            if (tm) {
                fprintf(stderr,
                        "[CHRONOS] time synced: %04d-%02d-%02d %02d:%02d:%02d UTC\n",
                        tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                        tm->tm_hour, tm->tm_min, tm->tm_sec);
            } else {
                fprintf(stderr, "[CHRONOS] time synced: epoch=%lu\n",
                        (unsigned long)unix_sec);
            }
        }

        /* Re-sync in 1 hour */
        sleep(3600);
    }

    return 0;
}
