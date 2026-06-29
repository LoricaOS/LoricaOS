#include "stsh.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/syscall.h>

#define SYS_CAP_QUERY      362
#define SYS_SPAWN          514
#define SYS_ADMIN_SESSION  517   /* drop (arg 0) is self/ungated; elevate is login's job */

static int s_has_delegate = 0;
static int s_has_query    = 0;

/* Human-readable cap names — index = kind value from kernel/cap/cap.h */
static const char *cap_names[] = {
    "NULL",            /*  0 */
    "VFS_OPEN",        /*  1 */
    "VFS_WRITE",       /*  2 */
    "VFS_READ",        /*  3 */
    "AUTH",            /*  4 */
    "CAP_GRANT",       /*  5 */
    "SETUID",          /*  6 */
    "NET_SOCKET",      /*  7 */
    "NET_ADMIN",       /*  8 */
    "THREAD_CREATE",   /*  9 */
    "PROC_READ",       /* 10 */
    "DISK_ADMIN",      /* 11 */
    "FB",              /* 12 */
    "CAP_DELEGATE",    /* 13 */
    "CAP_QUERY",       /* 14 */
    "IPC",             /* 15 */
    "POWER",           /* 16 */
};
#define NUM_CAP_NAMES 17

static const char *
rights_str(unsigned int rights)
{
    static char buf[8];
    int i = 0;
    if (rights & 1) buf[i++] = 'r';
    if (rights & 2) buf[i++] = 'w';
    if (rights & 4) buf[i++] = 'x';
    if (i == 0) buf[i++] = '-';
    buf[i] = '\0';
    return buf;
}

static int
cap_name_to_kind(const char *name)
{
    for (int i = 1; i < NUM_CAP_NAMES; i++) {
        if (strcmp(cap_names[i], name) == 0)
            return i;
    }
    return -1;
}

/*
 * caps_init — cache whether we hold CAP_DELEGATE and CAP_QUERY.
 * Uses sys_cap_query(0, buf, sizeof(buf)) which always succeeds for pid=0.
 */
void
caps_init(void)
{
    cap_slot_t slots[CAP_TABLE_SIZE];
    memset(slots, 0, sizeof(slots));

    long ret = syscall(SYS_CAP_QUERY, 0L, (long)slots, (long)sizeof(slots));
    if (ret <= 0)
        return;

    for (int i = 0; i < (int)ret; i++) {
        if (slots[i].kind == 13) s_has_delegate = 1;
        if (slots[i].kind == 14) s_has_query    = 1;
    }
}

int
has_cap_delegate(void)
{
    return s_has_delegate;
}

/*
 * caps_builtin — display capabilities.
 * No args = own caps (pid=0, always allowed).
 * With PID arg = target's caps (requires CAP_QUERY).
 */
int
caps_builtin(int argc, char **argv)
{
    long pid = 0;
    if (argc >= 2) {
        pid = atol(argv[1]);
        if (pid != 0 && !s_has_query) {
            fprintf(stderr, "caps: permission denied (CAP_QUERY not held)\n");
            return 1;
        }
    }

    cap_slot_t slots[CAP_TABLE_SIZE];
    memset(slots, 0, sizeof(slots));

    long ret = syscall(SYS_CAP_QUERY, pid, (long)slots, (long)sizeof(slots));
    if (ret < 0) {
        if (ret == -130)
            fprintf(stderr, "caps: permission denied (ENOCAP)\n");
        else if (ret == -3)
            fprintf(stderr, "caps: no such process\n");
        else
            fprintf(stderr, "caps: error %ld\n", ret);
        return 1;
    }

    int first = 1;
    for (int i = 0; i < (int)ret; i++) {
        if (slots[i].kind == 0) continue;
        if (!first) printf(" ");
        first = 0;

        const char *name = (slots[i].kind < NUM_CAP_NAMES)
                           ? cap_names[slots[i].kind]
                           : "UNKNOWN";
        printf("%s(%s)", name, rights_str(slots[i].rights));
    }
    if (!first) printf("\n");
    return 0;
}

/*
 * sandbox_builtin — run a command with restricted capabilities.
 * Usage: sandbox -allow CAP1,CAP2 -- command args...
 * Requires CAP_DELEGATE.
 */
