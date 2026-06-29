/* pkill — signal processes by name from /proc.
 *
 * Walks /proc/[pid]/status, matches PATTERN as a fixed substring against
 * the Name field (Aegis has no regex), and sends a signal to every match
 * via kill(2). Prints nothing on success.
 *
 *   pkill [-SIG | -s SIG] [-x] [-f] [-n] PATTERN
 *
 *   -SIG    signal to send, numeric or by name ("SIG" prefix optional),
 *           e.g. -9, -KILL, -SIGKILL; default TERM
 *   -s SIG  same, as a separate argument
 *   -x      exact whole-string match
 *   -f      match against the full /proc/[pid]/exe path instead of Name
 *   -n      newest match only (highest pid)
 *
 * pkill's own pid AND its parent pid are always excluded — a loose
 * pattern must not kill the invoking shell. Exit 0 if at least one
 * process was signalled, 1 if none, 2 on usage error.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <dirent.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>

#define MAX_PROCS 128

typedef struct {
    int  pid;
    char name[64];
    char exe[256];
} pinfo_t;

static pinfo_t s_match[MAX_PROCS];
static int     s_nmatch;

/* Linux x86_64 signal names, indexed 1-31. */
static const char *const s_signames[32] = {
    NULL,     "HUP",  "INT",  "QUIT", "ILL",    "TRAP", "ABRT", "BUS",
    "FPE",    "KILL", "USR1", "SEGV", "USR2",   "PIPE", "ALRM", "TERM",
    "STKFLT", "CHLD", "CONT", "STOP", "TSTP",   "TTIN", "TTOU", "URG",
    "XCPU",   "XFSZ", "VTALRM", "PROF", "WINCH", "IO",  "PWR",  "SYS"
};

/* "9", "KILL", "SIGKILL" (case-insensitive) -> 9; -1 if unknown. */
static int parse_signal(const char *s)
{
    if (isdigit((unsigned char)s[0])) {
        char *end;
        long v = strtol(s, &end, 10);
        if (*end || v < 1 || v > 31) return -1;
        return (int)v;
    }
    if (strncasecmp(s, "SIG", 3) == 0 && s[3])
        s += 3;
    for (int i = 1; i <= 31; i++)
        if (strcasecmp(s, s_signames[i]) == 0)
            return i;
    return -1;
}

/* Read one line value from a "Key:\tvalue" status file already in buf. */
static const char *field(const char *buf, const char *key)
{
    const char *p = buf;
    size_t klen = strlen(key);
    while (p && *p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == ':') {
            p += klen + 1;
            while (*p == ' ' || *p == '\t') p++;
            return p;
        }
        p = strchr(p, '\n');
        if (p) p++;
    }
    return NULL;
}

static int read_proc(int pid, pinfo_t *out)
{
    char path[64], buf[1024];
    snprintf(path, sizeof(path), "/proc/%d/status", pid);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    memset(out, 0, sizeof(*out));
    out->pid = pid;

    const char *v = field(buf, "Name");
    if (v) {
        size_t i = 0;
        while (v[i] && v[i] != '\n' && i < sizeof(out->name) - 1) {
            out->name[i] = v[i];
            i++;
        }
        out->name[i] = '\0';
    }

    snprintf(path, sizeof(path), "/proc/%d/exe", pid);
    f = fopen(path, "r");
    if (f) {
        if (fgets(out->exe, sizeof(out->exe), f)) {
            char *nl = strchr(out->exe, '\n');
            if (nl) *nl = '\0';
        }
        fclose(f);
    }
    return 0;
}

static int cmp_pid(const void *a, const void *b)
{
    return ((const pinfo_t *)a)->pid - ((const pinfo_t *)b)->pid;
}

static void usage(void)
{
    fprintf(stderr, "usage: pkill [-SIG | -s SIG] [-x] [-f] [-n] PATTERN\n");
    exit(2);
}

int main(int argc, char **argv)
{
    int opt_x = 0, opt_f = 0, opt_n = 0;
    int sig = SIGTERM;
    const char *pat = NULL;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '-' && a[1]) {
            if (strcmp(a, "-x") == 0) {
                opt_x = 1;
            } else if (strcmp(a, "-f") == 0) {
                opt_f = 1;
            } else if (strcmp(a, "-n") == 0) {
                opt_n = 1;
            } else if (strcmp(a, "-s") == 0) {
                if (++i >= argc) usage();
                sig = parse_signal(argv[i]);
                if (sig < 0) {
                    fprintf(stderr, "pkill: unknown signal: %s\n", argv[i]);
                    exit(2);
                }
            } else {
                sig = parse_signal(a + 1);
                if (sig < 0) usage();
            }
        } else if (!pat) {
            pat = a;
        } else {
            usage();
        }
    }
    if (!pat) usage();

    int self   = (int)getpid();
    int parent = (int)getppid();

    DIR *d = opendir("/proc");
    if (!d) { perror("pkill: /proc"); return 1; }
    struct dirent *de;
    pinfo_t p;
    while ((de = readdir(d)) != NULL && s_nmatch < MAX_PROCS) {
        if (!isdigit((unsigned char)de->d_name[0])) continue;
        int pid = atoi(de->d_name);
        if (pid <= 0 || pid == self || pid == parent) continue;
        if (read_proc(pid, &p) != 0) continue;
        const char *target = opt_f ? p.exe : p.name;
        int hit = opt_x ? (strcmp(target, pat) == 0)
                        : (strstr(target, pat) != NULL);
        if (hit)
            s_match[s_nmatch++] = p;
    }
    closedir(d);

    if (s_nmatch == 0) return 1;
    qsort(s_match, (size_t)s_nmatch, sizeof(pinfo_t), cmp_pid);

    int signalled = 0;
    int first = opt_n ? s_nmatch - 1 : 0;
    for (int i = first; i < s_nmatch; i++) {
        if (kill(s_match[i].pid, sig) == 0) {
            signalled++;
        } else {
            fprintf(stderr, "pkill: kill pid %d: %s\n",
                    s_match[i].pid, strerror(errno));
        }
    }
    return signalled > 0 ? 0 : 1;
}
