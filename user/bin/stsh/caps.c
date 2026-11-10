#include "stsh.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include "../../lib/libinstall/admin_targets.h"

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

/* Command-scoped authentication. The shell never claims an inherited admin
 * session: login authenticates and authorizes its exact allowlisted target.
 * Neither this UX nor the fresh-interpreter state protocol carries authority. */

int
admin_builtin(int argc, char **argv)
{
    if (argc<2 || argc>MAX_ARGV-2) {
        fprintf(stderr,"usage: admin command [args...]\n"
                "Authenticates one allowlisted administrative tool; shell sessions and\n"
                "commands do not inherit administrative authority.\n");
        return 1;
    }
    char path[256];
    if (strchr(argv[1],'/')) {
        if (snprintf(path,sizeof path,"%s",argv[1])>=(int)sizeof path) return 1;
    } else if (!strcmp(argv[1],"gui-installer")) {
        if (snprintf(path,sizeof path,"%s",ADMIN_GUI_INSTALLER_TARGET)>=(int)sizeof path) return 1;
    } else {
        if (snprintf(path,sizeof path,"/bin/%s",argv[1])>=(int)sizeof path) return 1;
    }
    if (!admin_target_caps(path)) {
        fprintf(stderr,"admin: command is not an allowlisted administrative tool\n");
        return 1;
    }
    /* Never PATH-search authentication targets or manufacture ready markers.
     * login is the authoritative fixed-path allowlist and credential boundary. */
    cmd_t cmd={0};
    cmd.argv[cmd.argc++]="/bin/login";
    cmd.argv[cmd.argc++]="--admin-exec";
    cmd.argv[cmd.argc++]=path;
    cmd.argv[cmd.argc++]="--";
    for (int i=2;i<argc;i++) cmd.argv[cmd.argc++]=argv[i];
    int status;
    run_pipeline(&cmd,1,env_as_array(),&status);
    return status;
}

int
deadmin_builtin(void)
{
    if (syscall(SYS_ADMIN_SESSION, 0L)<0) {
        perror("deadmin: drop shell session");
        return 1;
    }
    /* Drop the Lumen red-toolbar tint (mirror of admin_builtin). */
    fputs("\033]777;admin;0\007", stdout);
    fflush(stdout);
    puts("shell admin session dropped; independently authorized commands are unchanged.");
    return 0;
}
