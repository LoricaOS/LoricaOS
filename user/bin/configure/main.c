/* user/bin/configure/main.c — "Configure LoricaOS" first-boot setup.
 *
 * The Raspberry Pi image is FLASHED to disk (no installer ISO / no install
 * step), so it ships with no user account. This runs once on first boot to let
 * the owner pick their username and password, then forces the greeter on every
 * subsequent boot (removes the live autologin). It is a thin UI over libinstall
 * — the same credential writer the x86 installer uses — minus every disk op.
 *
 * The primary account is uid 0 (LoricaOS has no "root": uid 0 is the first user
 * and carries no kernel power — authority is capabilities). install_write_
 * credentials writes /etc/passwd + /etc/shadow + /etc/group + the /etc/aegis/
 * admin elevation credential + the home dir in one call; the admin credential
 * defaults to the user's own password (sudo-style: re-type your own password to
 * elevate), changeable later with adminpw. For *additional* accounts the system
 * ships useradd/passwd/etc.
 *
 * Authority: runs with CAP_KIND_AUTH + CAP_KIND_INSTALL via the FIRST-BOOT
 * EXCEPTION — caps.d/configure declares the `firstboot` tier, which the kernel
 * grants only while /etc/aegis/configured is absent (g_first_boot). That is what
 * lets it write the /etc/aegis tree on a machine with no account and thus no
 * admin session to elevate. Writing /etc/aegis/configured at the end flips the
 * exception off on every later boot — a one-shot grant.
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
#define AUTOLOGIN_FILE  "/etc/aegis/autologin"

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

    /* Write the account + the /etc/aegis/admin elevation credential (defaults to
     * the user's own hash — pass NULL) + the home dir, in one call. Needs
     * CAP_KIND_AUTH + CAP_KIND_INSTALL, held here via the first-boot exception. */
    if (install_write_credentials(user, hash, NULL) != 0) {
        fprintf(stderr, "\nError: could not write the account files. If the "
                        "system is already configured the first-boot exception "
                        "is gone — nothing was written.\n");
        return 1;
    }

    /* Force the greeter from now on: drop any live passwordless autologin. */
    unlink(AUTOLOGIN_FILE);

    /* Mark configured — this flips the kernel's first-boot exception off on every
     * subsequent boot, so this program can never write /etc/aegis again. Write it
     * before the self-cleanup so idempotency holds even if a delete below fails. */
    {
        int fd = open(CONFIGURED_MARK, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) { write(fd, "1\n", 2); close(fd); }
    }

    /* Self-cleanup (mirrors vigil's installer removal): delete our own caps grant
     * and our binary so nothing carrying the first-boot exception's AUTH+INSTALL
     * lingers on a configured system. We still hold INSTALL this boot (caps were
     * applied at exec), so the /etc/aegis/caps.d write is permitted; a running
     * program may unlink its own executable (the inode survives until exit).
     * Best-effort — the marker above already closed the exception and blocks a
     * re-run, so a failure here is harmless. */
    unlink("/etc/aegis/caps.d/configure");   /* our capability grant */
    unlink("/bin/configure");                /* our binary */
    sync();

    printf("\n✓ Account '%s' created and LoricaOS configured.\n", user);
    printf("  You'll log in with this password from now on"
           " (re-type it to elevate to admin).\n\n");
    return 0;
}
