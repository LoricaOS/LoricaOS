/* user/bin/configure/main.c — "Configure LoricaOS" first-boot setup.
 *
 * The Raspberry Pi image is FLASHED to disk (no installer ISO / no install
 * step), so it ships with no user account. This runs once on first boot to let
 * the owner pick their username and password, then forces the greeter on every
 * subsequent boot (removes the live autologin). It is a thin UI over libinstall
 * — the same credential writer the x86 installer uses — minus every disk op.
 *
 * The primary account is uid 0 (LoricaOS has no "root": uid 0 is the first user
 * and carries no kernel power — authority is capabilities). This writes the
 * account files it can with CAP_KIND_AUTH alone — /etc/passwd, /etc/shadow,
 * /etc/group + the home dir — reusing libinstall's proven crypto/validation.
 *
 * Division of labour with vigil (PID 1): the configurator does the AUTH-gated
 * account write; vigil owns the /etc/aegis side (it already strips the live
 * autologin on first boot and holds the tree authority) — removing autologin to
 * force the greeter, writing the /etc/aegis/configured marker, and (open design
 * item) seeding the /etc/aegis/admin elevation credential, which needs
 * CAP_KIND_INSTALL+AUTH in an admin session that does not exist on first boot.
 * For *additional* accounts later the system ships useradd/passwd/etc.
 *
 * Runs with CAP_KIND_AUTH via caps.d (that is all the /etc/shadow write needs).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/stat.h>

#include "libinstall.h"

#define CONFIGURED_MARK "/etc/aegis/configured"

/* Write one whole file (create/truncate) then chmod (the VFS hardcodes 0644 on
 * O_CREAT). Returns 0 on success. */
static int write_file(const char *path, const char *data, int len, int mode)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    int off = 0;
    while (off < len) { int w = (int)write(fd, data + off, (size_t)(len - off));
                        if (w <= 0) { close(fd); return -1; } off += w; }
    close(fd);
    chmod(path, mode);
    return 0;
}

/* read a password with asterisk echo (termios raw), like the installer. */
static int read_password(const char *prompt, char *buf, int bufsize)
{
    struct termios orig, raw;
    int pi = 0; char c;
    int tty = (tcgetattr(0, &orig) == 0);
    if (tty) { raw = orig; raw.c_lflag &= ~(unsigned)(ECHO | ICANON); tcsetattr(0, TCSANOW, &raw); }
    printf("%s", prompt); fflush(stdout);
    while (pi < bufsize - 1) {
        int n = (int)read(0, &c, 1);
        if (n <= 0) break;
        if (c == '\n' || c == '\r') break;
        if (c == '\b' || c == 127) { if (pi > 0) { pi--; if (tty) write(1, "\b \b", 3); } continue; }
        buf[pi++] = c;
        if (tty) write(1, "*", 1);
    }
    buf[pi] = '\0';
    write(1, "\n", 1);
    if (tty) tcsetattr(0, TCSANOW, &orig);
    return pi;
}

static void read_line(const char *prompt, char *buf, int bufsize)
{
    printf("%s", prompt); fflush(stdout);
    if (!fgets(buf, bufsize, stdin)) { buf[0] = '\0'; return; }
    buf[strcspn(buf, "\n")] = '\0';
}

int main(void)
{
    /* Idempotent: if already configured, do nothing (vigil also guards this,
     * but never trust a single gate on the account-setup path). */
    if (access(CONFIGURED_MARK, F_OK) == 0) {
        printf("LoricaOS is already configured.\n");
        return 0;
    }

    printf("\n");
    printf("  ┌─────────────────────────────────────────┐\n");
    printf("  │           Configure LoricaOS            │\n");
    printf("  └─────────────────────────────────────────┘\n\n");
    printf("Welcome. Let's create your account.\n");
    printf("(You are the primary user — uid 0. LoricaOS has no separate root;\n");
    printf(" authority comes from capabilities, not from being uid 0.)\n\n");

    char user[64];
    for (;;) {
        read_line("Username: ", user, sizeof user);
        if (install_username_valid(user)) break;
        printf("  Invalid name. Use a-z, 0-9, '_' or '-', starting with a letter"
               " or '_', <=31 chars.\n");
    }

    char pw[128], confirm[128];
    for (;;) {
        if (read_password("Password: ", pw, sizeof pw) == 0) {
            printf("  Empty password rejected.\n"); continue;
        }
        read_password("Confirm password: ", confirm, sizeof confirm);
        if (strcmp(pw, confirm) == 0) break;
        printf("  Passwords do not match. Try again.\n");
    }

    char hash[256];
    if (install_hash_password(pw, hash, sizeof hash) != 0) {
        fprintf(stderr, "\nError: could not hash the password (no secure salt "
                        "from /dev/urandom). Aborting — nothing was written.\n");
        return 1;
    }

    /* Write the account files (uid 0 primary user). /etc/shadow is CAP_KIND_AUTH
     * gated; /etc/passwd and /etc/group are ordinary. The /etc/aegis tree is
     * left to vigil (this holds no INSTALL cap). */
    char line[512]; int n;
    n = snprintf(line, sizeof line, "%s:x:0:0:%s:/home/%s:/bin/stsh\n", user, user, user);
    if (write_file("/etc/passwd", line, n, 0644) != 0) {
        fprintf(stderr, "\nError: could not write /etc/passwd. Aborting.\n"); return 1; }
    n = snprintf(line, sizeof line, "%s:%s:19814:0:99999:7:::\n", user, hash);
    if (write_file("/etc/shadow", line, n, 0600) != 0) {
        fprintf(stderr, "\nError: could not write /etc/shadow (need CAP_KIND_AUTH). Aborting.\n");
        return 1; }
    n = snprintf(line, sizeof line, "%s:x:0:%s\nwheel:x:999:%s\n", user, user, user);
    if (write_file("/etc/group", line, n, 0644) != 0) {
        fprintf(stderr, "\nError: could not write /etc/group. Aborting.\n"); return 1; }

    /* Home directory for the new user (owned by uid 0). */
    {
        char home[80];
        snprintf(home, sizeof home, "/home/%s", user);
        install_make_home(home, 0, 0);
    }
    sync();

    printf("\n✓ Account '%s' created.\n", user);
    printf("  You'll be asked to log in with this password from now on.\n\n");
    return 0;
}
