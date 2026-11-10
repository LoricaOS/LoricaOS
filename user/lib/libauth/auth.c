/* auth.c — shared authentication library for Aegis.
 *
 * Provides credential verification against /etc/passwd + /etc/shadow,
 * identity switching (setuid/setgid), and session elevation for the
 * kernel capability policy table. Used by both /bin/login and
 * /bin/bastion.
 */
#include "auth.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/syscall.h>
#include <crypt.h>
#include <errno.h>
#include <limits.h>

/* ---- passwd/shadow lookup --------------------------------------------- */

static int parse_identity(const char *s, int *out)
{
    char *end;
    if (!s || !s[0] || s[0] == '-' || s[0] == '+')
        return -1;
    for (const char *p = s; *p; p++)
        if (*p < '0' || *p > '9') return -1;
    errno = 0;
    long n = strtol(s, &end, 10);
    if (errno || *end || n > INT_MAX)
        return -1;
    *out = (int)n;
    return 0;
}

int
auth_lookup_passwd(const char *username, int *uid, int *gid,
                   char *home, int homelen, char *shell, int shelllen)
{
    if (!username || !uid || !gid || !home || homelen <= 0 ||
        !shell || shelllen <= 0)
        return -1;
    FILE *fp = fopen("/etc/passwd", "r");
    if (!fp) return -1;
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        if (!strchr(line, '\n') && !feof(fp)) {
            fclose(fp);
            return -1; /* never authenticate a truncated passwd entry */
        }
        char *fields[7];
        char *p = line, *q;
        int f = 0;
        while (f < 7 && (q = strchr(p, ':'))) {
            *q = '\0';
            fields[f++] = p;
            p = q + 1;
        }
        if (f == 6) {
            q = strchr(p, '\n');
            if (q) *q = '\0';
            fields[f++] = p;
        }
        if (f < 7) continue;
        if (strcmp(fields[0], username) != 0) continue;
        if (parse_identity(fields[2], uid) != 0 ||
            parse_identity(fields[3], gid) != 0 ||
            fields[5][0] != '/' || fields[6][0] != '/' ||
            strlen(fields[5]) >= (size_t)homelen ||
            strlen(fields[6]) >= (size_t)shelllen) {
            fclose(fp);
            return -1;
        }
        strncpy(home,  fields[5], (size_t)homelen  - 1); home[homelen-1]  = '\0';
        strncpy(shell, fields[6], (size_t)shelllen - 1); shell[shelllen-1] = '\0';
        fclose(fp);
        return 0;
    }
    fclose(fp);
    return -1;
}

int
auth_lookup_shadow(const char *username, char *hash, int hashlen)
{
    if (!username || !hash || hashlen <= 0)
        return -1;
    /* Scan /etc/shadow line-by-line rather than slurping it into a fixed buffer.
     * The old 2048-byte read silently truncated the file past ~19 SHA-512
     * accounts, so trailing users could not log in with a correct password (and
     * a cut mid-line risked a corrupt compare). One shadow entry is far under
     * 1 KB (username + $6$ hash + aging fields), so a per-line buffer handles
     * any number of accounts with bounded memory. */
    FILE *fp = fopen("/etc/shadow", "r");
    if (!fp) return -1;
    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        char *colon = strchr(line, ':');
        if (!colon) continue;
        *colon = '\0';
        if (strcmp(line, username) != 0) continue;
        char *h = colon + 1;
        char *hend = strchr(h, ':');
        if (hend) *hend = '\0';
        if ((!nl && !hend) || strlen(h) >= (size_t)hashlen) {
            fclose(fp);
            return -1;
        }
        strncpy(hash, h, (size_t)hashlen - 1);
        hash[hashlen - 1] = '\0';
        fclose(fp);
        return 0;
    }
    fclose(fp);
    return -1;
}

/* ---- verification ----------------------------------------------------- */

int
auth_verify(const char *password, const char *hash)
{
    if (!password || !hash || !hash[0] || hash[0] == '!' || hash[0] == '*')
        return -1;
    char *computed = crypt(password, hash);
    if (!computed || computed[0] == '*' || strcmp(computed, hash) != 0)
        return -1;
    return 0;
}

int
auth_check(const char *username, const char *password,
           int *uid, int *gid,
           char *home, int homelen, char *shell, int shelllen)
{
    if (auth_lookup_passwd(username, uid, gid, home, homelen, shell, shelllen) != 0)
        return -1;
    char hash[256];
    if (auth_lookup_shadow(username, hash, (int)sizeof(hash)) != 0)
        return -1;
    return auth_verify(password, hash);
}

/* ---- identity + caps -------------------------------------------------- */

int
auth_set_identity(int uid, int gid)
{
    /* Binding and both switches must belong to this same process. Change the
     * group first; do not attempt the uid switch if that failed. */
    if (uid < 0 || gid < 0 || syscall(106, (long)gid) != 0)
        return -1;
    return syscall(105, (long)uid) == 0 ? 0 : -1;
}

void
auth_grant_shell_caps(void)
{
    /* No-op. Pre-46c this issued sys_cap_grant_exec (syscall 361) to
     * pre-register shell capabilities; the Phase 46c cap-policy redesign
     * removed that syscall (it now returns ENOSYS) and moved capability
     * declaration into /etc/aegis/caps.d/<binary>, read by the kernel at
     * execve time. The four syscalls here were dead ENOSYS round-trips on
     * every login/unlock. The symbol is kept so existing callers in
     * libauth / libcitadel need not change in lockstep. */
}

int
auth_elevate_session(int uid, int gid)
{
    /* sys_auth_session — marks session authenticated AND binds (uid, gid) as
     * the only identity a later setuid/setgid may assume. */
    if (uid < 0 || gid < 0)
        return -1;
    return syscall(364, (long)uid, (long)gid) == 0 ? 0 : -1;
}
