/* credentials.c — password hash + /etc/passwd writer (libinstall) */
#include "libinstall.h"
#include <crypt.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef O_TRUNC
#define O_TRUNC 0x200
#endif

/* A username is interpolated raw into colon-delimited /etc/passwd, /etc/shadow
 * and /etc/group lines, so it must not contain ':' or a newline (which would
 * inject extra fields or whole account lines), and must be a sane account
 * name. Mirror the check both installers should enforce up front; this is the
 * last line of defence before the files are written. */
int install_username_valid(const char *username)
{
    if (!username || !username[0])
        return 0;
    int len = 0;
    for (const char *p = username; *p; p++, len++) {
        char c = *p;
        int ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                 c == '_' || c == '-';
        /* First char must be a letter or underscore (POSIX-ish). */
        if (p == username && !((c >= 'a' && c <= 'z') || c == '_'))
            return 0;
        if (!ok)
            return 0;
    }
    return len <= 31;
}

/* ── Salt generation (file-local) ───────────────────────────────────── */

static void generate_salt(char *buf, int bufsize)
{
    static const char b64[] =
        "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    uint8_t rand_bytes[12];
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        read(fd, rand_bytes, sizeof(rand_bytes));
        close(fd);
    } else {
        memset(rand_bytes, 0, sizeof(rand_bytes));
    }

    int pos = 0;
    int i;
    buf[pos++] = '$';
    buf[pos++] = '6';
    buf[pos++] = '$';
    for (i = 0; i < 12 && pos < bufsize - 2; i++)
        buf[pos++] = b64[rand_bytes[i] % 64];
    buf[pos++] = '$';
    buf[pos] = '\0';
}

/* ── Public: install_hash_password ──────────────────────────────────── */

int install_hash_password(const char *password, char *out, int outsz)
{
    if (!password || !out || outsz < 128)
        return -1;
    char salt[32];
    generate_salt(salt, sizeof(salt));
    char *hashed = crypt(password, salt);
    if (!hashed)
        return -1;
    snprintf(out, (size_t)outsz, "%s", hashed);
    return 0;
}

/* ── Public: install_write_credentials ──────────────────────────────── */

/* Write the account files for the installed system's single primary user.
 *
 * LoricaOS has no "root". uid 0 is simply the FIRST assigned uid and carries no
 * kernel power — authority comes only from capabilities, never from being uid 0
 * (the kernel even makes an authenticated admin session re-prove itself). So the
 * person installing the system IS uid 0; there is no separate superuser account
 * and no 1000+ convention. Additional users a distro adds can start at uid 1.
 *
 * The chosen password is also written as the admin-elevation credential
 * (/etc/aegis/admin), so the user gains admin capabilities by re-typing their
 * own password (sudo-style) rather than knowing a second secret. */
int install_write_credentials(const char *username,
                              const char *user_hash)
{
    if (!username || !username[0] || !user_hash || !user_hash[0])
        return -1;

    /* Refuse a malformed username rather than corrupt the account files. */
    if (!install_username_valid(username))
        return -1;

    /* /etc/passwd — primary user, uid 0, home /home/<user>. */
    {
        int fd = open("/etc/passwd", O_WRONLY | O_CREAT | O_TRUNC);
        if (fd < 0)
            return -1;
        char line[256];
        int n = snprintf(line, sizeof(line),
                         "%s:x:0:0:%s:/home/%s:/bin/stsh\n",
                         username, username, username);
        if (write(fd, line, (size_t)n) != n) { close(fd); return -1; }
        close(fd);
    }

    /* /etc/shadow */
    {
        int fd = open("/etc/shadow", O_WRONLY | O_CREAT | O_TRUNC);
        if (fd < 0)
            return -1;
        char line[512];
        int n = snprintf(line, sizeof(line),
                         "%s:%s:19814:0:99999:7:::\n", username, user_hash);
        if (write(fd, line, (size_t)n) != n) { close(fd); return -1; }
        close(fd);
    }

    /* /etc/group — primary group gid 0; the user is also in wheel. */
    {
        int fd = open("/etc/group", O_WRONLY | O_CREAT | O_TRUNC);
        if (fd < 0)
            return -1;
        char line[256];
        int n = snprintf(line, sizeof(line),
                         "%s:x:0:%s\nwheel:x:999:%s\n",
                         username, username, username);
        if (write(fd, line, (size_t)n) != n) { close(fd); return -1; }
        close(fd);
    }

    /* /etc/aegis/admin — admin-elevation credential = the user's own hash, so
     * `login -elevate` re-authenticates with the password they just chose. */
    {
        int fd = open("/etc/aegis/admin", O_WRONLY | O_CREAT | O_TRUNC);
        if (fd < 0)
            return -1;
        char line[256];
        int n = snprintf(line, sizeof(line), "%s\n", user_hash);
        if (write(fd, line, (size_t)n) != n) { close(fd); return -1; }
        close(fd);
    }

    /* Lock down /etc/shadow + the admin credential (0600); passwd/group stay
     * world-readable. The kernel ignores open()'s mode arg (the VFS hardcodes
     * 0644 on O_CREAT), so set the modes explicitly. Runs before the pre-copy
     * sync() so the new inode modes reach the installed disk. */
    chmod("/etc/shadow", 0600);
    chmod("/etc/passwd", 0644);
    chmod("/etc/group", 0644);
    chmod("/etc/aegis/admin", 0600);

    /* Home directory for the primary user (owned by uid 0). The passwd line
     * points at /home/<username>; without this the user logs into a missing
     * directory and anything writing to $HOME fails with ENOENT. */
    {
        char home[80];
        mkdir("/home", 0755);                       /* may already exist */
        snprintf(home, sizeof(home), "/home/%s", username);
        mkdir(home, 0755);
        chown(home, 0, 0);
    }

    return 0;
}
