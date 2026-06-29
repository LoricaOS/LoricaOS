/* sockreftest — regression test for the AF_INET socket + epoll refcount fix.
 *
 * Proves the Phase 1 fix (kernel/net/socket.c, kernel/net/epoll.c): an fd-backed
 * socket/epoll object must live until the LAST fd referencing it is closed, not
 * the first. Before the fix, sock_vfs_dup was a no-op and sock_vfs_close
 * unconditionally freed the slot, so:
 *
 *   - dup-then-close freed the slot out from under the surviving fd, and
 *   - the next socket() reused that freed slot, so the surviving fd silently
 *     ALIASED a different connection (cross-connection use-after-free).
 *
 * Detection is purely local — no network link, no peer. We bind each socket to
 * a distinct, recognisable local port and read it back with getsockname(): the
 * port is the slot's identity. The probe is observable two ways on a pre-fix
 * kernel:
 *   (A) after close(s), getsockname(dup_of_s) returns EBADF — the slot was
 *       freed (sock_get sees SOCK_FREE), so the surviving fd is already dead.
 *   (B) after a fresh socket()+bind() reuses that freed slot, getsockname(d)
 *       returns the FRESH socket's port — the surviving fd now aliases it.
 * On the fixed kernel the surviving fd keeps its own bound port through both.
 *
 * Prints exactly one summary line the harness asserts on:
 *   SOCKREF_OK                — every check passed (fixed kernel)
 *   SOCKREF_FAIL: <why>       — a check detected the UAF / corruption
 *
 * Needs the NET_SOCKET capability (caps.d/sockreftest) for socket().
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <sys/epoll.h>

/* Distinct, eye-catchable local ports — one per socket so a port read back
 * from the wrong slot is unmistakable. All are in the registered range and
 * never bound by anything else in the test rootfs. */
#define PORT_DUP_A   0xA1A1u   /* 41377 — the dup-survivor's own port      */
#define PORT_DUP_B   0xB2B2u   /* 45746 — a fresh socket after close(s)    */
#define PORT_FORK    0xC3C3u   /* 50115 — the fork-survivor's own port     */
#define PORT_FORK_B  0xD4D4u   /* 54484 — a fresh socket after child close */

static void fail(const char *why)
{
    /* One line, flushed, then exit non-zero. printf to stdout so the harness
     * (and a human via the console) sees the reason. */
    printf("SOCKREF_FAIL: %s\n", why);
    fflush(stdout);
    _exit(1);
}

/* Make a TCP socket bound to 127.0.0.1:port. No listen/connect — bind alone
 * stores local_port in the slot, which is all the identity probe needs and
 * requires no network. Returns the fd or calls fail(). */
static int make_bound(unsigned port, const char *ctx)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%s: socket() = %d (errno %d)", ctx, fd, errno);
        fail(buf);
    }
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons((unsigned short)port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%s: bind(%u) errno %d", ctx, port, errno);
        close(fd);
        fail(buf);
    }
    return fd;
}

/* Read the bound local port back from fd via getsockname(). On EBADF (the
 * pre-fix "slot was freed under me" symptom) returns a sentinel so the caller
 * can report it distinctly. Returns the port, or 0 on getsockname failure. */
static unsigned bound_port(int fd, int *err_out)
{
    struct sockaddr_in sa;
    socklen_t len = sizeof(sa);
    memset(&sa, 0, sizeof(sa));
    int r = getsockname(fd, (struct sockaddr *)&sa, &len);
    if (r != 0) {
        if (err_out) *err_out = errno;
        return 0;
    }
    if (err_out) *err_out = 0;
    return ntohs(sa.sin_port);
}

