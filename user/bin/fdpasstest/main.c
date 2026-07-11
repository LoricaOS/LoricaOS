/* fdpasstest — regression test for the SCM_RIGHTS authority-laundering guard.
 *
 * Aegis refuses to pass an fd carrying filesystem/device authority across a
 * process boundary via SCM_RIGHTS (kernel scm_fd_passable allowlist): only
 * no-authority IPC/shared-memory fds — memfd, pipe, unix socket — may cross.
 * This proves BOTH directions on a real boot:
 *   (1) a memfd IS passable (must not break — lumen passes window surfaces),
 *   (2) an ext2 file fd is REFUSED with EPERM (the confused-deputy privesc).
 *
 * Prints exactly one summary line the harness asserts on:
 *   FDPASS_OK             — memfd passed AND file fd refused (fixed kernel)
 *   FDPASS_FAIL: <why>    — memfd blocked, or a file fd was laundered through
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/mman.h>

static int done(const char *line) { printf("%s\n", line); fflush(stdout); return line[7] == 'O' ? 0 : 1; }

/* Send one fd over sv[0] with a 1-byte payload; return sendmsg result / -errno. */
static long send_fd(int sock, int fd)
{
    char payload = 'x';
    struct iovec iov = { &payload, 1 };
    union { struct cmsghdr h; char buf[CMSG_SPACE(sizeof(int))]; } u;
    memset(&u, 0, sizeof u);
    struct msghdr mh = {0};
    mh.msg_iov = &iov; mh.msg_iovlen = 1;
    mh.msg_control = u.buf; mh.msg_controllen = CMSG_SPACE(sizeof(int));
    struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
    c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS; c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &fd, sizeof(int));
    long r = sendmsg(sock, &mh, 0);
    return r < 0 ? -errno : r;
}

int main(void)
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0)
        return done("FDPASS_FAIL: socketpair");

    /* (1) memfd MUST pass. */
    int mfd = memfd_create("fdpass", 0);
    if (mfd < 0) return done("FDPASS_FAIL: memfd_create");
    if (ftruncate(mfd, 4096) != 0) return done("FDPASS_FAIL: ftruncate");
    if (send_fd(sv[0], mfd) < 0)
        return done("FDPASS_FAIL: memfd was refused (would break lumen)");
    /* receive it */
    {
        char payload; struct iovec iov = { &payload, 1 };
        union { struct cmsghdr h; char buf[CMSG_SPACE(sizeof(int))]; } u;
        memset(&u, 0, sizeof u);
        struct msghdr mh = {0};
        mh.msg_iov = &iov; mh.msg_iovlen = 1;
        mh.msg_control = u.buf; mh.msg_controllen = CMSG_SPACE(sizeof(int));
        if (recvmsg(sv[1], &mh, 0) < 0) return done("FDPASS_FAIL: recvmsg memfd");
        struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
        if (!c || c->cmsg_type != SCM_RIGHTS) return done("FDPASS_FAIL: no memfd received");
        int rfd; memcpy(&rfd, CMSG_DATA(c), sizeof(int));
        void *p = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, rfd, 0);
        if (p == MAP_FAILED) return done("FDPASS_FAIL: received memfd not mmappable");
        close(rfd);
    }

    /* (2) an ext2 file fd MUST be refused (EPERM). /etc/passwd is a plain
     * ext2 file, world-readable — opening it needs no special cap, so this
     * isolates the SCM_RIGHTS guard from the open-time gate. */
    int ffd = open("/etc/passwd", O_RDONLY);
    if (ffd < 0) return done("FDPASS_FAIL: open /etc/passwd");
    long r = send_fd(sv[0], ffd);
    close(ffd);
    if (r >= 0)
        return done("FDPASS_FAIL: ext2 file fd was LAUNDERED through SCM_RIGHTS");
    if (r != -EPERM)
        return done("FDPASS_FAIL: file fd refused but not with EPERM");

    return done("FDPASS_OK");
}
