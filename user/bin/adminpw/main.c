/* adminpw — change the admin-elevation credential (/etc/aegis/admin).
 *
 * LoricaOS has no root: admin actions are authorized by this separate
 * credential (verified by `login -elevate`), which the installer seeds
 * from the account password unless a separate one was chosen. This tool
 * is the post-install way to change it.
 *
 * Flow mirrors the CLI installer's elevation model:
 *   1. Prompt for the CURRENT admin password and hand it to
 *      /bin/login -elevate (install_elevate), which verifies it and asks
 *      the kernel to elevate this process to an admin session. That
 *      re-derives our cap table from caps.d/adminpw (admin tier):
 *      INSTALL because /etc/aegis/admin is an install-protected inode,
 *      AUTH because the kernel gates ANY open of the credential file on
 *      CAP_KIND_AUTH (it is shadow-grade sensitive).
 *   2. Prompt for the new password twice, hash (crypt SHA-512), rewrite
 *      the credential file, chmod 0600, sync.
 *
 * Running from an already-elevated [admin] shell still re-prompts for the
 * current password: changing the elevation credential always re-proves it.
 */
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include "libinstall.h"

#define ADMIN_CRED_FILE "/etc/aegis/admin"

/* Prompt with echo off; returns length. */
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

int main(void)
{
    /* Restore cooked mode — stsh leaves the terminal raw. */
    struct termios cooked;
    if (tcgetattr(0, &cooked) == 0) {
        cooked.c_lflag |= (unsigned)(ECHO | ICANON | ISIG);
        tcsetattr(0, TCSANOW, &cooked);
    }

    if (access(ADMIN_CRED_FILE, F_OK) != 0) {
        printf("adminpw: no admin credential exists (%s)\n", ADMIN_CRED_FILE);
        return 1;
    }

    printf("Change the admin password (authorizes admin actions).\n");

    /* Verify the current credential AND elevate in one step: login -elevate
     * reads the password from the pipe, checks it against the stored hash,
     * and elevates this process (re-deriving caps.d/adminpw's admin-tier
     * INSTALL cap, which the credential write below needs). */
    {
        char current[128];
        read_password("Current admin password: ", current, sizeof(current));
        int rc = install_elevate(current);
        memset(current, 0, sizeof(current));
        if (rc < 0) {
            printf("adminpw: admin authentication failed\n");
            return 1;
        }
    }

    char pw[128], confirm[128];
    if (read_password("New admin password: ", pw, sizeof(pw)) == 0) {
        printf("adminpw: password cannot be empty\n");
        return 1;
    }
    read_password("Confirm new admin password: ", confirm, sizeof(confirm));
    if (strcmp(pw, confirm) != 0) {
        memset(pw, 0, sizeof(pw));
        memset(confirm, 0, sizeof(confirm));
        printf("adminpw: passwords do not match\n");
        return 1;
    }

    char hash[256];
    int hrc = install_hash_password(pw, hash, sizeof(hash));
    memset(pw, 0, sizeof(pw));
    memset(confirm, 0, sizeof(confirm));
    if (hrc < 0) {
        printf("adminpw: crypt() failed\n");
        return 1;
    }

    int fd = open(ADMIN_CRED_FILE, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        printf("adminpw: cannot write %s (admin capability missing?)\n",
               ADMIN_CRED_FILE);
        return 1;
    }
    char line[300];
    int n = snprintf(line, sizeof(line), "%s\n", hash);
    if (write(fd, line, (size_t)n) != n) {
        close(fd);
        printf("adminpw: write failed\n");
        return 1;
    }
    close(fd);
    chmod(ADMIN_CRED_FILE, 0600);
    sync();

    printf("Admin password changed.\n");
    return 0;
}
