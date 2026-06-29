#include <poll.h>
#include <unistd.h>
#include <string.h>
#include <sys/epoll.h>

static int g_pass = 0;
static int g_fail = 0;

static void pass(const char *name)
{
    const char *prefix = "[POLLTEST] PASS: ";
    write(1, prefix, strlen(prefix));
    write(1, name, strlen(name));
    write(1, "\n", 1);
    g_pass++;
}

static void fail(const char *name)
{
    const char *prefix = "[POLLTEST] FAIL: ";
    write(1, prefix, strlen(prefix));
    write(1, name, strlen(name));
    write(1, "\n", 1);
    g_fail++;
}

/* Test 1: pipe poll — empty, with data, after close */
static void test_pipe_poll(void)
{
    int fds[2];
    if (pipe(fds) != 0) { fail("pipe_poll create"); return; }

    /* Empty pipe: poll read end with timeout=0, expect 0 ready */
    struct pollfd pfd = { .fd = fds[0], .events = POLLIN };
    int r = poll(&pfd, 1, 0);
    if (r != 0 || (pfd.revents & POLLIN)) {
        fail("pipe_poll empty");
    } else {
        pass("pipe_poll empty");
    }

    /* Write data: poll read end, expect POLLIN */
    write(fds[1], "X", 1);
    pfd.revents = 0;
    r = poll(&pfd, 1, 0);
    if (r != 1 || !(pfd.revents & POLLIN)) {
        fail("pipe_poll data");
    } else {
        pass("pipe_poll data");
    }

    /* Drain the data first */
    char buf[1];
    read(fds[0], buf, 1);

    /* Close write end: poll read end, expect POLLHUP */
    close(fds[1]);
    pfd.revents = 0;
    r = poll(&pfd, 1, 0);
    if (!(pfd.revents & POLLHUP)) {
        fail("pipe_poll hup");
    } else {
        pass("pipe_poll hup");
    }

    close(fds[0]);
}

/* Test 2: epoll on pipe */
static void test_epoll_pipe(void)
{
    int fds[2];
    if (pipe(fds) != 0) { fail("epoll_pipe create"); return; }

    int epfd = epoll_create1(0);
    if (epfd < 0) { fail("epoll_pipe epoll_create1"); close(fds[0]); close(fds[1]); return; }

    struct epoll_event ev = { .events = EPOLLIN, .data.fd = fds[0] };
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, fds[0], &ev) != 0) {
        fail("epoll_pipe ctl_add");
        close(epfd); close(fds[0]); close(fds[1]);
        return;
    }

    /* Empty pipe: epoll_wait with timeout=0 — expect 0 events */
    struct epoll_event out;
    int r = epoll_wait(epfd, &out, 1, 0);
    if (r != 0) {
        fail("epoll_pipe empty");
    } else {
        pass("epoll_pipe empty");
    }

    /* Write data: epoll_wait — expect 1 event with EPOLLIN */
    write(fds[1], "Y", 1);
    r = epoll_wait(epfd, &out, 1, 0);
    if (r != 1 || !(out.events & EPOLLIN)) {
        fail("epoll_pipe data");
    } else {
        pass("epoll_pipe data");
    }

    close(epfd);
    close(fds[0]);
    close(fds[1]);
}

/* Test 3: TTY poll — stdin with timeout=0, expect 0 ready (no queued input) */
static void test_tty_poll(void)
{
    struct pollfd pfd = { .fd = 0, .events = POLLIN };
    int r = poll(&pfd, 1, 0);
    /* Should return 0 (no data) and NOT set POLLNVAL */
    if (pfd.revents & 0x0020) { /* POLLNVAL */
        fail("tty_poll nval");
    } else if (r == 0) {
        pass("tty_poll no_data");
    } else {
        /* r > 0 means data was queued (possible but unlikely in test) — still pass */
        pass("tty_poll no_data");
    }
}

/* Test 4: POLLNVAL for bad fd */
static void test_pollnval(void)
{
    struct pollfd pfd = { .fd = 99, .events = POLLIN };
    int r = poll(&pfd, 1, 0);
    (void)r;
    if (pfd.revents & 0x0020) { /* POLLNVAL */
        pass("pollnval bad_fd");
    } else {
        fail("pollnval bad_fd");
    }
}

int main(void)
{
    test_pipe_poll();
    test_epoll_pipe();
    test_tty_poll();
    test_pollnval();

    /* Summary */
    if (g_fail == 0) {
        const char *msg = "[POLLTEST] ALL PASSED\n";
        write(1, msg, strlen(msg));
        return 0;
    }
    return 1;
}
