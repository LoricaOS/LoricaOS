/* ip / ifconfig — show network interface configuration.
 *
 * A single binary packaged as both /bin/ip and /bin/ifconfig. The output
 * format is chosen from argv[0]: "ifconfig" prints the classic net-tools
 * layout, anything else prints the iproute2-style `ip addr` layout.
 *
 * Config is read from the kernel via sys_netcfg op=1 (no capability needed),
 * which returns the eth0 MAC + the active IPv4 address/netmask/gateway. All
 * addresses are in network byte order, so the raw bytes are already the
 * dotted-quad in order (see user/bin/bastion greeter status line).
 *
 * Supported invocations:
 *   ip                 ip a / ip addr / ip address   -> show addresses
 *   ip link                                          -> show link (MAC) only
 *   ifconfig           ifconfig -a                    -> net-tools layout
 */
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

#define SYS_NETCFG 500

/* Mirrors kernel netcfg_info_t (kernel/syscall/sys_socket.c). */
typedef struct {
    uint8_t  mac[6];
    uint8_t  pad[2];
    uint32_t ip;
    uint32_t mask;
    uint32_t gateway;
} netcfg_info_t;

static const char *basename_of(const char *p)
{
    const char *b = p;
    for (; *p; p++)
        if (*p == '/') b = p + 1;
    return b;
}

/* network-byte-order u32 -> dotted quad in caller buffer */
static void fmt_ip(uint32_t addr, char *out, size_t n)
{
    const uint8_t *b = (const uint8_t *)&addr;
    snprintf(out, n, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

/* netmask (network byte order) -> CIDR prefix length */
static int mask_to_prefix(uint32_t mask)
{
    /* mask bytes are MSB-first in memory; count set bits */
    int bits = 0;
    const uint8_t *b = (const uint8_t *)&mask;
    for (int i = 0; i < 4; i++)
        for (int k = 0; k < 8; k++)
            if (b[i] & (1u << k)) bits++;
    return bits;
}

static void print_ip_style(const netcfg_info_t *info, int link_only)
{
    const uint8_t *m = info->mac;
    int has_mac = m[0] || m[1] || m[2] || m[3] || m[4] || m[5];

    /* loopback first, like iproute2 */
    printf("1: lo: <LOOPBACK,UP> mtu 65536\n");
    printf("    link/loopback 00:00:00:00:00:00\n");
    if (!link_only)
        printf("    inet 127.0.0.1/8 scope host lo\n");

    if (!has_mac) {
        printf("(no ethernet interface: driver did not register a netdev)\n");
        return;
    }

    printf("2: eth0: <BROADCAST,MULTICAST%s> mtu 1500\n",
           info->ip ? ",UP" : "");
    printf("    link/ether %02x:%02x:%02x:%02x:%02x:%02x\n",
           m[0], m[1], m[2], m[3], m[4], m[5]);
    if (!link_only && info->ip) {
        char ip[16];
        fmt_ip(info->ip, ip, sizeof ip);
        printf("    inet %s/%d scope global eth0\n", ip, mask_to_prefix(info->mask));
        if (info->gateway) {
            char gw[16];
            fmt_ip(info->gateway, gw, sizeof gw);
            printf("    gateway %s\n", gw);
        }
    } else if (!link_only) {
        printf("    (no IPv4 address — DHCP not configured)\n");
    }
}

static void print_ifconfig_style(const netcfg_info_t *info)
{
    const uint8_t *m = info->mac;
    int has_mac = m[0] || m[1] || m[2] || m[3] || m[4] || m[5];

    if (has_mac) {
        printf("eth0: flags=%s  mtu 1500\n",
               info->ip ? "<UP,BROADCAST,RUNNING,MULTICAST>"
                        : "<BROADCAST,MULTICAST>");
        if (info->ip) {
            char ip[16], nm[16];
            fmt_ip(info->ip, ip, sizeof ip);
            fmt_ip(info->mask, nm, sizeof nm);
            printf("        inet %s  netmask %s\n", ip, nm);
            if (info->gateway) {
                char gw[16];
                fmt_ip(info->gateway, gw, sizeof gw);
                printf("        gateway %s\n", gw);
            }
        }
        printf("        ether %02x:%02x:%02x:%02x:%02x:%02x\n",
               m[0], m[1], m[2], m[3], m[4], m[5]);
        printf("\n");
    }

    printf("lo: flags=<UP,LOOPBACK,RUNNING>  mtu 65536\n");
    printf("        inet 127.0.0.1  netmask 255.0.0.0\n");
}

int main(int argc, char **argv)
{
    netcfg_info_t info;
    memset(&info, 0, sizeof info);
    if (syscall(SYS_NETCFG, 1, (long)&info, 0, 0) < 0) {
        /* ENODEV etc. — no interface; still show loopback for ip style. */
        memset(&info, 0, sizeof info);
    }

    const char *prog = basename_of(argv[0]);
    if (strcmp(prog, "ifconfig") == 0) {
        print_ifconfig_style(&info);
        return 0;
    }

    /* iproute2-style. Accept and loosely interpret a subcommand. */
    int link_only = 0;
    if (argc > 1) {
        const char *cmd = argv[1];
        if (strcmp(cmd, "link") == 0 || strcmp(cmd, "l") == 0)
            link_only = 1;
        /* "a", "addr", "address", "show", etc. -> default address view */
    }
    print_ip_style(&info, link_only);
    return 0;
}
