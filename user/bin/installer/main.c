/* user/bin/installer/main.c — LoricaOS text-mode installer
 *
 * Thin UI shell over libinstall.a.  Collects disk choice and
 * credentials from the user via stdin prompts, then hands off to
 * install_run_all() which does the actual work.
 *
 * The UI layer (this file) owns:
 *   - Welcome banner
 *   - Disk enumeration + confirmation
 *   - Interactive password entry (read_password with TTY raw mode)
 *   - Printf-based progress callbacks
 *
 * libinstall (../../lib/libinstall) owns:
 *   - GPT writing, rootfs copy, ESP install
 *   - grub.cfg, test binary strip, /etc/passwd writer
 *   - Password hashing
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>

#include "libinstall.h"

/* ── Progress callbacks ─────────────────────────────────────────────── */

static void tui_on_step(const char *label, void *ctx)
{
    (void)ctx;
    printf("\n%s... ", label);
    fflush(stdout);
}

static void tui_on_progress(int pct, void *ctx)
{
    (void)ctx;
    if (pct == 100)
        printf("done (100%%)\n");
    else if (pct % 10 == 0)
        printf("%d%% ", pct);
    fflush(stdout);
}

static void tui_on_error(const char *msg, void *ctx)
{
    (void)ctx;
    printf("\nERROR: %s\n", msg);
}

/* ── Password entry (TUI-only) ──────────────────────────────────────── */

/* read_password — read password with asterisk echo using termios raw
 * mode. Handles backspace. Returns length of password. */
static int read_password(const char *prompt, char *buf, int bufsize)
{
    struct termios orig, raw;
    int pi = 0;
    char c;

    tcgetattr(0, &orig);
    raw = orig;
    raw.c_lflag &= ~(unsigned)(ECHO | ICANON);
    tcsetattr(0, TCSANOW, &raw);

    printf("%s", prompt);
    fflush(stdout);

    while (pi < bufsize - 1) {
        int n = (int)read(0, &c, 1);
        if (n <= 0) break;
        if (c == '\n' || c == '\r') break;
        if (c == '\b' || c == 127) {
            if (pi > 0) {
                pi--;
                write(1, "\b \b", 3);
            }
            continue;
        }
        buf[pi++] = c;
        write(1, "*", 1);
    }
    buf[pi] = '\0';
    write(1, "\n", 1);

    tcsetattr(0, TCSANOW, &orig);
    return pi;
}

/* read_line — read a line with echo. Returns length. */
static int read_line(const char *prompt, char *buf, int bufsize)
{
    int i = 0;
    char c;
    printf("%s", prompt);
    fflush(stdout);
    while (i < bufsize - 1 && read(0, &c, 1) == 1) {
        if (c == '\n' || c == '\r') break;
        if (c == '\b' || c == 127) {
            if (i > 0) { i--; write(1, "\b \b", 3); }
            continue;
        }
        buf[i++] = c;
        write(1, &c, 1);
    }
    buf[i] = '\0';
    printf("\n");
    return i;
}

/* collect_credentials — prompt for the primary user account and hash the
 * password. LoricaOS has no "root": this user IS uid 0 (the first uid), and the
 * same password becomes the sudo-style admin-elevation credential. Fills
 * *username and *user_hash. Returns 0 on success, -1 on cancel or mismatch. */
static int collect_credentials(char *username, int username_sz,
                               char *user_hash, int user_hash_sz)
{
    char pw[64], confirm[64];

    printf("\n--- Create your account ---\n");
    printf("This user is uid 0 — the first user. LoricaOS has no separate root;\n");
    printf("you elevate to admin actions by re-entering this same password.\n");
    if (read_line("Username: ", username, username_sz) == 0) {
        printf("ERROR: username cannot be empty\n");
        return -1;
    }
    if (!install_username_valid(username)) {
        printf("ERROR: username must be a-z/0-9/_/- (max 31, start a-z or _)\n");
        return -1;
    }
    if (read_password("Password: ", pw, sizeof(pw)) == 0) {
        printf("ERROR: password cannot be empty\n");
        return -1;
    }
    if (read_password("Confirm password: ", confirm, sizeof(confirm)) == 0) {
        printf("ERROR: confirmation failed\n");
        return -1;
    }
    if (strcmp(pw, confirm) != 0) {
        printf("ERROR: passwords do not match\n");
        return -1;
    }
    if (install_hash_password(pw, user_hash, user_hash_sz) < 0) {
        printf("ERROR: crypt() failed\n");
        return -1;
    }
    printf("User '%s' configured (uid 0).\n", username);
    return 0;
}

/* ── Main ───────────────────────────────────────────────────────────── */

