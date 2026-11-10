/* login — text-mode login binary for LoricaOS.
 *
 * Authenticates via libauth.a, then execve's the user's shell.
 * Capabilities: AUTH and SETUID from kernel policy table (service tier).
 * Authentication, binding, identity switch and shell exec stay in this process.
 * This does not authorize independent admin executables.
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
#include <sys/stat.h>

#include "auth.h"
#include "../../lib/libinstall/admin_targets.h"

extern char **environ;

#define MAX_ATTEMPTS 3
#define FAIL_DELAY   3

#define SYS_ADMIN_SESSION 517
#define ADMIN_CRED_FILE   "/etc/aegis/admin"

static void wipe_secret(char *buf, size_t len)
{
    volatile char *p = buf;
    while (len--) *p++ = 0;
}

/* Read a line from fd, stripping trailing newline. */
static int
readline(int fd, char *buf, int len)
{
    int i = 0;
    char c;
    while (i < len - 1) {
        int n = (int)read(fd, &c, 1);
        if (n <= 0) { buf[i] = '\0'; return -1; }
        if (c == '\n' || c == '\r') { buf[i] = '\0'; return i; }
        buf[i++] = c;
    }
    buf[i] = '\0';
    return -1; /* truncated line */
}

/*
 * do_elevate — `login -elevate`: verify the SEPARATE admin credential and ask the
 * kernel to elevate the direct PARENT executable to an admin session.
 *
 * This is the single trusted authority for admin elevation (the shell holds no
 * such power and runs no credential code). login already owns CAP_KIND_AUTH and
 * the libauth credential machinery; it also holds CAP_KIND_ADMIN_AUTH (caps.d/
 * login) so the kernel accepts its sys_admin_session(1) request — which the kernel
 * applies to login's parent (the shell that fork/exec'd us). Returns 0 on success.
 */
static int
verify_admin(int password_fd)
{
    char stored[256];
    char password[128];
    int fd, n, ok;

    /* Throttle brute force. do_elevate is the SOLE gate for admin authority, and
     * any unprivileged process can fork/exec `login -elevate` in a loop feeding
     * guesses over a pipe. This once tried to serialize with flock(LOCK_EX) on a
     * /run lock plus a per-failure sleep — but that was INERT: flock() and
     * fcntl() record locks are unimplemented in the kernel (ENOSYS / fake
     * success), and /run is world-writable so the lock file is attacker-
     * manipulable regardless. With uid cosmetically 0, no userland lock can fix
     * this, so the throttle now lives where an attacker can't reach it: the
     * KERNEL paces reads of /etc/aegis/admin by non-admin processes to one per
     * window, globally (kernel/fs/vfs.c, vfs_open_ex). A throttled attempt gets
     * -EAGAIN on the open below and fails closed. The per-failure sleep here
     * stays as a small extra cost on the interactive path. */
    fd = open(ADMIN_CRED_FILE, O_RDONLY);
    if (fd < 0) {
        dprintf(2, "login: no admin credential configured (%s)\n", ADMIN_CRED_FILE);
        return 1;
    }
    n = (int)read(fd, stored, sizeof(stored) - 1);
    int closed = close(fd);
    if (n <= 0 || n == (int)sizeof(stored) - 1 || closed != 0) {
        dprintf(2, "login: cannot read complete admin credential\n");
        return 1;
    }
    stored[n] = '\0';
    while (n > 0 && (stored[n - 1] == '\n' || stored[n - 1] == '\r' ||
                     stored[n - 1] == ' '  || stored[n - 1] == '\t'))
        stored[--n] = '\0';

    /* Prompt for the admin password with echo off (reuse login's pattern). */
    {
        struct termios t = {0};
        int have_tty = password_fd == 0 && (tcgetattr(0, &t) == 0);
        struct termios t_raw = t;
        int pi = 0, terminated = 0;
        char c;

        if (have_tty) {
            t_raw.c_lflag &= ~(unsigned)(ECHO | ICANON);
            tcsetattr(0, TCSANOW, &t_raw);
        }
        write(1, "admin password: ", 16);
        while (pi < (int)sizeof(password) - 1) {
            int r = (int)read(password_fd, &c, 1);
            if (r <= 0) break;
            if (c == '\n' || c == '\r') { terminated = 1; break; }
            password[pi++] = c;
        }
        password[pi] = '\0';
        write(1, "\n", 1);
        if (have_tty)
            tcsetattr(0, TCSANOW, &t);
        /* Refuse a truncated candidate instead of authenticating its prefix. */
        if (!terminated) {
            wipe_secret(password, sizeof(password));
            wipe_secret(stored, sizeof(stored));
            return 1;
        }
    }

    ok = (auth_verify(password, stored) == 0);
    wipe_secret(password, sizeof(password));
    wipe_secret(stored, sizeof(stored));
    if (!ok) {
        dprintf(2, "login: admin authentication failed\n");
        sleep(FAIL_DELAY);   /* mandatory throttle, held under the elevate lock */
        return 1;
    }

    return 0;
}

