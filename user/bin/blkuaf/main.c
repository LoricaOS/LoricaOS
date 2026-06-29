/* blkuaf — deterministic reproducer for the validate-then-block-then-copy TOCTOU
 * (the kernel-panic class fixed in epoll_wait/sys_poll/sys_read/sys_pread64).
 *
 * A blocking syscall validates its user buffer once at entry, then blocks; if a
 * sibling thread munmaps that buffer during the block, the kernel's post-wake
 * copy_to_user/copy_from_user (a raw memcpy, no fault fixup) would #PF in ring 0
 * and PANIC. The fix re-validates the buffer after the block and returns EFAULT.
 *
 * The race is made DETERMINISTIC: the poller blocks on an EMPTY pipe and only
 * wakes when the helper thread writes it. The helper munmaps the pollfd buffer
 * FIRST, then writes — so the poller is guaranteed to wake to an already-unmapped
 * buffer. On a fixed kernel poll() returns EFAULT and the process lives; on a
 * pre-fix kernel the kernel panics and the VM dies.
 *
 *   BLKUAF_OK              — poll returned (EFAULT or otherwise); kernel survived
 *   BLKUAF_SKIP: <why>     — setup failed
 * On a pre-fix kernel neither prints (panic).
 *
 * Baseline caps only.
 */
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <poll.h>
#include <sys/mman.h>

static int   g_pipe[2];
static void *g_pollbuf;      /* mmap'd page holding the pollfd the poller passes */

static void *helper(void *arg)
{
    (void)arg;
    /* Give the main thread time to enter poll() and block on the empty pipe. */
    usleep(200000);   /* 200 ms */
    /* Unmap the pollfd buffer WHILE the poller is blocked on it. */
    munmap(g_pollbuf, 4096);
    /* Now make the pipe readable → wakes the poller, which re-touches the
     * now-unmapped pollfd buffer in the kernel. */
    char b = 'x';
    (void)write(g_pipe[1], &b, 1);
    return (void *)0;
}

int main(void)
{
    if (pipe(g_pipe) != 0) { printf("BLKUAF_SKIP: pipe errno=%d\n", errno); return 0; }

    g_pollbuf = mmap(0, 4096, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (g_pollbuf == MAP_FAILED) { printf("BLKUAF_SKIP: mmap errno=%d\n", errno); return 0; }

    /* Place a pollfd watching the read end for POLLIN in the mmap'd buffer. */
    struct pollfd *pfd = (struct pollfd *)g_pollbuf;
    pfd->fd      = g_pipe[0];
    pfd->events  = POLLIN;
    pfd->revents = 0;

    pthread_t th;
    if (pthread_create(&th, 0, helper, 0) != 0) {
        printf("BLKUAF_SKIP: pthread_create errno=%d\n", errno);
        return 0;
    }

    /* Block here on the empty pipe. The helper munmaps g_pollbuf then writes the
     * pipe, so poll() wakes and the kernel re-touches the unmapped pfd. On the
     * fixed kernel this returns EFAULT; pre-fix it panics. Either way, if we
     * print below, the kernel survived. */
    int r = poll(pfd, 1, 5000);
    int e = errno;

    pthread_join(th, 0);

    printf("BLKUAF_OK (poll rc=%d errno=%d)\n", r, (r < 0) ? e : 0);
    return 0;
}
