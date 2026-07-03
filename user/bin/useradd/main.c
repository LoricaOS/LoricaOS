/* useradd — create a login account (and, by default, its home directory).
 *
 * LoricaOS has no ambient root: creating an account is a privileged action
 * that writes /etc/shadow (kernel gates any open of it on CAP_KIND_AUTH) and
 * chowns the new home (CAP_KIND_SETUID). caps.d/useradd is therefore admin
 * tier with AUTH + SETUID — it only works from an admin-elevated session.
 *
 * The primary user (uid 0) is created by the installer; useradd adds further
 * users, assigned the next free uid at/above 1000, each in a private group
 * (gid = uid). A home is created unless -M is given (LoricaOS creates a home
 * whenever an account is created; -m is accepted for familiarity and implied).
 *
 *   useradd [-m|-M] [-s <shell>] <username>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include "libinstall.h"

#ifndef O_APPEND
#define O_APPEND 0x400
#endif

#define DEFAULT_SHELL "/bin/stsh"
#define UID_MIN       1000

/* Prompt with echo off; returns length. (Same pattern as adminpw/installer.) */
static int read_password(const char *prompt, char *buf, int bufsz)
{
    struct termios t;
    int have_tty = (tcgetattr(0, &t) == 0);
    struct termios t_raw = t;
    if (have_tty) {
        t_raw.c_lflag &= ~(unsigned)(ECHO | ICANON);
        tcsetattr(0, TCSANOW, &t_raw);
    }
    printf("%s", prompt);
    fflush(stdout);
    int i = 0;
    char c;
    while (i < bufsz - 1 && read(0, &c, 1) == 1) {
        if (c == '\n' || c == '\r') break;
        if (c == '\b' || c == 127) {
            if (i > 0) { i--; write(1, "\b \b", 3); }
            continue;
        }
        buf[i++] = c;
        write(1, "*", 1);
    }
    buf[i] = '\0';
    printf("\n");
    if (have_tty)
        tcsetattr(0, TCSANOW, &t);
    return i;
}

/* Scan /etc/passwd. Sets *taken=1 if `username` already has a line, and
 * *max_uid to the highest uid seen at/above UID_MIN (so the next account gets
 * a fresh id without colliding with an existing one). Returns 0, or -1 if
 * /etc/passwd can't be read. */
static int scan_passwd(const char *username, int *taken, int *max_uid)
{
    *taken = 0;
    *max_uid = UID_MIN - 1;
    FILE *f = fopen("/etc/passwd", "r");
    if (!f)
        return -1;
    char line[512];
    size_t ulen = strlen(username);
    while (fgets(line, sizeof(line), f)) {
        /* name is up to the first ':'. */
        char *colon = strchr(line, ':');
        if (!colon)
            continue;
        if ((size_t)(colon - line) == ulen &&
            strncmp(line, username, ulen) == 0)
            *taken = 1;
        /* uid is the 3rd field: name:x:uid:... */
        char *p = colon + 1;                 /* password field */
        p = strchr(p, ':');
        if (!p) continue;
        int uid = atoi(p + 1);
        if (uid >= UID_MIN && uid > *max_uid)
            *max_uid = uid;
    }
    fclose(f);
    return 0;
}

/* Append one line to `path` (create if absent). Returns 0 / -1. */
static int append_line(const char *path, const char *line)
{
    int fd = open(path, O_WRONLY | O_APPEND | O_CREAT);
    if (fd < 0)
        return -1;
    size_t n = strlen(line);
    int rc = (write(fd, line, n) == (ssize_t)n) ? 0 : -1;
    close(fd);
    return rc;
}

