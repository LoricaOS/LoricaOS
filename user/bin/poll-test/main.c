/* poll-test — userspace concurrent-pollers regression test.
 *
 * Modes:
 *   poll-test server <path>    : create AF_UNIX server, accept 2 clients,
 *                                write "A" to client #1, "B" to client #2,
 *                                sleep 2s, exit 0
 *   poll-test client <path>    : connect, poll for 1 byte (10s timeout),
 *                                read it, print "[POLL-TEST] got: <c>", exit 0
 *   poll-test all <path>       : fork server + 2 clients in one process,
 *                                wait for all three. Avoids needing the
 *                                shell to compose `&` / `;` / `wait`
 *                                (vortex send_keys can't transmit `&`).
 *
 * Test harness: invokes "poll-test all /tmp/p.sock" via send_keys, then
 * asserts both clients print "got: A" and "got: B" within seconds.
 * Pre-fix one client would hang indefinitely (single g_poll_waiter
 * starvation).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <errno.h>

#define ECONNREFUSED_LINUX 111

static int connect_to(const char *path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    /* Retry up to ~5s while server is still binding. */
    for (int i = 0; i < 100; i++) {
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0)
            return fd;
        if (errno != ECONNREFUSED_LINUX) {
            close(fd);
            return -1;
        }
        struct timespec ts = { 0, 50 * 1000 * 1000 };  /* 50 ms */
        nanosleep(&ts, NULL);
    }
    close(fd);
    return -1;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        dprintf(2, "usage: poll-test {server|client} <path>\n");
        return 2;
    }
    const char *mode = argv[1];
    const char *path = argv[2];

    if (strcmp(mode, "server") == 0) {
        int sfd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (sfd < 0) { dprintf(2, "[POLL-TEST] socket: %d\n", errno); return 1; }
        struct sockaddr_un a;
        memset(&a, 0, sizeof(a));
        a.sun_family = AF_UNIX;
        strncpy(a.sun_path, path, sizeof(a.sun_path) - 1);
        unlink(path);
        if (bind(sfd, (struct sockaddr *)&a, sizeof(a)) < 0) {
            dprintf(2, "[POLL-TEST] bind: %d\n", errno); return 1;
        }
        if (listen(sfd, 4) < 0) {
            dprintf(2, "[POLL-TEST] listen: %d\n", errno); return 1;
        }
        dprintf(1, "[POLL-TEST] server ready\n");
        int c1 = accept(sfd, NULL, NULL);
        int c2 = accept(sfd, NULL, NULL);
        if (c1 < 0 || c2 < 0) {
            dprintf(2, "[POLL-TEST] accept failed: c1=%d c2=%d errno=%d\n",
                    c1, c2, errno);
            return 1;
        }
        dprintf(1, "[POLL-TEST] both accepted\n");
        write(c1, "A", 1);
        write(c2, "B", 1);
        sleep(2);
        return 0;
    }

    if (strcmp(mode, "all") == 0) {
        /* argv[0] may be either an absolute path (when invoked as
         * `/bin/poll-test`) or a bare basename (when PATH-resolved by
         * the shell). execv requires a usable path, so fall back to
         * the canonical install location when argv[0] isn't absolute. */
        const char *self = argv[0][0] == '/' ? argv[0] : "/bin/poll-test";

        pid_t srv = fork();
        if (srv < 0) { dprintf(2, "[POLL-TEST] fork srv: %d\n", errno); return 1; }
        if (srv == 0) {
            char *args[] = { (char *)self, "server", (char *)path, NULL };
            execv(self, args);
            dprintf(2, "[POLL-TEST] execv server failed: %d\n", errno);
            _exit(1);
        }

        /* Give the server a moment to bind/listen before clients connect.
         * connect_to retries on ECONNREFUSED so this is belt-and-suspenders. */
        struct timespec ts = { 1, 0 };
        nanosleep(&ts, NULL);

        pid_t c1 = fork();
        if (c1 == 0) {
            char *args[] = { (char *)self, "client", (char *)path, NULL };
            execv(self, args);
            _exit(1);
        }
        pid_t c2 = fork();
        if (c2 == 0) {
            char *args[] = { (char *)self, "client", (char *)path, NULL };
            execv(self, args);
            _exit(1);
        }

        /* Reap all children. Order doesn't matter — both clients should
         * print their "got:" line within seconds. */
        int status;
        waitpid(c1, &status, 0);
        waitpid(c2, &status, 0);
        waitpid(srv, &status, 0);
        return 0;
    }

    if (strcmp(mode, "client") == 0) {
        int fd = connect_to(path);
        if (fd < 0) { dprintf(1, "[POLL-TEST] connect failed\n"); return 1; }
        struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };
        int r = poll(&pfd, 1, 10000);  /* 10s — generous */
        if (r <= 0) {
            dprintf(1, "[POLL-TEST] poll r=%d revents=0x%x\n",
                    r, pfd.revents);
            return 1;
        }
        char c;
        if (read(fd, &c, 1) != 1) {
            dprintf(1, "[POLL-TEST] read failed\n");
            return 1;
        }
        dprintf(1, "[POLL-TEST] got: %c\n", c);
        return 0;
    }

    return 2;
}