/* Test 1+2: dup-then-close keeps the surviving fd live and un-aliased. */
static void test_dup(void)
{
    int s = make_bound(PORT_DUP_A, "dup");
    int d = dup(s);
    if (d < 0)
        fail("dup: dup() failed");

    /* Close the original. Pre-fix this frees the shared slot; post-fix it just
     * drops one reference and the slot lives for d. */
    close(s);

    /* (A) d must still be a live socket reporting its own bound port. Pre-fix
     *     the slot is SOCK_FREE here → getsockname(d) returns EBADF. */
    int e = 0;
    unsigned p = bound_port(d, &e);
    if (p == 0) {
        char buf[80];
        snprintf(buf, sizeof(buf),
                 "dup: getsockname(d) after close(s) failed errno %d (slot freed under dup)", e);
        close(d);
        fail(buf);
    }
    if (p != PORT_DUP_A) {
        char buf[80];
        snprintf(buf, sizeof(buf),
                 "dup: d reports port 0x%X, expected 0x%X (aliased)", p, PORT_DUP_A);
        close(d);
        fail(buf);
    }

    /* (B) Force a fresh slot allocation. On the pre-fix kernel close(s) freed
     *     d's slot, so this socket()+bind() reuses it and d now aliases this
     *     new socket. Post-fix, d's slot was never freed, so the fresh socket
     *     gets a different slot and the two stay independent. */
    int s2 = make_bound(PORT_DUP_B, "dup-realloc");
    unsigned pd = bound_port(d, &e);
    unsigned p2 = bound_port(s2, &e);
    if (pd != PORT_DUP_A) {
        char buf[96];
        snprintf(buf, sizeof(buf),
                 "dup: d aliased a reused slot — port 0x%X, expected 0x%X (cross-conn UAF)",
                 pd, PORT_DUP_A);
        close(d); close(s2);
        fail(buf);
    }
    if (p2 != PORT_DUP_B) {
        char buf[80];
        snprintf(buf, sizeof(buf),
                 "dup: fresh socket reports port 0x%X, expected 0x%X", p2, PORT_DUP_B);
        close(d); close(s2);
        fail(buf);
    }
    close(d);
    close(s2);
}

/* Test 3: fork-then-close in the child must not free the parent's fd. */
static void test_fork(void)
{
    int s = make_bound(PORT_FORK, "fork");

    pid_t pid = fork();
    if (pid < 0)
        fail("fork: fork() failed");
    if (pid == 0) {
        /* Child: it inherited the fd (fd_table_copy fired sock_vfs_dup). Close
         * it and exit. Pre-fix the no-op .dup meant no reference was taken, so
         * this close frees the slot the parent still holds. */
        close(s);
        _exit(0);
    }

    /* Parent: reap the child, then prove s is still live + correctly bound. */
    int st = 0;
    waitpid(pid, &st, 0);

    int e = 0;
    unsigned p = bound_port(s, &e);
    if (p == 0) {
        char buf[80];
        snprintf(buf, sizeof(buf),
                 "fork: getsockname(s) after child close failed errno %d (slot freed by child)", e);
        close(s);
        fail(buf);
    }
    if (p != PORT_FORK) {
        char buf[80];
        snprintf(buf, sizeof(buf),
                 "fork: s reports port 0x%X, expected 0x%X (aliased)", p, PORT_FORK);
        close(s);
        fail(buf);
    }

    /* Confirm no aliasing: a fresh socket must not reuse s's slot. */
    int s3 = make_bound(PORT_FORK_B, "fork-realloc");
    unsigned ps = bound_port(s, &e);
    if (ps != PORT_FORK) {
        char buf[96];
        snprintf(buf, sizeof(buf),
                 "fork: s aliased a reused slot — port 0x%X, expected 0x%X (cross-conn UAF)",
                 ps, PORT_FORK);
        close(s); close(s3);
        fail(buf);
    }
    close(s);
    close(s3);
}

/* Test 4: epoll dup-then-close — same fix class (epoll_fd_t refcount). epoll
 * has no getsockname analogue, so the probe is: a watch added before close(e)
 * must still be queryable through the dup afterward. We verify the dup is a
 * live epoll fd by performing an EPOLL_CTL_DEL of the watch we added on the
 * original — DEL returns ENOENT if the instance was freed/reused (watch gone),
 * 0 if the instance (and its watch list) survived. */
static void test_epoll_dup(void)
{
    int ep = epoll_create1(0);
    if (ep < 0)
        fail("epoll: epoll_create1 failed");

    /* Watch our own stdin fd (always valid); we never wait, only add/del. */
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events  = EPOLLIN;
    ev.data.fd = 0;
    if (epoll_ctl(ep, EPOLL_CTL_ADD, 0, &ev) != 0) {
        close(ep);
        fail("epoll: EPOLL_CTL_ADD failed");
    }

    int ed = dup(ep);
    if (ed < 0) { close(ep); fail("epoll: dup() failed"); }

    /* Close the original. Pre-fix (.dup == NULL) this frees the instance; the
     * dup ed then points at a freed/reused slot. */
    close(ep);

    /* The watch on fd 0 must still be there via ed. DEL it: 0 = instance and
     * watch survived (fixed); ENOENT/EBADF = instance was freed under ed. */
    if (epoll_ctl(ed, EPOLL_CTL_DEL, 0, &ev) != 0) {
        char buf[80];
        snprintf(buf, sizeof(buf),
                 "epoll: del-after-dup-close errno %d (instance freed under dup)", errno);
        close(ed);
        fail(buf);
    }
    close(ed);
}

int main(void)
{
    test_dup();
    test_fork();
    test_epoll_dup();

    printf("SOCKREF_OK\n");
    fflush(stdout);
    return 0;
}