static int
do_elevate(void)
{
    if (verify_admin(0) != 0) return 1;
    if (syscall(SYS_ADMIN_SESSION, 1L) != 0) {
        dprintf(2, "login: kernel denied elevation (missing ADMIN_AUTH?)\n");
        return 1;
    }
    return 0;
}

/* The separate admin credential authorizes principal 0, not the password of
 * account uid 0. Never accept a shell, PATH search, arbitrary path or argv0. */
static int
do_admin_exec(int argc, char **argv)
{
    if (argc < 4 || !admin_target_caps(argv[2])) return 1;
    const char *target = argv[2];
    int arg = 3, password_fd = 0;
    if (!strcmp(argv[arg], "--password-fd")) {
        if (argc < 6) return 1;
        char *end;
        errno = 0;
        long fd = strtol(argv[arg + 1], &end, 10);
        struct stat st;
        if (errno || !argv[arg + 1][0] || *end || fd < 3 || fd > 255 ||
            fstat((int)fd, &st) != 0 || !S_ISFIFO(st.st_mode))
            return 1;
        password_fd = (int)fd;
        arg += 2;
    }
    if (strcmp(argv[arg++], "--")) return 1;
    if (verify_admin(password_fd) != 0) {
        if (password_fd) close(password_fd);
        return 1;
    }
    if (password_fd && close(password_fd) != 0) return 1;
    /* No fork between verification and exec. Every step is mandatory. */
    if (auth_elevate_session(0, 0) != 0 ||
        auth_set_identity(0, 0) != 0 ||
        syscall(521) != 0 || syscall(519) != 1) {
        dprintf(2, "login: kernel denied administrative principal\n");
        return 1;
    }
    /* Drop inherited GUI/IPC/disk descriptors. Aegis's FD table has 256 slots;
     * do not carry the caller's loader/configuration environment into admin. */
    for (int fd = 3; fd < 256; fd++) close(fd);
    char *av[132];
    int n = 0;
    av[n++] = (char *)target;
    av[n++] = ADMIN_READY_MARKER;
    for (; arg < argc && n < 131; arg++) av[n++] = argv[arg];
    if (arg != argc) return 1;
    av[n] = NULL;
    char *safe_env[] = { "PATH=/bin", "HOME=/", "USER=admin", "LOGNAME=admin",
                         "TERM=xterm", NULL };
    execve(target, av, safe_env);
    dprintf(2, "login: administrative exec failed: %s (errno=%d)\n", target, errno);
    return 1; /* Never fall back to a shell or another executable. */
}

int
main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--admin-exec") == 0)
        return do_admin_exec(argc, argv);
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
        struct termios t = {0};
        tcgetattr(0, &t);
        struct termios t_raw = t;
        t_raw.c_lflag &= ~(unsigned)(ECHO | ICANON);
        tcsetattr(0, TCSANOW, &t_raw);

        write(1, "password: ", 10);
        int password_complete = 0;
        {
            int pi = 0;
            char c;
            while (pi < (int)sizeof(password) - 1) {
                int n = (int)read(0, &c, 1);
                if (n <= 0) break;
                if (c == '\n' || c == '\r') { password_complete = 1; break; }
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
        if (!password_complete || auth_check(username, password, &uid, &gid,
                       home, (int)sizeof(home), shell, (int)sizeof(shell)) != 0) {
            wipe_secret(password, sizeof(password));
            sleep(FAIL_DELAY);
            write(1, "Login incorrect\n", 16);
            continue;
        }

        wipe_secret(password, sizeof(password));
        /* No fork: an independent child would lose the authenticated binding. */
        if (auth_elevate_session(uid, gid) != 0 ||
            auth_set_identity(uid, gid) != 0) {
            dprintf(2, "login: kernel denied authenticated identity\n");
            return 1;
        }
        write(1, "\033[2J\033[H", 7);
        if (chdir(home) != 0) {
            dprintf(2, "login: cannot enter home directory: %s\n", home);
            return 1;
        }

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
