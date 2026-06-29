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

/* ---- passwd/shadow lookup --------------------------------------------- */

int
auth_lookup_passwd(const char *username, int *uid, int *gid,
                   char *home, int homelen, char *shell, int shelllen)
{
    FILE *fp = fopen("/etc/passwd", "r");
    if (!fp) return -1;
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
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
        *uid = atoi(fields[2]);
        *gid = atoi(fields[3]);
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
    int fd = open("/etc/shadow", O_RDONLY);
    if (fd < 0) return -1;
    char filebuf[2048];
    int total = 0, n;
    while ((n = (int)read(fd, filebuf + total,
                          (size_t)((int)sizeof(filebuf) - total - 1))) > 0)
        total += n;
    close(fd);
    filebuf[total] = '\0';

    char *line = filebuf;
    while (line && *line) {
        char *end = strchr(line, '\n');
        if (end) *end = '\0';
        char *colon = strchr(line, ':');
        if (colon) {
            *colon = '\0';
            if (strcmp(line, username) == 0) {
                char *h = colon + 1;
                char *hend = strchr(h, ':');
                if (hend) *hend = '\0';
                strncpy(hash, h, (size_t)hashlen - 1);
                hash[hashlen - 1] = '\0';
                return 0;
            }
            *colon = ':';
        }
        if (!end) break;
        line = end + 1;
    }
    return -1;
}

/* ---- verification ----------------------------------------------------- */

int
auth_verify(const char *password, const char *hash)
{
    char *computed = crypt(password, hash);
    if (!computed || strcmp(computed, hash) != 0)
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

void
auth_set_identity(int uid, int gid)
{
    syscall(105, (long)uid);  /* sys_setuid */
    syscall(106, (long)gid);  /* sys_setgid */
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

void
auth_elevate_session(int uid, int gid)
{
    /* sys_auth_session — marks session authenticated AND binds (uid, gid) as
     * the only identity a later setuid/setgid may assume. */
    syscall(364, (long)uid, (long)gid);
}
