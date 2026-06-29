/* nettest/main.c — minimal outbound TCP HTTP test
 *
 * Waits for DHCP (via sys_netcfg query), then opens a TCP socket to
 * 1.1.1.1:80, sends a bare HTTP/1.0 GET, reads the response, and
 * prints the first ~200 bytes to serial. Used as a vigil oneshot
 * service to verify that outbound TCP works end-to-end.
 */
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>

#define SYS_NETCFG 500

typedef struct {
    uint8_t  mac[6];
    uint8_t  pad[2];
    uint32_t ip;
    uint32_t mask;
    uint32_t gateway;
} netcfg_info_t;

int
main(void)
{
    /* Wait up to 30s for DHCP to assign an IP. */
    netcfg_info_t info;
    int i;
    for (i = 0; i < 30; i++) {
        memset(&info, 0, sizeof(info));
        (void)syscall(SYS_NETCFG, 1, (long)&info, 0, 0);
        if (info.ip != 0) break;
        sleep(1);
    }
    if (info.ip == 0) {
        dprintf(2, "[NETTEST] no IP after 30s, giving up\n");
        return 1;
    }
    uint8_t *b = (uint8_t *)&info.ip;
    dprintf(2, "[NETTEST] got ip=%u.%u.%u.%u\n", b[0], b[1], b[2], b[3]);

    /* Open TCP socket. */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        dprintf(2, "[NETTEST] socket() failed: %d\n", errno);
        return 1;
    }

    /* Connect to 1.1.1.1:80. */
    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family      = AF_INET;
    dst.sin_port        = htons(80);
    dst.sin_addr.s_addr = htonl(0x01010101); /* 1.1.1.1 */
    dprintf(2, "[NETTEST] connecting to 1.1.1.1:80...\n");
    if (connect(fd, (struct sockaddr *)&dst, sizeof(dst)) < 0) {
        dprintf(2, "[NETTEST] connect failed: %d\n", errno);
        close(fd);
        return 1;
    }
    dprintf(2, "[NETTEST] CONNECTED\n");

    /* Send HTTP/1.0 GET. */
    const char *req =
        "GET / HTTP/1.0\r\n"
        "Host: 1.1.1.1\r\n"
        "User-Agent: Aegis/nettest\r\n"
        "Connection: close\r\n"
        "\r\n";
    int rlen = (int)strlen(req);
    int sent = (int)send(fd, req, (size_t)rlen, 0);
    dprintf(2, "[NETTEST] sent %d/%d bytes\n", sent, rlen);
    if (sent != rlen) {
        close(fd);
        return 1;
    }

    /* Read response. */
    char buf[256];
    int n = (int)recv(fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) {
        dprintf(2, "[NETTEST] recv returned %d\n", n);
        close(fd);
        return 1;
    }
    buf[n] = '\0';
    /* Strip \r so serial output isn't doubled. */
    int j, k = 0;
    for (j = 0; j < n; j++) {
        if (buf[j] == '\r') continue;
        if (buf[j] == '\n' && k > 0 && buf[k-1] == '\n') continue; /* collapse */
        buf[k++] = buf[j];
    }
    buf[k] = '\0';

    dprintf(2, "[NETTEST] recv %d bytes:\n%s\n[NETTEST] --- end ---\n", n, buf);
    close(fd);
    return 0;
}