int main(int argc, char **argv)
{
    /* Restore cooked mode — stsh leaves the terminal raw. */
    struct termios cooked;
    if (tcgetattr(0, &cooked) == 0) {
        cooked.c_lflag |= (unsigned)(ECHO | ICANON | ISIG);
        tcsetattr(0, TCSANOW, &cooked);
    }

    int make_home = 1;                         /* default: always create home */
    const char *shell = DEFAULT_SHELL;
    const char *username = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-m") == 0)        make_home = 1;   /* implied */
        else if (strcmp(argv[i], "-M") == 0)   make_home = 0;
        else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc)
            shell = argv[++i];
        else if (argv[i][0] == '-') {
            fprintf(stderr, "useradd: unknown option '%s'\n", argv[i]);
            return 2;
        } else if (!username) {
            username = argv[i];
        } else {
            fprintf(stderr, "useradd: unexpected argument '%s'\n", argv[i]);
            return 2;
        }
    }

    if (!username) {
        fprintf(stderr, "usage: useradd [-m|-M] [-s <shell>] <username>\n");
        return 2;
    }
    if (!install_username_valid(username)) {
        fprintf(stderr, "useradd: invalid username "
                        "(need [a-z_][a-z0-9_-]*, max 31)\n");
        return 1;
    }

    /* Creating an account is a privileged action, and the caps useradd needs
     * (AUTH to write /etc/shadow, SETUID to chown the home) are granted to any
     * *authenticated* session — not only an elevated one. LoricaOS has no
     * ambient root, so gate on proving the admin credential: `login -elevate`
     * verifies /etc/aegis/admin and elevates this process. Fails closed if the
     * caller can't authenticate as admin. (This tool is on the trusted path in
     * /bin, so a non-admin cannot tamper with the check.) */
    if (install_elevate(NULL) < 0) {
        fprintf(stderr, "useradd: admin authorization required.\n");
        return 1;
    }

    int taken = 0, max_uid = 0;
    if (scan_passwd(username, &taken, &max_uid) < 0) {
        fprintf(stderr, "useradd: cannot read /etc/passwd\n");
        return 1;
    }
    if (taken) {
        fprintf(stderr, "useradd: user '%s' already exists\n", username);
        return 1;
    }
    int uid = max_uid + 1;                      /* first free id >= UID_MIN */

    char pw[128], confirm[128];
    int rc = 1;
    if (read_password("New password: ", pw, sizeof(pw)) <= 0) {
        fprintf(stderr, "useradd: empty password\n");
        goto out;
    }
    if (read_password("Retype password: ", confirm, sizeof(confirm)) <= 0 ||
        strcmp(pw, confirm) != 0) {
        fprintf(stderr, "useradd: passwords do not match\n");
        goto out;
    }

    char hash[256];
    if (install_hash_password(pw, hash, sizeof(hash)) < 0) {
        fprintf(stderr, "useradd: crypt() failed\n");
        goto out;
    }

    /* Append the account lines. passwd/group are ordinary files; shadow is
     * AUTH-gated by the kernel. Written before the home so a chown failure
     * doesn't leave a half-registered account with no credentials. */
    char line[600];
    snprintf(line, sizeof(line), "%s:x:%d:%d:%s:/home/%s:%s\n",
             username, uid, uid, username, username, shell);
    if (append_line("/etc/passwd", line) < 0) {
        fprintf(stderr, "useradd: write /etc/passwd failed\n");
        goto out;
    }
    snprintf(line, sizeof(line), "%s:%s:19814:0:99999:7:::\n", username, hash);
    if (append_line("/etc/shadow", line) < 0) {
        fprintf(stderr, "useradd: write /etc/shadow failed "
                        "(need an admin session?)\n");
        goto out;
    }
    snprintf(line, sizeof(line), "%s:x:%d:\n", username, uid);
    if (append_line("/etc/group", line) < 0) {
        fprintf(stderr, "useradd: write /etc/group failed\n");
        goto out;
    }

    if (make_home) {
        char home[80];
        snprintf(home, sizeof(home), "/home/%s", username);
        if (install_make_home(home, uid, uid) < 0)
            fprintf(stderr, "useradd: warning: could not create %s\n", home);
    }

    printf("useradd: created '%s' (uid %d)%s\n",
           username, uid, make_home ? " with home" : "");
    rc = 0;
out:
    memset(pw, 0, sizeof(pw));
    memset(confirm, 0, sizeof(confirm));
    return rc;
}
