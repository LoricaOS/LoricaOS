/* captest — privilege-escalation / no-ambient-authority regression probe (T2).
 *
 * Run from a plain shell, captest execs as an ordinary /bin binary with NO
 * policy file, so it receives only BASELINE caps (VFS_OPEN/READ/WRITE, IPC,
 * PROC_READ, THREAD_CREATE). The whole point of Aegis's capability model is
 * that such a process — even running as uid 0 — holds NO authority beyond the
 * baseline. This probe attempts every privileged operation a baseline process
 * must be denied, plus the install-protected-tree writes that back the
 * trusted-path anchor, and confirms each is refused.
 *
 * It deliberately avoids irreversible actions: it never calls reboot (a POWER
 * regression would reset the test VM); the POWER check uses sethostname, which
 * on the expected denial changes nothing, and on a regression only changes the
 * hostname (harmless, detectable) instead of power-cycling.
 *
 * Prints exactly one summary line the harness asserts on:
 *   [CAPTEST] ALL PASS (n/n)       — every privileged op was correctly denied
 *   [CAPTEST] FAIL (p/n)           — at least one op was allowed (a bypass!)
 * plus one "[CAPTEST] <name>: PASS|FAIL" line per check for diagnosis.
 *
 * NOTE (DISK_ADMIN gate, security review 05): captest now ships a policy file
 * (caps.d/captest = `admin DISK_ADMIN`). That ADMIN-tier cap is gated behind an
 * admin_session, so the no-arg baseline run (a plain authenticated session) must
 * STILL be denied blkdev-list — proving the gate withholds DISK_ADMIN from mere
 * login. The companion mode `captest disk` runs the SINGLE positive check: after
 * the shell has been `admin`-elevated, DISK_ADMIN must now be granted, so
 * blkdev-list must SUCCEED — proving the gate is a real elevation check, not a
 * blanket deny. It prints:
 *   [CAPTEST] disk-elevated: PASS|FAIL
 *   [CAPTEST] DISK PASS|FAIL
 *
 * Each check PASSES when the operation is DENIED (syscall returns < 0). The
 * errno value is not asserted (ENOCAP is aliased to EPERM, and protected-tree
 * writes may surface as EPERM/EACCES/EROFS) — denial is the security property.
 */
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/syscall.h>

#define SYS_BLKDEV_LIST 510
#define SYS_FB_FLUSH    515

static int g_pass;
static int g_total;

/* A privileged op must FAIL (rc < 0) to pass the check. */
static void
expect_denied(const char *name, long rc)
{
    g_total++;
    if (rc < 0) {
        g_pass++;
        printf("[CAPTEST] %s: PASS (denied)\n", name);
    } else {
        printf("[CAPTEST] %s: FAIL (ALLOWED rc=%ld) — BYPASS\n", name, rc);
    }
}

/* A benign no-op must SUCCEED (rc == 0) — proves the rule isn't blanket-deny. */
static void
expect_allowed(const char *name, long rc)
{
    g_total++;
    if (rc == 0) {
        g_pass++;
        printf("[CAPTEST] %s: PASS (allowed)\n", name);
    } else {
        printf("[CAPTEST] %s: FAIL (rc=%ld)\n", name, rc);
    }
}

int
main(int argc, char **argv)
{
    /* Positive mode: `captest disk` — run ONLY the elevated DISK_ADMIN check.
     * Expected to be invoked from an admin-elevated shell, where DISK_ADMIN
     * (declared in caps.d/captest) is now granted, so blkdev-list must SUCCEED. */
    if (argc > 1 && argv[1][0] == 'd') {
        char buf[64];
        long rc = syscall(SYS_BLKDEV_LIST, buf, (long)sizeof(buf));
        /* rc >= 0 == device count == DISK_ADMIN was granted. */
        if (rc >= 0) {
            printf("[CAPTEST] disk-elevated: PASS (allowed rc=%ld)\n", rc);
            printf("[CAPTEST] DISK PASS\n");
            return 0;
        }
        printf("[CAPTEST] disk-elevated: FAIL (DENIED rc=%ld) — admin "
               "elevation did not grant DISK_ADMIN\n", rc);
        printf("[CAPTEST] DISK FAIL\n");
        return 1;
    }

    printf("[CAPTEST] start (baseline-caps probe)\n");

    /* setuid to a foreign identity: no SETUID cap and no authenticated
     * binding -> must be refused (no ambient root by the back door). */
    expect_denied("setuid-foreign", setuid(31337));

    /* setuid to the current uid is a no-op and must be permitted — confirms
     * the deny above is the binding rule, not a blanket refusal. */
    expect_allowed("setuid-noop", setuid(getuid()));

    /* POWER: sethostname needs CAP_KIND_POWER. Baseline lacks it. */
    expect_denied("sethostname-power",
                  sethostname("captest-probe", 13));

    /* NET_SOCKET: an AF_INET socket needs CAP_KIND_NET_SOCKET. (AF_UNIX is
     * intentionally allowed by baseline IPC, so it is not tested here.) */
    expect_denied("socket-inet",
                  socket(AF_INET, SOCK_STREAM, 0));

    /* AUTH: /etc/shadow is gated on CAP_KIND_AUTH at the resolved inode. */
    expect_denied("open-shadow",
                  open("/etc/shadow", O_RDONLY));

    /* INSTALL / trusted-path protection: creating a file in /bin must require
     * CAP_KIND_INSTALL — this is what makes the /bin granting anchor
     * unforgeable. */
    expect_denied("create-in-bin",
                  open("/bin/.captest_forge", O_CREAT | O_WRONLY, 0644));

    /* /sbin must be just as protected as /bin (it is also a granting anchor).
     * If /sbin were absent/unprotected this would let an attacker stage a
     * forged binary there and inherit caps by basename. */
    expect_denied("create-in-sbin",
                  open("/sbin/.captest_forge", O_CREAT | O_WRONLY, 0644));

    /* /etc/aegis (policy + anchors live here) must be install-protected too. */
    expect_denied("create-in-etc-aegis",
                  open("/etc/aegis/.captest_forge", O_CREAT | O_WRONLY, 0644));

    /* FB: sys_fb_flush now requires CAP_KIND_FB (T1 cap-completeness fix). A
     * baseline process must not be able to drive scanout presentation. */
    expect_denied("fb-flush", syscall(SYS_FB_FLUSH, 0L));

    /* DISK_ADMIN: raw block-device enumeration needs CAP_KIND_DISK_ADMIN.
     * captest's policy DECLARES `admin DISK_ADMIN`, but DISK_ADMIN is gated
     * behind admin_session (security review 05) — this baseline run is merely
     * authenticated, not admin-elevated, so it must STILL be denied. (Before the
     * gate, a policy-declared DISK_ADMIN was granted on `authenticated` and this
     * check would FAIL — that is the regression this guards.) */
    {
        char buf[64];
        expect_denied("blkdev-list",
                      syscall(SYS_BLKDEV_LIST, buf, (long)sizeof(buf)));
    }

    if (g_pass == g_total)
        printf("[CAPTEST] ALL PASS (%d/%d)\n", g_pass, g_total);
    else
        printf("[CAPTEST] FAIL (%d/%d)\n", g_pass, g_total);
    return g_pass == g_total ? 0 : 1;
}
