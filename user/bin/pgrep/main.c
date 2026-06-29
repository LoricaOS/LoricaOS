/* pgrep — find processes by name from /proc.
 *
 * Walks /proc/[pid]/status and matches PATTERN as a fixed substring
 * against the Name field (Aegis has no regex). Prints one matching PID
 * per line, ascending.
 *
 *   pgrep [-l] [-x] [-f] [-n] [-v] PATTERN
 *
 *   -l   print "PID NAME" instead of just PID
 *   -x   exact whole-string match
 *   -f   match against the full /proc/[pid]/exe path instead of Name
 *   -n   newest match only (highest pid)
 *   -v   invert the match
 *
 * pgrep's own pid is always excluded. Exit 0 if at least one process
 * matched, 1 if none, 2 on usage error.
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
    char name[64];
    char exe[256];
} pinfo_t;

static pinfo_t s_match[MAX_PROCS];
static int     s_nmatch;

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
    fprintf(stderr, "usage: pgrep [-l] [-x] [-f] [-n] [-v] PATTERN\n");
    exit(2);
}

int main(int argc, char **argv)
{
    int opt_l = 0, opt_x = 0, opt_f = 0, opt_n = 0, opt_v = 0;
    const char *pat = NULL;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '-' && a[1]) {
            for (int j = 1; a[j]; j++) {
                switch (a[j]) {
                case 'l': opt_l = 1; break;
                case 'x': opt_x = 1; break;
                case 'f': opt_f = 1; break;
                case 'n': opt_n = 1; break;
                case 'v': opt_v = 1; break;
                default:  usage();
                }
            }
        } else if (!pat) {
            pat = a;
        } else {
            usage();
        }
    }
    if (!pat) usage();

    int self = (int)getpid();

    DIR *d = opendir("/proc");
    if (!d) { perror("pgrep: /proc"); return 1; }
    struct dirent *de;
    pinfo_t p;
    while ((de = readdir(d)) != NULL && s_nmatch < MAX_PROCS) {
        if (!isdigit((unsigned char)de->d_name[0])) continue;
        int pid = atoi(de->d_name);
        if (pid <= 0 || pid == self) continue;
        if (read_proc(pid, &p) != 0) continue;
        const char *target = opt_f ? p.exe : p.name;
        int hit = opt_x ? (strcmp(target, pat) == 0)
                        : (strstr(target, pat) != NULL);
        if (opt_v) hit = !hit;
        if (hit)
            s_match[s_nmatch++] = p;
    }
    closedir(d);

    if (s_nmatch == 0) return 1;
    qsort(s_match, (size_t)s_nmatch, sizeof(pinfo_t), cmp_pid);

    int first = opt_n ? s_nmatch - 1 : 0;
    for (int i = first; i < s_nmatch; i++) {
        if (opt_l)
            printf("%d %s\n", s_match[i].pid, s_match[i].name);
        else
            printf("%d\n", s_match[i].pid);
    }
    return 0;
}
