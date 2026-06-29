/* httpd_bin.c — minimal HTTP server for socket API testing */
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <string.h>

static const char RESPONSE[] =
    "HTTP/1.0 200 OK\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 17\r\n"
    "\r\n"
    "Hello from Aegis\n";

static void log_str(const char *s)
{
    write(1, s, strlen(s));
}

int main(void)
{
    log_str("httpd: starting\n");

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { log_str("httpd: socket() failed\n"); return 1; }
    log_str("httpd: socket ok\n");

    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_port        = __builtin_bswap16(80);
    addr.sin_addr.s_addr = 0;  /* INADDR_ANY */

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0)
        { log_str("httpd: bind() failed\n"); return 1; }
    log_str("httpd: bind ok\n");

    if (listen(srv, 4) < 0)
        { log_str("httpd: listen() failed\n"); return 1; }
    log_str("httpd: listening on :80\n");

    for (;;) {
        log_str("httpd: waiting for connection\n");
        int cli = accept(srv, NULL, NULL);
        if (cli < 0) { log_str("httpd: accept failed\n"); continue; }
        log_str("httpd: accepted connection\n");

        char buf[256];
        int n = read(cli, buf, sizeof(buf) - 1);  /* drain request */
        log_str("httpd: read done\n");
        int w = write(cli, RESPONSE, sizeof(RESPONSE) - 1);
        log_str("httpd: write done\n");
        (void)n; (void)w;
        close(cli);
        log_str("httpd: closed\n");
    }
}
