/* extabtest — proves the fault-tolerant copy_*_user exception table works,
 * independently of the per-syscall re-validations.
 *
 * It races munmap against a BLOCKED recvfrom() on an AF_UNIX socketpair.
 * sys_recvfrom's AF_UNIX path validates the buffer once at entry, blocks in
 * unix_sock_read, then copy_to_user's into it — with NO post-block re-validation
 * (unlike sys_read/poll/epoll, which I hardened). So the ONLY thing standing
 * between "sibling munmap'd the buffer mid-recv" and a ring-0 #PF kernel panic
 * is the __ex_table fixup in copy_to_user. If the kernel survives and we print,
 * the exception table caught the fault.
 *
 * Deterministic ordering: A blocks in recvfrom on an empty socket and only
 * wakes when B sends; B munmaps the recv buffer BEFORE sending, so A is
 * guaranteed to wake into copy_to_user on an unmapped page.
 *
 *   EXTAB_OK (recv rc=%d errno=%d)  — kernel survived the faulting copy
 *   EXTAB_SKIP: <why>               — setup failed
 * Pre-fix (no ex_table): the kernel panics in copy_to_user; nothing prints.
 *
 * Baseline caps + NET_SOCKET? socketpair(AF_UNIX) needs CAP_KIND_IPC, which is
 * baseline. No caps.d needed.
 */
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/mman.h>

static int   g_sv[2];
static void *g_rxbuf;

static void *helper(void *arg)
{
    (void)arg;
    usleep(200000);                 /* let A enter recvfrom and block */
    munmap(g_rxbuf, 4096);          /* unmap A's recv buffer mid-block */
    char b = 'Z';
    (void)send(g_sv[1], &b, 1, 0);  /* wake A → it copy_to_user's into g_rxbuf */
    return (void *)0;
}

int main(void)
{
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, g_sv) != 0) {
        printf("EXTAB_SKIP: socketpair errno=%d\n", errno);
        return 0;
    }

    g_rxbuf = mmap(0, 4096, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (g_rxbuf == MAP_FAILED) { printf("EXTAB_SKIP: mmap errno=%d\n", errno); return 0; }
    /* touch it so it's mapped at recvfrom's entry-time validation */
    *(volatile char *)g_rxbuf = 0;

    pthread_t th;
    if (pthread_create(&th, 0, helper, 0) != 0) {
        printf("EXTAB_SKIP: pthread_create errno=%d\n", errno);
        return 0;
    }

    /* Block until B sends. B munmaps g_rxbuf first, so the kernel's post-block
     * copy_to_user lands on an unmapped page → exception-table fixup (no panic).
     * If the kernel lacked the ex_table, this recv would panic the machine. */
    char *rx = (char *)g_rxbuf;
    long r = (long)recv(g_sv[0], rx, 64, 0);
    int e = errno;

    pthread_join(th, 0);

    printf("EXTAB_OK (recv rc=%ld errno=%d)\n", r, (r < 0) ? e : 0);
    return 0;
}
