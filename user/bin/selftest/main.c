/* /bin/selftest — boot-time OS self-test launcher.
 *
 * Vigil starts this as a oneshot service on every boot, but it does nothing
 * unless the kernel cmdline carries the `selftest` token (test ISOs only —
 * production boots are untouched). When enabled it execs the userland security
 * probe /bin/captest, whose "[CAPTEST] ALL PASS" line the harness greps for:
 * every privileged operation must be DENIED to an ordinary baseline-cap process
 * (no ambient authority in userland, the OS-side counterpart to the kernel's
 * own capability test).
 */
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

extern char **environ;

int main(void)
{
    char cmd[512];
    int fd = open("/proc/cmdline", O_RDONLY);
    long n = (fd >= 0) ? (long)read(fd, cmd, sizeof(cmd) - 1) : -1;
    if (fd >= 0) close(fd);
    if (n <= 0) return 0;
    cmd[n] = '\0';
    if (!strstr(cmd, "selftest")) return 0;   /* production boot: do nothing */

    dprintf(2, "[SELFTEST] running captest baseline probe\n");
    /* vigil closes stdout (fd 1) for non-login services; captest prints its
     * [CAPTEST] results to stdout. Point fd 1 at the console (fd 2, still open)
     * so the probe's output reaches the serial log the harness greps. */
    dup2(2, 1);
    char *av[] = { "captest", 0 };
    execve("/bin/captest", av, environ);
    dprintf(2, "[SELFTEST] FAIL: could not exec /bin/captest\n");
    return 1;
}
