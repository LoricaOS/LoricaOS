/* user/dhcp/main.c — DHCP client daemon for Aegis
 *
 * Implements RFC 2131 SELECTING → REQUESTING → BOUND → RENEWING.
 * Writes acquired IP via sys_netcfg (syscall 500).
 * Writes DNS server to /etc/resolv.conf (ramfs-backed, writable).
 * Vigil service: /etc/vigil/services/dhcp/ with NET_ADMIN NET_SOCKET caps.
 */
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/un.h>

#define SYS_NETCFG 500

/* Caps are now granted by kernel policy at exec time via /etc/aegis/caps.d/.
 * No runtime cap request needed. */

/* ---- DHCP packet layout (RFC 2131) ----------------------------------- */

typedef struct __attribute__((packed)) {
    uint8_t  op;          /* 1=BOOTREQUEST */
    uint8_t  htype;       /* 1=Ethernet */
    uint8_t  hlen;        /* 6 */
    uint8_t  hops;        /* 0 */
    uint32_t xid;         /* transaction id */
    uint16_t secs;        /* 0 */
    uint16_t flags;       /* 0x8000 = broadcast flag */
    uint32_t ciaddr;      /* client IP (0 in SELECTING) */
    uint32_t yiaddr;      /* 'your' IP (filled by server) */
    uint32_t siaddr;      /* server IP */
    uint32_t giaddr;      /* gateway IP */
    uint8_t  chaddr[16];  /* client hardware address */
    uint8_t  sname[64];   /* server name (unused) */
    uint8_t  file[128];   /* boot file (unused) */
    uint32_t magic;       /* 0x63825363 big-endian */
    uint8_t  options[308];
} dhcp_pkt_t;

/* sys_netcfg op=1 result layout */
typedef struct {
    uint8_t  mac[6];
    uint8_t  pad[2];
    uint32_t ip;
    uint32_t mask;
    uint32_t gateway;
} netcfg_info_t;

/* ---- Module state ---------------------------------------------------- */

static uint8_t  s_mac[6];
static uint32_t s_xid;
static uint32_t s_offered_ip;
static uint32_t s_server_ip;
static uint32_t s_ip;
static uint32_t s_mask;
static uint32_t s_gateway;
static uint32_t s_dns;
static uint32_t s_lease;
static int      s_has_dns;

static const int s_backoff[] = { 4, 8, 16, 32, 32 };
#define MAX_ATTEMPTS 5
#define MAX_RETRIES  3

/* ---- Helpers --------------------------------------------------------- */

static uint32_t
make_xid(void)
{
    uint32_t x = ((uint32_t)s_mac[2] << 24) | ((uint32_t)s_mac[3] << 16) |
                 ((uint32_t)s_mac[4] <<  8) |  (uint32_t)s_mac[5];
    return x ^ (uint32_t)time(NULL);
}

