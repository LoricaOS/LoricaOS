/* ls — list directory contents.  Supports -l (long format).
 *
 * Long format per entry:
 *   <mode> <nlink> <user> <group> <size> <Mon DD HH:MM|Mon DD  YYYY> <name>
 * with " -> target" appended for symlinks (via readlink).
 *
 * Aegis notes:
 *  - lstat works (syscall 6; musl routes lstat() through SYS_lstat when
 *    fd==AT_FDCWD and flags==AT_SYMLINK_NOFOLLOW).
 *  - The kernel does not populate st_mtime (always 0), so dates show as
 *    "Jan  1  1970" until the kernel copies i_mtime from the ext2 inode.
 *    The standard 6-month recent/old heuristic is implemented anyway so
 *    output becomes correct the moment the kernel fills the field.
 *  - st_nlink is always 1 (kernel hardcodes it); printed as-is.
 *  - User/group names come from musl's getpwuid/getgrgid (/etc/passwd,
 *    /etc/group — same approach as whoami); numeric fallback otherwise.
 */
#include <stdio.h>
#include <dirent.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <pwd.h>
#include <grp.h>
#include <sys/stat.h>

#define SIX_MONTHS_SECS (15552000L) /* 180 days, standard ls heuristic */

static void
mode_string(unsigned m, char out[11])
{
    static const char rwx[] = "rwxrwxrwx";
    int i;
    if      (S_ISDIR(m))  out[0] = 'd';
    else if (S_ISLNK(m))  out[0] = 'l';
    else if (S_ISCHR(m))  out[0] = 'c';
    else if (S_ISBLK(m))  out[0] = 'b';
    else if (S_ISFIFO(m)) out[0] = 'p';
    else if (S_ISSOCK(m)) out[0] = 's';
    else                  out[0] = '-';
    for (i = 0; i < 9; i++)
        out[i + 1] = (m & (1u << (8 - i))) ? rwx[i] : '-';
    if (m & S_ISUID) out[3] = (out[3] == 'x') ? 's' : 'S';
    if (m & S_ISGID) out[6] = (out[6] == 'x') ? 's' : 'S';
    if (m & S_ISVTX) out[9] = (out[9] == 'x') ? 't' : 'T';
    out[10] = '\0';
}

static void
mtime_string(time_t mtime, char *out, size_t outsz)
{
    struct tm tm;
    time_t now = time(NULL);
    gmtime_r(&mtime, &tm); /* date(1) uses gmtime_r too — no TZ on Aegis */
    if (mtime > now + 60 || now - mtime > SIX_MONTHS_SECS)
        strftime(out, outsz, "%b %e  %Y", &tm);
    else
        strftime(out, outsz, "%b %e %H:%M", &tm);
}

static const char *
user_name(unsigned uid, char *buf, size_t bufsz)
{
    struct passwd *pw = getpwuid(uid);
    if (pw && pw->pw_name && pw->pw_name[0])
        return pw->pw_name;
    snprintf(buf, bufsz, "%u", uid);
    return buf;
}

static const char *
group_name(unsigned gid, char *buf, size_t bufsz)
{
    struct group *gr = getgrgid(gid);
    if (gr && gr->gr_name && gr->gr_name[0])
        return gr->gr_name;
    snprintf(buf, bufsz, "%u", gid);
    return buf;
}

/* Print one long-format line.  fullpath is used for lstat/readlink,
 * name is what gets printed.  Returns 0 on success, 1 on stat failure. */
static int
print_long(const char *fullpath, const char *name)
{
    struct stat st;
    char mode[11], tbuf[32], ubuf[16], gbuf[16];
    char target[256];

    if (lstat(fullpath, &st) != 0) {
        perror(fullpath);
        return 1;
    }
    mode_string((unsigned)st.st_mode, mode);
    mtime_string(st.st_mtime, tbuf, sizeof(tbuf));

    printf("%s %2lu %-8s %-8s %8lld %s %s",
           mode,
           (unsigned long)st.st_nlink,
           user_name((unsigned)st.st_uid, ubuf, sizeof(ubuf)),
           group_name((unsigned)st.st_gid, gbuf, sizeof(gbuf)),
           (long long)st.st_size,
           tbuf,
           name);

    if (S_ISLNK(st.st_mode)) {
        ssize_t n = readlink(fullpath, target, sizeof(target) - 1);
        if (n > 0) {
            target[n] = '\0';
            printf(" -> %s", target);
        }
    }
    putchar('\n');
    return 0;
}

int
main(int argc, char **argv)
{
    /* First non-flag argument is the path.  Flag args contribute option
     * letters ('l' is the only one interpreted; others accepted and
     * ignored — preserves the v1.0.5 "ls -l must not be a path" fix).
     * A bare "-" is a path. */
    const char *path = ".";
    int lflag = 0;
    int i;
    for (i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] != '\0') {
            const char *p;
            for (p = argv[i] + 1; *p; p++)
                if (*p == 'l')
                    lflag = 1;
            continue;
        }
        path = argv[i];
        break;
    }

    if (!lflag) {
        /* Short format — unchanged from the original ls. */
        DIR *d = opendir(path);
        if (!d) { perror(path); return 1; }
        struct dirent *e;
        while ((e = readdir(d)) != NULL)
            puts(e->d_name);
        closedir(d);
        return 0;
    }

    /* Long format. */
    struct stat st;
    if (lstat(path, &st) != 0) {
        perror(path);
        return 1;
    }

    if (!S_ISDIR(st.st_mode))
        return print_long(path, path);

    DIR *d = opendir(path);
    if (!d) { perror(path); return 1; }
    int rc = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        char full[512];
        snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
        rc |= print_long(full, e->d_name);
    }
    closedir(d);
    return rc;
}
