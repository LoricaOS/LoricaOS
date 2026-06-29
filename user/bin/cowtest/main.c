/* cowtest — copy-on-write cross-process isolation regression probe.
 *
 * Targets the HIGH memory-corruption bug where vmm_set_user_prot (sys_mprotect)
 * rebuilt a leaf PTE from PROT_* hardware flags only, discarding the software
 * bits VMM_FLAG_COW / VMM_FLAG_SHARED. After a COW fork, an mprotect(RW) on a
 * still-shared (RO+COW) page turned it into a WRITABLE, NON-COW PTE on the same
 * physical frame whose refcount was still >= 2 — so a subsequent write went
 * straight into the frame with no fault, no copy, and BOTH address spaces then
 * aliased one live writable frame (cross-process read/write).
 *
 * The probe forks (COW), has the parent mprotect(PROT_READ|PROT_WRITE) a page
 * that was writable before the fork, writes a sentinel, and asserts the child
 * never observes the parent's write (and, in reverse, the parent never observes
 * the child's). With the bug present the child would read the parent's sentinel.
 *
 * A pipe provides lockstep + an out-of-band channel for each side to report
 * what it actually read in its own address space.
 *
 * Prints exactly one summary line the harness asserts on:
 *   [COWTEST] ALL PASS (n/n)   — address spaces stayed isolated across mprotect
 *   [COWTEST] FAIL (p/n)       — a write leaked across the fork boundary (BUG)
 * plus one "[COWTEST] <name>: PASS|FAIL" line per check for diagnosis.
 */
#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>

#define VAL_PREFORK 0x11111111u  /* written before fork — the COW snapshot */
#define VAL_PARENT  0xDEADBEEFu  /* parent writes after mprotect(RW)        */
#define VAL_CHILD   0xCAFEBABEu  /* child writes into its own copy          */

static int g_pass;
static int g_total;

static void
check(const char *name, int ok)
{
    g_total++;
    if (ok) {
        g_pass++;
        printf("[COWTEST] %s: PASS\n", name);
    } else {
        printf("[COWTEST] %s: FAIL\n", name);
    }
}

/* Blocking full read/write of n bytes over a pipe (handles short I/O). */
static int
full_io(int fd, void *buf, size_t n, int writing)
{
    uint8_t *p = buf;
    size_t done = 0;
    while (done < n) {
        ssize_t r = writing ? write(fd, p + done, n - done)
                            : read(fd, p + done, n - done);
        if (r <= 0)
            return -1;
        done += (size_t)r;
    }
    return 0;
}

int
main(void)
{
    printf("[COWTEST] start (COW cross-process isolation probe)\n");

    volatile uint32_t *page =
        mmap(NULL, 4096, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) {
        printf("[COWTEST] FAIL (0/1) — mmap failed\n");
        return 1;
    }
    *page = VAL_PREFORK;          /* shared snapshot, COW-forked below */

    int p2c[2], c2p[2];           /* parent->child sync, child->parent data */
    if (pipe(p2c) < 0 || pipe(c2p) < 0) {
        printf("[COWTEST] FAIL (0/1) — pipe failed\n");
        return 1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        printf("[COWTEST] FAIL (0/1) — fork failed\n");
        return 1;
    }

    if (pid == 0) {
        /* ---- child ---- */
        close(p2c[1]);
        close(c2p[0]);
        char sync = 0;
        uint32_t observed;

        /* Phase 1: wait for parent's post-mprotect write, then report what
         * THIS address space sees at the same VA. */
        full_io(p2c[0], &sync, 1, 0);
        observed = *page;
        full_io(c2p[1], &observed, sizeof(observed), 1);

        /* Phase 2: write our own distinct value (breaks COW on the child
         * side), then tell the parent so it can re-read its own copy. */
        *page = VAL_CHILD;
        full_io(c2p[1], &sync, 1, 1);
        _exit(0);
    }

    /* ---- parent ---- */
    close(p2c[0]);
    close(c2p[1]);
    char sync = 1;
    uint32_t child_saw = 0;

    /* THE attack path: re-protect the still-shared (RO+COW) page writable.
     * Pre-fix this stripped COW and made the shared frame writable in place. */
    int mp = mprotect((void *)page, 4096, PROT_READ | PROT_WRITE);
    check("mprotect-rw", mp == 0);

    *page = VAL_PARENT;           /* with the fix: faults -> COW copy */
    full_io(p2c[1], &sync, 1, 1); /* tell child the write is done */

    /* What did the child observe at the same VA in its own address space? */
    if (full_io(c2p[0], &child_saw, sizeof(child_saw), 0) < 0)
        child_saw = 0xFFFFFFFFu;

    /* Child must NOT see the parent's post-mprotect write. */
    check("child-isolated-from-parent", child_saw != VAL_PARENT);
    /* And it should still see the fork-time snapshot. */
    check("child-sees-snapshot", child_saw == VAL_PREFORK);

    /* Phase 2: let the child write VAL_CHILD, then confirm the parent's own
     * copy is untouched (reverse direction of the leak). */
    full_io(c2p[0], &sync, 1, 0);
    check("parent-isolated-from-child", *page == VAL_PARENT);

    int status = 0;
    waitpid(pid, &status, 0);

    if (g_pass == g_total)
        printf("[COWTEST] ALL PASS (%d/%d)\n", g_pass, g_total);
    else
        printf("[COWTEST] FAIL (%d/%d)\n", g_pass, g_total);
    return g_pass == g_total ? 0 : 1;
}