static void
ip_to_str(uint32_t ip_nbo, char *buf, int bufsz)
{
    uint8_t *b = (uint8_t *)&ip_nbo;
    snprintf(buf, bufsz, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

static int
prefix_len(uint32_t mask_nbo)
{
    uint32_t m = ntohl(mask_nbo);
    int n = 0;
    while (m & 0x80000000u) { n++; m <<= 1; }
    return n;
}

/* ---- Option parsing -------------------------------------------------- */

static void
parse_options(const uint8_t *opts, int len, uint8_t *msg_type_out)
{
    int i = 0;
    *msg_type_out = 0;
    /* Defaults */
    s_mask    = htonl(0xffffff00u);
    s_gateway = 0;
    s_dns     = 0;
    s_has_dns = 0;
    s_lease   = 3600;
    s_server_ip = 0;

    while (i < len) {
        uint8_t tag = opts[i++];
        if (tag == 255) break;
        if (tag == 0)   continue;
        if (i >= len) break;
        uint8_t olen = opts[i++];
        if (i + olen > len) break;

        switch (tag) {
        case 1:
            if (olen == 4) memcpy(&s_mask, opts + i, 4);
            break;
        case 3:
            if (olen >= 4) memcpy(&s_gateway, opts + i, 4);
            break;
        case 6:
            if (olen >= 4) { memcpy(&s_dns, opts + i, 4); s_has_dns = 1; }
            break;
        case 51:
            if (olen == 4) {
                uint32_t tmp;
                memcpy(&tmp, opts + i, 4);
                s_lease = ntohl(tmp);
            }
            break;
        case 53:
            if (olen == 1) *msg_type_out = opts[i];
            break;
        case 54:
            if (olen == 4) memcpy(&s_server_ip, opts + i, 4);
            break;
        default:
            break;
        }
        i += olen;
    }
}

/* ---- Packet construction -------------------------------------------- */

static void
pkt_base(dhcp_pkt_t *p)
{
    memset(p, 0, sizeof(*p));
    p->op     = 1;
    p->htype  = 1;
    p->hlen   = 6;
    p->xid    = htonl(s_xid);
    p->flags  = htons(0x8000);  /* broadcast flag — required on real hardware */
    memcpy(p->chaddr, s_mac, 6);
    p->magic  = htonl(0x63825363u);
}

static void
pkt_add_discover(dhcp_pkt_t *p)
{
    uint8_t *opt = p->options;
    int off = 0;
    opt[off++] = 53; opt[off++] = 1; opt[off++] = 1;  /* DHCPDISCOVER */
    opt[off++] = 55; opt[off++] = 4;
    opt[off++] = 1; opt[off++] = 3; opt[off++] = 6; opt[off++] = 51;
    opt[off++] = 61; opt[off++] = 7; opt[off++] = 1;
    memcpy(opt + off, s_mac, 6); off += 6;
    opt[off++] = 255;
}

static void
pkt_add_request(dhcp_pkt_t *p, uint32_t offered_ip, uint32_t server_ip)
{
    uint8_t *opt = p->options;
    int off = 0;
    opt[off++] = 53; opt[off++] = 1; opt[off++] = 3;  /* DHCPREQUEST */
    opt[off++] = 55; opt[off++] = 4;
    opt[off++] = 1; opt[off++] = 3; opt[off++] = 6; opt[off++] = 51;
    if (offered_ip) {
        opt[off++] = 50; opt[off++] = 4;
        memcpy(opt + off, &offered_ip, 4); off += 4;
    }
    if (server_ip) {
        opt[off++] = 54; opt[off++] = 4;
        memcpy(opt + off, &server_ip, 4); off += 4;
    }
    opt[off++] = 61; opt[off++] = 7; opt[off++] = 1;
    memcpy(opt + off, s_mac, 6); off += 6;
    opt[off++] = 255;
}

/* ---- Socket ---------------------------------------------------------- */

static int
open_udp_sock(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));

    struct timeval tv = { 0, 500000 };  /* 500ms receive timeout */
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(68);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int
send_broadcast(int fd, dhcp_pkt_t *pkt)
{
    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family      = AF_INET;
    dst.sin_port        = htons(67);
    dst.sin_addr.s_addr = INADDR_BROADCAST;
    return (int)sendto(fd, pkt, sizeof(*pkt), 0,
                       (struct sockaddr *)&dst, sizeof(dst));
}

static int
send_unicast(int fd, dhcp_pkt_t *pkt, uint32_t server_ip_nbo)
{
    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family      = AF_INET;
    dst.sin_port        = htons(67);
    dst.sin_addr.s_addr = server_ip_nbo;
    return (int)sendto(fd, pkt, sizeof(*pkt), 0,
                       (struct sockaddr *)&dst, sizeof(dst));
}

/* ---- DNS write ------------------------------------------------------- */

static void
write_resolv_conf(void)
{
    int fd = open("/etc/resolv.conf", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    /* The DHCP-provided resolver first (respects the local network), if any. */
    if (s_has_dns) {
        uint8_t *d = (uint8_t *)&s_dns;
        dprintf(fd, "nameserver %u.%u.%u.%u\n", d[0], d[1], d[2], d[3]);
    }
    /* Reliable public resolvers as parallel fallbacks. musl's stub resolver
     * queries ALL listed nameservers concurrently and uses the fastest reply,
     * so a slow or lossy upstream (e.g. an ISP resolver reached over a busy
     * WAN) no longer makes DNS intermittently fail — Cloudflare/Google, both
     * low-latency anycast, almost always answer first. (MAXNS is 3.) */
    dprintf(fd, "nameserver 1.1.1.1\n");
    dprintf(fd, "nameserver 8.8.8.8\n");
    close(fd);
}

/* ---- Apply configuration -------------------------------------------- */

static void
apply_config(void)
{
    /* sys_netcfg op=0: set IP/mask/gw — values already in network byte order */
    (void)syscall(SYS_NETCFG, 0, (long)s_ip, (long)s_mask, (long)s_gateway);
    write_resolv_conf();

    char ip_s[20], gw_s[20], dns_s[24];
    ip_to_str(s_ip, ip_s, sizeof(ip_s));
    ip_to_str(s_gateway, gw_s, sizeof(gw_s));
    int plen = prefix_len(s_mask);

    if (s_has_dns) {
        ip_to_str(s_dns, dns_s, sizeof(dns_s));
        dprintf(2, "[DHCP] acquired %s/%d gw %s lease %us dns %s\n",
                ip_s, plen, gw_s, s_lease, dns_s);
    } else {
        dprintf(2, "[DHCP] acquired %s/%d gw %s lease %us\n",
                ip_s, plen, gw_s, s_lease);
    }
}

/* ---- SELECTING + REQUESTING ----------------------------------------- */

static int
try_acquire(int fd)
{
    dhcp_pkt_t pkt, reply;
    struct sockaddr_in from;
    socklen_t fromlen;
    uint8_t msg_type;
    int retry;

    /* SELECTING: send DISCOVER, wait for OFFER */
    pkt_base(&pkt);
    pkt_add_discover(&pkt);
    int got_offer = 0;
    for (retry = 0; retry < MAX_RETRIES && !got_offer; retry++) {
        if (send_broadcast(fd, &pkt) < 0) continue;
        for (;;) {
            fromlen = sizeof(from);
            int n = (int)recvfrom(fd, &reply, sizeof(reply), 0,
                                 (struct sockaddr *)&from, &fromlen);
            if (n < 0) break;
            if (n < 240) continue;  /* too short: need at least fixed header + magic */
            if (reply.xid != pkt.xid) continue;
            int opts_len = n - (int)((uint8_t *)reply.options - (uint8_t *)&reply);
            if (opts_len < 0) opts_len = 0;
            parse_options(reply.options, opts_len, &msg_type);
            if (msg_type == 2) {  /* DHCPOFFER */
                s_offered_ip = reply.yiaddr;
                if (!s_server_ip)
                    s_server_ip = from.sin_addr.s_addr;
                got_offer = 1;
                break;
            }
        }
    }
    if (!got_offer) return 0;

    /* REQUESTING: send REQUEST, wait for ACK */
    pkt_base(&pkt);
    pkt_add_request(&pkt, s_offered_ip, s_server_ip);

    int got_ack = 0;
    for (retry = 0; retry < MAX_RETRIES && !got_ack; retry++) {
        if (send_broadcast(fd, &pkt) < 0) continue;
        for (;;) {
            fromlen = sizeof(from);
            int n = (int)recvfrom(fd, &reply, sizeof(reply), 0,
                                 (struct sockaddr *)&from, &fromlen);
            if (n < 0) break;
            if (n < 240) continue;  /* too short */
            if (reply.xid != pkt.xid) continue;
            int opts_len = n - (int)((uint8_t *)reply.options - (uint8_t *)&reply);
            if (opts_len < 0) opts_len = 0;
            parse_options(reply.options, opts_len, &msg_type);
            if (msg_type == 5) { s_ip = reply.yiaddr; got_ack = 1; break; }
            if (msg_type == 6) return 0;  /* DHCPNAK */
        }
    }
    return got_ack;
}

/* ---- RENEWING -------------------------------------------------------- */

static int
try_renew(int fd)
{
    dhcp_pkt_t pkt, reply;
    struct sockaddr_in from;
    socklen_t fromlen;
    uint8_t msg_type;
    int retry;

    pkt_base(&pkt);
    pkt.flags  = 0;        /* unicast — no broadcast flag for renewal */
    pkt.ciaddr = s_ip;     /* RFC 2131 §4.4.5: set ciaddr for renewal */
    s_xid = make_xid();
    pkt.xid = htonl(s_xid);
    pkt_add_request(&pkt, 0, 0);  /* no option 50/54 for unicast renewal */

    int got_ack = 0;
    for (retry = 0; retry < MAX_RETRIES && !got_ack; retry++) {
        if (send_unicast(fd, &pkt, s_server_ip) < 0) continue;
        for (;;) {
            fromlen = sizeof(from);
            int n = (int)recvfrom(fd, &reply, sizeof(reply), 0,
                                 (struct sockaddr *)&from, &fromlen);
            if (n < 0) break;
            if (n < 240) continue;  /* too short */
            if (reply.xid != pkt.xid) continue;
            int opts_len = n - (int)((uint8_t *)reply.options - (uint8_t *)&reply);
            if (opts_len < 0) opts_len = 0;
            parse_options(reply.options, opts_len, &msg_type);
            if (msg_type == 5) { s_ip = reply.yiaddr; got_ack = 1; break; }
            if (msg_type == 6) return 0;  /* DHCPNAK */
        }
    }
    return got_ack;
}

/* ---- main ------------------------------------------------------------ */

int
main(void)
{
    /* NET_ADMIN cap comes from kernel policy (/etc/aegis/caps.d/dhcp) at exec time */

    /* Read MAC via sys_netcfg op=1 — if MAC is all zeros, no NIC present */
    netcfg_info_t info;
    memset(&info, 0, sizeof(info));
    (void)syscall(SYS_NETCFG, 1, (long)&info, 0, 0);
    memcpy(s_mac, info.mac, 6);

    /* Check if we have a real NIC (non-zero MAC) */
    if (s_mac[0] == 0 && s_mac[1] == 0 && s_mac[2] == 0 &&
        s_mac[3] == 0 && s_mac[4] == 0 && s_mac[5] == 0) {
        dprintf(2, "[DHCP] no network interface (MAC 00:00:00:00:00:00), exiting\n");
        return 0;
    }

    int attempt;
    for (attempt = 0; attempt < MAX_ATTEMPTS; attempt++) {
        if (attempt > 0) {
            int delay = s_backoff[attempt - 1];
            dprintf(2, "[DHCP] retry %d, waiting %ds\n", attempt, delay);
            sleep((unsigned int)delay);
        }

        s_xid = make_xid();

        int fd = open_udp_sock();
        if (fd < 0) continue;

        if (try_acquire(fd)) {
            apply_config();
            /* BOUND: sleep T1 = lease/2, then RENEWING loop */
            while (1) {
                uint32_t t1 = s_lease / 2;
                if (t1 < 30) t1 = 30;
                sleep((unsigned int)t1);
                s_xid = make_xid();
                if (try_renew(fd)) {
                    dprintf(2, "[DHCP] renewed lease, next renewal in %us\n",
                            s_lease / 2);
                } else {
                    dprintf(2, "[DHCP] renewal failed, restarting\n");
                    close(fd);
                    attempt = -1;  /* for-loop ++attempt makes it 0 — restart fresh */
                    goto next_attempt;
                }
            }
        }
        close(fd);
    next_attempt:;
    }

    dprintf(2, "[DHCP] failed after %d attempts, exiting\n", MAX_ATTEMPTS);
    return 1;
}