int
sandbox_builtin(int argc, char **argv, char **envp)
{
    if (!s_has_delegate) {
        fprintf(stderr, "sandbox: permission denied (CAP_DELEGATE not held)\n");
        return 1;
    }

    if (argc < 4 || strcmp(argv[1], "-allow") != 0) {
        fprintf(stderr, "usage: sandbox -allow CAP[,CAP,...] -- command [args...]\n");
        return 1;
    }

    /* Find -- separator */
    int cmd_start = -1;
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0) {
            cmd_start = i + 1;
            break;
        }
    }
    if (cmd_start < 0 || cmd_start >= argc) {
        fprintf(stderr, "sandbox: missing -- before command\n");
        return 1;
    }

    /* Parse allowlist into cap_mask */
    cap_slot_t mask[CAP_TABLE_SIZE];
    memset(mask, 0, sizeof(mask));
    int nmask = 0;

    char allowlist[256];
    strncpy(allowlist, argv[2], 255);
    allowlist[255] = '\0';

    char *tok = allowlist;
    while (*tok && nmask < (int)CAP_TABLE_SIZE) {
        char *comma = strchr(tok, ',');
        if (comma) *comma = '\0';

        int kind = cap_name_to_kind(tok);
        if (kind < 0) {
            fprintf(stderr, "sandbox: unknown capability '%s'\n", tok);
            return 1;
        }
        mask[nmask].kind   = (unsigned int)kind;
        mask[nmask].rights = 1 | 2 | 4;  /* all rights */
        nmask++;

        if (comma) tok = comma + 1;
        else break;
    }

    /* Build path */
    char path[256];
    if (argv[cmd_start][0] != '/')
        snprintf(path, sizeof(path), "/bin/%s", argv[cmd_start]);
    else
        snprintf(path, sizeof(path), "%s", argv[cmd_start]);

    /* Build child argv */
    char *child_argv[MAX_ARGV + 1];
    int ci = 0;
    for (int i = cmd_start; i < argc && ci < MAX_ARGV; i++)
        child_argv[ci++] = argv[i];
    child_argv[ci] = NULL;

    /* sys_spawn with cap_mask (5th arg).
     * Use syscall() from libc which handles 6-arg calls correctly. */
    long pid = syscall(SYS_SPAWN, (long)path, (long)child_argv,
                       (long)envp, (long)-1, (long)mask);
    if (pid < 0) {
        fprintf(stderr, "sandbox: spawn failed (%ld)\n", pid);
        return 1;
    }

    int status;
    waitpid((int)pid, &status, 0);

    if ((status & 0x7f) == 0)
        return (status >> 8) & 0xff;
    return 128 + (status & 0x7f);
}

/*
 * grant_builtin — formerly granted a capability to a running process.
 * Runtime cap granting (sys_cap_grant, syscall 363) has been removed.
 * Capabilities are now granted by kernel policy at exec time.
 */
int
grant_builtin(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    dprintf(2, "grant: removed — caps are now granted by kernel policy at exec time\n");
    dprintf(2, "see /etc/aegis/caps.d/ for per-binary cap policies\n");
    return 1;
}

/* ── admin session (sudo-style elevation) ──────────────────────────────────
 *
 * `admin` elevates THIS shell session to an administrative session after
 * verifying a SEPARATE admin credential (/etc/aegis/admin) — even root must
 * authenticate. Anything the shell then spawns (herald) inherits the admin
 * session and so may hold CAP_KIND_INSTALL. `deadmin` drops it. The kernel
 * gates the elevation syscall on CAP_KIND_ADMIN_AUTH (held by stsh); the
 * credential check happens here in userspace, mirroring login + sys_auth_session. */

static int s_admin_session = 0;

int
stsh_admin_active(void)
{
    return s_admin_session;
}

int
admin_builtin(int argc, char **argv)
{
    extern char **environ;
    pid_t pid;
    int status = 0;
    (void)argc; (void)argv;

    if (s_admin_session) {
        puts("admin: session already elevated");
        return 0;
    }

    /* Delegate to the trusted authenticator. /bin/login -elevate verifies the
     * SEPARATE admin credential and asks the kernel to elevate THIS shell — login
     * runs as our child, so the kernel applies the grant to us, its parent. The
     * shell holds NO elevation authority (no ADMIN_AUTH cap) and runs no
     * credential code; concentrating both in login keeps the surface minimal. */
    pid = fork();
    if (pid < 0) {
        perror("admin: fork");
        return 1;
    }
    if (pid == 0) {
        char *av[] = { "/bin/login", "-elevate", NULL };
        execve("/bin/login", av, environ);
        _exit(127);
    }
    if (waitpid(pid, &status, 0) < 0) {
        perror("admin: waitpid");
        return 1;
    }
    /* login exits 0 only on a verified elevation (which set our admin_session). */
    if (!((status & 0x7f) == 0 && ((status >> 8) & 0xff) == 0))
        return 1;   /* login already reported the reason */

    s_admin_session = 1;
    /* Emit a private OSC so a Lumen terminal tints this window's titlebar red.
     * Unknown OSC is ignored by other terminals → safe everywhere. BEL (\007)
     * terminated; glyph_term's OSC parser matches this. */
    fputs("\033]777;admin;1\007", stdout);
    fflush(stdout);
    puts("admin session active — package management unlocked. Run 'deadmin' to drop.");
    return 0;
}

int
deadmin_builtin(void)
{
    syscall(SYS_ADMIN_SESSION, 0L);
    s_admin_session = 0;
    /* Drop the Lumen red-toolbar tint (mirror of admin_builtin). */
    fputs("\033]777;admin;0\007", stdout);
    fflush(stdout);
    puts("admin session dropped.");
    return 0;
}
