/* ps — process status from /proc.
 *
 * Walks /proc/[pid]/status (Name/State/Pid/PPid/Uid/VmSize) and
 * /proc/[pid]/exe. Aegis is a single-seat system, so the default shows
 * every process (BSD "ps aux" muscle memory is tolerated and ignored).
 *
 *   ps              all processes
 *   ps -p PID       only PID (may repeat: -p 1 -p 2)
 *   ps -u UID       only processes owned by UID
 *   ps -f           full: adds PPID/UID columns and the exe path as CMD
 *
 * Columns: PID [PPID UID] STAT VSZ CMD
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <unistd.h>

#define MAX_PROCS 128

typedef struct {
    int  pid;
    int  ppid;
    int  uid;
    char state;
    long vsz_kb;
    char name[64];
    char exe[256];
} pinfo_t;

static pinfo_t s_procs[MAX_PROCS];
static int     s_nprocs;

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
    out->state = '?';

    const char *v;
    if ((v = field(buf, "Name"))) {
        size_t i = 0;
        while (v[i] && v[i] != '\n' && i < sizeof(out->name) - 1) {
            out->name[i] = v[i];
            i++;
        }
        out->name[i] = '\0';
    }
    if ((v = field(buf, "State")))  out->state  = *v;
    if ((v = field(buf, "PPid")))   out->ppid   = atoi(v);
    if ((v = field(buf, "Uid")))    out->uid    = atoi(v);
    if ((v = field(buf, "VmSize"))) out->vsz_kb = atol(v);

    snprintf(path, sizeof(path), "/proc/%d/exe", pid);
    f = fopen(path, "r");
    if (f) {
        if (fgets(out->exe, sizeof(out->exe), f)) {
            char *nl = strchr(out->exe, '\n');
            if (nl) *nl = '\0';
        }
        fclose(f);
    }
    if (!out->exe[0])
        snprintf(out->exe, sizeof(out->exe), "[%s]", out->name);
    return 0;
}

static int cmp_pid(const void *a, const void *b)
{
    return ((const pinfo_t *)a)->pid - ((const pinfo_t *)b)->pid;
}

static void fmt_kb(char *out, size_t cap, long kb)
{
    if (kb < 10240)
        snprintf(out, cap, "%ldK", kb);
    else
        snprintf(out, cap, "%ld.%ldM", kb / 1024, (kb % 1024) * 10 / 1024);
}

static void usage(void)
{
    fprintf(stderr, "usage: ps [-f] [-p pid]... [-u uid]\n");
    exit(1);
}

int main(int argc, char **argv)
{
    int full = 0;
    int want_uid = -1;
    int want_pids[32];
    int nwant = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-f") == 0 || strcmp(a, "-l") == 0) {
            full = 1;
        } else if (strcmp(a, "-p") == 0) {
            if (++i >= argc) usage();
            if (nwant < 32) want_pids[nwant++] = atoi(argv[i]);
        } else if (strcmp(a, "-u") == 0) {
            if (++i >= argc) usage();
            want_uid = atoi(argv[i]);
        } else if (strcmp(a, "-e") == 0 || strcmp(a, "ax") == 0 ||
                   strcmp(a, "aux") == 0 || strcmp(a, "-ef") == 0) {
            /* habit compat: these all mean "everything" — the default */
            if (a[0] != '-' || a[1] == 'e') full = (strcmp(a, "-ef") == 0)
                                                   ? 1 : full;
        } else {
            usage();
        }
    }

    DIR *d = opendir("/proc");
    if (!d) { perror("/proc"); return 1; }
    struct dirent *de;
    while ((de = readdir(d)) != NULL && s_nprocs < MAX_PROCS) {
        if (!isdigit((unsigned char)de->d_name[0])) continue;
        int pid = atoi(de->d_name);
        if (pid <= 0) continue;
        if (read_proc(pid, &s_procs[s_nprocs]) == 0)
            s_nprocs++;
    }
    closedir(d);
    qsort(s_procs, (size_t)s_nprocs, sizeof(pinfo_t), cmp_pid);

    if (full)
        printf("%5s %5s %4s %-4s %7s CMD\n",
               "PID", "PPID", "UID", "STAT", "VSZ");
    else
        printf("%5s %-4s %7s CMD\n", "PID", "STAT", "VSZ");

    for (int i = 0; i < s_nprocs; i++) {
        pinfo_t *p = &s_procs[i];
        if (want_uid >= 0 && p->uid != want_uid) continue;
        if (nwant > 0) {
            int hit = 0;
            for (int j = 0; j < nwant; j++)
                if (want_pids[j] == p->pid) hit = 1;
            if (!hit) continue;
        }
        char vsz[16];
        fmt_kb(vsz, sizeof(vsz), p->vsz_kb);
        if (full)
            printf("%5d %5d %4d %-4c %7s %s\n",
                   p->pid, p->ppid, p->uid, p->state, vsz, p->exe);
        else
            printf("%5d %-4c %7s %s\n", p->pid, p->state, vsz, p->name);
    }
    return 0;
}