int main(void)
{
    /* Restore cooked mode — stsh leaves the terminal in raw mode */
    struct termios cooked;
    tcgetattr(0, &cooked);
    cooked.c_lflag |= (unsigned)(ECHO | ICANON | ISIG);
    tcsetattr(0, TCSANOW, &cooked);

    printf("\n=== LoricaOS Installer ===\n\n");
    printf("This will install LoricaOS to your NVMe disk.\n");
    printf("WARNING: All data on the disk will be destroyed!\n\n");

    /* Raw whole-disk access (listing AND writing disks via sys_blkdev_*)
     * requires an admin session (CAP_KIND_DISK_ADMIN). Authenticate via
     * /bin/login -elevate on the shared tty up front — before enumerating
     * disks — or every subsequent block syscall returns ENOCAP. */
    printf("--- Administrator authorization ---\n");
    printf("Installing requires admin authorization.\n");
    if (install_elevate(NULL) < 0) {
        printf("\nERROR: admin authentication required to install.\n");
        printf("Aborting — no changes were made.\n");
        return 1;
    }
    printf("\n");

    install_blkdev_t devs[8];
    int ndevs = install_list_blkdevs(devs, 8);
    if (ndevs < 0) {
        /* The disk-list syscall refused us — almost always -ENOCAP because this
         * process lacks CAP_KIND_DISK_ADMIN (admin elevation didn't take). Say so
         * plainly instead of the ambiguous "cannot enumerate" below. */
        printf("ERROR: admin capability (DISK_ADMIN) required to install.\n");
        printf("Run the installer from an admin-elevated session.\n");
        printf("Aborting — no changes were made.\n");
        return 1;
    }
    if (ndevs == 0) {
        printf("ERROR: cannot enumerate block devices\n");
        return 1;
    }

    /* Build the list of eligible target disks (raw disks — skip ramdisks
     * and partition devices). Present them numbered so multi-disk machines
     * can choose, instead of silently targeting the last-enumerated one. */
    int eligible[8];
    int neligible = 0;
    printf("Available disks:\n");
    int i;
    for (i = 0; i < ndevs && neligible < 8; i++) {
        if (strncmp(devs[i].name, "ramdisk", 7) == 0) continue;
        if (strchr(devs[i].name, 'p') != NULL) continue;
        printf("  [%d] %s: %llu sectors (%llu MB)%s\n",
               neligible,
               devs[i].name,
               (unsigned long long)devs[i].block_count,
               (unsigned long long)devs[i].block_count *
                   devs[i].block_size / (1024 * 1024),
               install_disk_has_aegis(devs[i].name)
                   ? "  [existing LoricaOS install — will be erased]" : "");
        eligible[neligible++] = i;
    }
    if (neligible == 0) {
        printf("\nNo suitable disk found.\n");
        return 1;
    }

    int target;
    char respbuf[16] = {0};
    int ri = 0; char rc;
    if (neligible == 1) {
        target = eligible[0];
        printf("\nInstall to %s? [y/N] ", devs[target].name);
        fflush(stdout);
        while (ri < (int)sizeof(respbuf) - 1 && read(0, &rc, 1) == 1) {
            if (rc == '\n' || rc == '\r') break;
            respbuf[ri++] = rc;
        }
        printf("\n");
        if (respbuf[0] != 'y' && respbuf[0] != 'Y') {
            printf("Aborted.\n");
            return 0;
        }
    } else {
        printf("\nSelect disk to install to [0-%d] (Enter to abort): ",
               neligible - 1);
        fflush(stdout);
        while (ri < (int)sizeof(respbuf) - 1 && read(0, &rc, 1) == 1) {
            if (rc == '\n' || rc == '\r') break;
            respbuf[ri++] = rc;
        }
        printf("\n");
        int sel = respbuf[0] - '0';   /* single digit: at most 8 disks */
        if (respbuf[0] < '0' || respbuf[0] > '9' || sel >= neligible) {
            printf("Aborted.\n");
            return 0;
        }
        target = eligible[sel];
        printf("Installing to %s — ALL DATA WILL BE DESTROYED.\n",
               devs[target].name);
    }

    /* Collect credentials BEFORE destructive disk ops so a cancel is
     * still safe. */
    char username[64]   = "";
    char user_hash[256] = "";
    if (collect_credentials(username, sizeof(username),
                            user_hash, sizeof(user_hash)) < 0) {
        printf("Credential collection failed. Aborting.\n");
        return 1;
    }

    install_progress_t prog = {
        .on_step     = tui_on_step,
        .on_progress = tui_on_progress,
        .on_error    = tui_on_error,
        .ctx         = NULL,
    };

    if (install_run_all(devs[target].name,
                        devs[target].block_count,
                        devs[target].block_size,
                        username,
                        user_hash,
                        &prog) < 0) {
        printf("\n=== Installation FAILED ===\n");
        return 1;
    }

    printf("\n=== Installation complete! ===\n");
    printf("Remove the ISO and reboot to start LoricaOS from disk.\n\n");
    return 0;
}
