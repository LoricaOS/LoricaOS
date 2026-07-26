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
 * NOTE (admin_session gates): captest ships a policy file declaring the
 * ADMIN-tier caps those gates must withhold — `admin DISK_ADMIN` (security
 * review 05 finding 1) plus `admin AUTH` and `admin POWER` (2026-07-24 audit
 * follow-up: both were granted on mere `authenticated`, so every logged-in
 * user's useradd/usermod/userdel held the authority to rewrite another user's
 * credentials). The no-arg run asserts all three are refused; the positive
 * modes assert they are granted once elevated:
 *   captest disk   -> [CAPTEST] disk-elevated: PASS|FAIL  /  [CAPTEST] DISK PASS|FAIL
 *   captest auth   -> [CAPTEST] auth-elevated: PASS|FAIL
 *                     [CAPTEST] power-elevated: PASS|FAIL /  [CAPTEST] AUTH PASS|FAIL
 *
 * WHAT THE BOOT-TIME RUN ACTUALLY PROVES (read before trusting it): vigil
 * launches /bin/selftest -> captest as a oneshot service. That chain never goes
 * through login, so the process is NOT authenticated (authenticated=0). Every
 * ADMIN-tier cap is therefore refused by the `authenticated` condition BEFORE
 * the admin_session gate is ever consulted — the boot-time pass proves the tier
 * is withheld from an UNAUTHENTICATED session, which is weaker than it looks.
 * (The previous comment here claimed this run was "a plain authenticated
 * session". It is not, and the DISK_ADMIN regression test has been passing for
 * that weaker reason since it was written.)
 *
 * To exercise the GATE itself you need authenticated=1, admin_session=0 — i.e.
 * a real login session. Run `captest` from a plain logged-in shell (all three
 * must still be denied), then `captest disk` / `captest auth` after stsh's
 * `admin` builtin (all must now be granted).
 *
 * Each check PASSES when the operation is DENIED (syscall returns < 0). The
 * errno value is not asserted (ENOCAP is aliased to EPERM, and protected-tree
 * writes may surface as EPERM/EACCES/EROFS) — denial is the security property.
 */
#include <stdio.h>
#include <string.h>
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

    /* Positive mode: `captest auth` — the AUTH/POWER counterpart to `disk`.
     * Both are declared ADMIN-tier in caps.d/captest and, since the 2026-07-24
     * audit follow-up, both are admin_session-gated rather than granted on mere
     * `authenticated`. Invoke from an admin-elevated shell: they must now be
     * GRANTED, proving the gate is a real elevation check and not a blanket
     * deny (the negative half lives in the baseline run below). */
    if (argc > 1 && argv[1][0] == 'a') {
        int ok = 1;

        /* AUTH: /etc/shadow is inode-gated on CAP_KIND_AUTH in vfs_open.
         * Read-only — this never mutates the credential file. */
        int fd = open("/etc/shadow", O_RDONLY);
        if (fd >= 0) {
            printf("[CAPTEST] auth-elevated: PASS (allowed)\n");
            close(fd);
        } else {
            printf("[CAPTEST] auth-elevated: FAIL (DENIED) — admin elevation "
                   "did not grant AUTH\n");
            ok = 0;
        }

        /* POWER: sethostname, not reboot — a POWER regression here must not
         * power-cycle the test VM (same reasoning as the baseline suite).
         * Restore the original name afterwards so the probe leaves no trace. */
        char oldhn[128];
        if (gethostname(oldhn, sizeof oldhn) != 0)
            oldhn[0] = '\0';
        if (sethostname("captest-probe", 13) == 0) {
            printf("[CAPTEST] power-elevated: PASS (allowed)\n");
        } else {
            printf("[CAPTEST] power-elevated: FAIL (DENIED) — admin elevation "
                   "did not grant POWER\n");
            ok = 0;
        }
        if (oldhn[0])
            sethostname(oldhn, strlen(oldhn));

        printf("[CAPTEST] AUTH %s\n", ok ? "PASS" : "FAIL");
        return ok ? 0 : 1;
    }

    printf("[CAPTEST] start (baseline-caps probe)\n");

    /* setuid to a foreign identity: no SETUID cap and no authenticated
     * binding -> must be refused (no ambient root by the back door). */
    expect_denied("setuid-foreign", setuid(31337));

    /* setuid to the current uid is a no-op and must be permitted — confirms
     * the deny above is the binding rule, not a blanket refusal. */
    expect_allowed("setuid-noop", setuid(getuid()));

    /* POWER: sethostname needs CAP_KIND_POWER. captest DECLARES `admin POWER`,
     * which since the 2026-07-24 audit follow-up is admin_session-gated — so
     * this run (not elevated) must still be refused. See the header note on
     * what this proves under vigil vs. under a login shell. */
    expect_denied("sethostname-power",
                  sethostname("captest-probe", 13));

    /* NET_SOCKET: an AF_INET socket needs CAP_KIND_NET_SOCKET. (AF_UNIX is
     * intentionally allowed by baseline IPC, so it is not tested here.) */
    expect_denied("socket-inet",
                  socket(AF_INET, SOCK_STREAM, 0));

    /* AUTH: /etc/shadow is gated on CAP_KIND_AUTH at the resolved inode.
     * captest DECLARES `admin AUTH`, admin_session-gated as of the 2026-07-24
     * follow-up, so an unelevated run must still be refused. This is the check
     * that would have caught useradd/usermod holding AUTH on mere login. */
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
