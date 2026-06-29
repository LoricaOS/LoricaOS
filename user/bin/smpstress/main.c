/* smpstress — deterministic amplifier for the SMP concurrent-startup crash.
 *
 * The crash (gated behind smp_sched): under -smp 4, ld-musl faults
 * (#GP/#PF, RIP in 0x400xxxxx) while loading a freshly-mapped ELF image
 * during *concurrent* process startup. Normal boot hits it ~3-9%. This
 * pushes the rate up by forking many dynamically-linked children at once
 * so several cores are inside ldso simultaneously.
 *
 * Each round forks BURST children that immediately execve a dynamic
 * binary (/bin/true → ldso runs, relocates, exits). Fork is full-page-copy
 * (no COW) so this also stresses the concurrent fork path. Runs ROUNDS
 * rounds then exits; vigil launches it as a oneshot at boot.
 *
 * Writes progress straight to /dev/console (→ serial): vigil does not wire a
 * oneshot's stdio to the console, so stdio would be invisible to the hunter.
 *
 * ponytail: fixed BURST/ROUNDS — tune via rebuild, not args (it's a probe).
 */
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <string.h>

#define BURST  16
#define ROUNDS 30

static int g_con = 2;

static void
con(const char *s)
{
    write(g_con, s, strlen(s));
}

static void
con_num(int n)
{
    char b[12]; int i = 10; b[11] = 0;
    if (n == 0) { con("0"); return; }
    while (n > 0 && i >= 0) { b[i--] = (char)('0' + n % 10); n /= 10; }
    con(&b[i + 1]);
}

/* Only run under the smp_sched diagnostic boot — inert everywhere else, so
 * the service can ship in every rootfs without firing in production. */
static int
smp_sched_on_cmdline(void)
{
    int fd = open("/proc/cmdline", O_RDONLY);
    if (fd < 0)
        return 0;
    char buf[512];
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return 0;
    buf[n] = 0;
    /* Separate token from smp_sched so the diagnostic SMP boot can run the
     * fix WITHOUT the aggressive amplifier (validate the real-boot bug), and
     * turn the amplifier on independently. */
    return strstr(buf, "smp_stress") != 0;
}

int
main(void)
{
    static const char *child = "/bin/true";
    char *const argv[] = { (char *)child, 0 };

    if (!smp_sched_on_cmdline())
        return 0;

    int c = open("/dev/console", O_WRONLY);
    if (c >= 0)
        g_con = c;

    con("[SMPSTRESS] start\n");

    for (int r = 0; r < ROUNDS; r++) {
        pid_t pids[BURST];
        int forkfail = 0, execfail = 0;
        for (int i = 0; i < BURST; i++) {
            pid_t p = fork();
            if (p == 0) {
                execve(child, argv, 0);
                _exit(127);   /* execve failed */
            }
            if (p < 0) forkfail++;
            pids[i] = p;
        }
        for (int i = 0; i < BURST; i++) {
            if (pids[i] > 0) {
                int st = 0;
                waitpid(pids[i], &st, 0);
                if (WIFEXITED(st) && WEXITSTATUS(st) == 127) execfail++;
            }
        }
        con("[SMPSTRESS] round ");
        con_num(r + 1); con("/"); con_num(ROUNDS);
        if (forkfail) { con(" forkfail="); con_num(forkfail); }
        if (execfail) { con(" execfail="); con_num(execfail); }
        con("\n");
    }
    con("[SMPSTRESS] all rounds clean\n");
    return 0;
}
