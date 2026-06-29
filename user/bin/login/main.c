/* login — text-mode login binary for Aegis.
 *
 * Authenticates via libauth.a, then execve's the user's shell.
 * Capabilities: AUTH and SETUID from kernel policy table (service tier).
 * After successful auth, calls auth_elevate_session() so the spawned
 * shell gets admin-tier caps from the policy table.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <sys/types.h>
#include <sys/syscall.h>

#include "auth.h"

extern char **environ;

#define MAX_ATTEMPTS 3
#define FAIL_DELAY   3

#define SYS_ADMIN_SESSION 517
#define ADMIN_CRED_FILE   "/etc/aegis/admin"

/* Read a line from fd, stripping trailing newline. */
static int
readline(int fd, char *buf, int len)
{
    int i = 0;
    char c;
    while (i < len - 1) {
        int n = (int)read(fd, &c, 1);
        if (n <= 0) return (i > 0) ? i : -1;
        if (c == '\n' || c == '\r') break;
        buf[i++] = c;
    }
    buf[i] = '\0';
    return i;
}

/*
 * do_elevate — `login -elevate`: verify the SEPARATE admin credential and ask the
 * kernel to elevate the PARENT shell to an admin session.
 *
 * This is the single trusted authority for admin elevation (the shell holds no
 * such power and runs no credential code). login already owns CAP_KIND_AUTH and
 * the libauth credential machinery; it also holds CAP_KIND_ADMIN_AUTH (caps.d/
 * login) so the kernel accepts its sys_admin_session(1) request — which the kernel
 * applies to login's parent (the shell that fork/exec'd us). Returns 0 on success.
 */
static int
do_elevate(void)
{
    char stored[256];
    char password[128];
    int fd, n, ok;

    fd = open(ADMIN_CRED_FILE, O_RDONLY);
    if (fd < 0) {
        dprintf(2, "login: no admin credential configured (%s)\n", ADMIN_CRED_FILE);
        return 1;
    }
    n = (int)read(fd, stored, sizeof(stored) - 1);
    close(fd);
    if (n <= 0) {
        dprintf(2, "login: admin credential is empty\n");
        return 1;
    }
    stored[n] = '\0';
    while (n > 0 && (stored[n - 1] == '\n' || stored[n - 1] == '\r' ||
                     stored[n - 1] == ' '  || stored[n - 1] == '\t'))
        stored[--n] = '\0';

    /* Prompt for the admin password with echo off (reuse login's pattern). */
    {
        struct termios t;
        int have_tty = (tcgetattr(0, &t) == 0);
        struct termios t_raw = t;
        int pi = 0;
        char c;

        if (have_tty) {
            t_raw.c_lflag &= ~(unsigned)(ECHO | ICANON);
            tcsetattr(0, TCSANOW, &t_raw);
        }
        write(1, "admin password: ", 16);
        while (pi < (int)sizeof(password) - 1) {
            int r = (int)read(0, &c, 1);
            if (r <= 0) break;
            if (c == '\n' || c == '\r') break;
            password[pi++] = c;
        }
        password[pi] = '\0';
        write(1, "\n", 1);
        if (have_tty)
            tcsetattr(0, TCSANOW, &t);
    }

    ok = (auth_verify(password, stored) == 0);
    memset(password, 0, sizeof(password));
    if (!ok) {
        dprintf(2, "login: admin authentication failed\n");
        return 1;
    }

    if (syscall(SYS_ADMIN_SESSION, 1L) != 0) {
        dprintf(2, "login: kernel denied elevation (missing ADMIN_AUTH?)\n");
        return 1;
    }
    return 0;
}

int
main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "-elevate") == 0)
        return do_elevate();

    char username[64];
    char password[128];
    char home[256];
    char shell[256];
    int  uid = 0, gid = 0;

    /* AUTH + SETUID caps come from kernel policy table (service tier).
     * No runtime cap request needed — login is listed in caps.d/login. */

    /* Display pre-auth banner */
    {
        int bfd = open("/etc/banner", O_RDONLY);
        if (bfd >= 0) {
            char bbuf[512];
            int br;
            while ((br = (int)read(bfd, bbuf, sizeof(bbuf))) > 0)
                write(1, bbuf, (size_t)br);
            close(bfd);
        }
    }

    for (int attempt = 0; attempt < MAX_ATTEMPTS; attempt++) {
        write(1, "\nlogin: ", 8);
        if (readline(0, username, (int)sizeof(username)) < 0) continue;
        if (username[0] == '\0') { attempt--; continue; }

        /* Disable echo for password input */
        struct termios t;
        tcgetattr(0, &t);
        struct termios t_raw = t;
        t_raw.c_lflag &= ~(unsigned)(ECHO | ICANON);
        tcsetattr(0, TCSANOW, &t_raw);

        write(1, "password: ", 10);
        {
            int pi = 0;
            char c;
            while (pi < (int)sizeof(password) - 1) {
                int n = (int)read(0, &c, 1);
                if (n <= 0) break;
                if (c == '\n' || c == '\r') break;
                if (c == '\x7f' || c == '\b') {
                    if (pi > 0) { pi--; write(1, "\b \b", 3); }
                    continue;
                }
                password[pi++] = c;
                write(1, "*", 1);
            }
            password[pi] = '\0';
        }
        write(1, "\n", 1);
        tcsetattr(0, TCSANOW, &t);

        /* Authenticate */
        if (auth_check(username, password, &uid, &gid,
                       home, (int)sizeof(home), shell, (int)sizeof(shell)) != 0) {
            sleep(FAIL_DELAY);
            write(1, "Login incorrect\n", 16);
            continue;
        }

        /* Success — elevate session (binds uid/gid), set identity, launch shell */
        auth_elevate_session(uid, gid);
        write(1, "\033[2J\033[H", 7);
        auth_set_identity(uid, gid);
        chdir(home);

        setenv("HOME",    home,     1);
        setenv("USER",    username, 1);
        setenv("LOGNAME", username, 1);
        setenv("SHELL",   shell,    1);
        setenv("PATH",    "/bin",   1);
        /* Fontconfig: bundled apps (e.g. Ladybird) ship their config under
         * their tree; point fontconfig at it so monospace/sans/serif resolve. */
        setenv("FONTCONFIG_PATH", "/lib/ladybird/etc/fonts", 1);
        setenv("FONTCONFIG_FILE", "/lib/ladybird/etc/fonts/fonts.conf", 1);

        /* Build login shell name with leading '-' */
        char login_shell[64];
        const char *base = strrchr(shell, '/');
        const char *name = base ? base + 1 : shell;
        login_shell[0] = '-';
        int nlen = (int)strlen(name);
        if (nlen > 62) nlen = 62;
        memcpy(login_shell + 1, name, (size_t)nlen);
        login_shell[1 + nlen] = '\0';

        auth_grant_shell_caps();

        char *argv[] = { login_shell, NULL };
        execve(shell, argv, environ);
        /* Primary shell failed — log errno and try /bin/sh as last resort */
        dprintf(2, "[LOGIN] execve(%s) failed: errno=%d, falling back to /bin/sh\n",
                shell, errno);
        char *fb_argv[] = { "-sh", NULL };
        execve("/bin/sh", fb_argv, NULL);
        dprintf(2, "[LOGIN] execve(/bin/sh) failed: errno=%d\n", errno);
        return 1;
    }

    write(1, "Maximum login attempts exceeded.\n", 33);
    return 1;
}
