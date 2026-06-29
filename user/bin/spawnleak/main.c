/* spawnleak — regression test for the sys_spawn VMA leak.
 *
 * Before the fix, elf_load recorded each spawned child's PT_LOAD + interpreter
 * VMAs into sched_current() (the SPAWNER) instead of the child, so a process
 * that repeatedly sys_spawn'd grew its own vma_table / VmSize without bound
 * (~840 KB per spawn for a dynamically-linked child) and would eventually
 * exhaust its vma_table and be unable to spawn at all. Lumen exhibited this
 * as a per-launcher-open leak.
 *
 * This test spawns ITSELF (a dynamic binary, so the interpreter-VMA path is
 * exercised) N times, waiting on each child, and compares its own VmSize
 * before and after. Post-fix the delta is ~0; pre-fix it was many MB.
 *
 *   parent mode:  print SPAWNLEAK_OK / SPAWNLEAK_FAIL delta=<kB>
 *   child mode (argv[1] == "child"): exit immediately
 */
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#define SYS_SPAWN   514
#define SPAWNS      30
/* Generous ceiling: post-fix delta is ~0; pre-fix was SPAWNS * ~840 kB ≈ 25 MB.
 * Allow slack for musl arena warmup yet stay far below the leak magnitude. */
#define MAX_DELTA_KB 512

static long read_vmsize_kb(void)
{
    int fd = open("/proc/self/status", O_RDONLY);
    if (fd < 0) return -1;
    char buf[1024];
    int n = (int)read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';
    char *p = strstr(buf, "VmSize:");
    if (!p) return -1;
    p += 7;
    while (*p == ' ' || *p == '\t') p++;
    return atol(p);
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "child") == 0)
        return 0;  /* child mode: exit immediately */

    char *cargv[] = { "/bin/spawnleak", "child", NULL };
    char *cenvp[] = { "PATH=/bin", NULL };

    /* Warm up musl's arena with one spawn before the baseline so first-touch
     * allocation growth isn't counted as a leak. */
    long pid = syscall(SYS_SPAWN, "/bin/spawnleak", cargv, cenvp, 1L, 0L);
    if (pid < 0) { printf("SPAWNLEAK_FAIL spawn errno=%ld\n", -pid); return 1; }
    waitpid((int)pid, NULL, 0);

    long before = read_vmsize_kb();

    for (int i = 0; i < SPAWNS; i++) {
        pid = syscall(SYS_SPAWN, "/bin/spawnleak", cargv, cenvp, 1L, 0L);
        if (pid < 0) {
            /* vma_table exhaustion (the eventual end-state of the leak) also
             * surfaces here as a spawn failure. */
            printf("SPAWNLEAK_FAIL spawn#%d errno=%ld\n", i, -pid);
            return 1;
        }
        waitpid((int)pid, NULL, 0);
    }

    long after = read_vmsize_kb();
    long delta = (before >= 0 && after >= 0) ? after - before : -1;

    if (delta < 0)
        printf("SPAWNLEAK_FAIL vmsize-read before=%ld after=%ld\n", before, after);
    else if (delta > MAX_DELTA_KB)
        printf("SPAWNLEAK_FAIL delta=%ldkB over %d spawns\n", delta, SPAWNS);
    else
        printf("SPAWNLEAK_OK delta=%ldkB over %d spawns\n", delta, SPAWNS);
    return 0;
}
