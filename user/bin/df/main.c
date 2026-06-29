/* df — report filesystem disk space usage from /proc/mounts.
 *
 * /proc/mounts lines: "<device> <mountpoint> <fstype> <total_kb> <free_kb>".
 * The ramfs mounts (/tmp, /run) report "0 0" — the kernel has no cheap
 * size accessor for them — so their size columns show 0 and Use% is "-".
 *
 *   df              all mounts, sizes in 1K blocks
 *   df -k           same (explicit 1K blocks)
 *   df -h           human-readable (plain K; M/G with one decimal)
 *   df PATH...      only the mount whose mountpoint is the longest
 *                   prefix of each PATH
 *
 * Columns: Filesystem Type Size Used Avail Use% Mounted on
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_MOUNTS 16

typedef struct {
    char dev[64];
    char mnt[128];
    char type[32];
    unsigned long long total_kb;
    unsigned long long free_kb;
} mount_t;

static mount_t s_mounts[MAX_MOUNTS];
static int     s_nmounts;
static int     s_human;

static int read_mounts(void)
{
    FILE *f = fopen("/proc/mounts", "r");
    if (!f) {
        perror("/proc/mounts");
        return -1;
    }
    char line[256];
    while (fgets(line, sizeof(line), f) && s_nmounts < MAX_MOUNTS) {
        mount_t *m = &s_mounts[s_nmounts];
        if (sscanf(line, "%63s %127s %31s %llu %llu",
                   m->dev, m->mnt, m->type,
                   &m->total_kb, &m->free_kb) == 5)
            s_nmounts++;
    }
    fclose(f);
    return 0;
}

/* Format kb: raw 1K blocks by default; with -h, plain K below 1M and
 * M/G with one decimal above (kb is integral, so K has no fraction). */
static void fmt_size(char *out, size_t cap, unsigned long long kb)
{
    if (!s_human) {
        snprintf(out, cap, "%llu", kb);
        return;
    }
    if (kb < 1024ULL) {
        snprintf(out, cap, "%lluK", kb);
    } else if (kb < 1024ULL * 1024ULL) {
        snprintf(out, cap, "%llu.%lluM",
                 kb / 1024, (kb % 1024) * 10 / 1024);
    } else {
        unsigned long long mb = kb / 1024;
        snprintf(out, cap, "%llu.%lluG",
                 mb / 1024, (mb % 1024) * 10 / 1024);
    }
}

static void print_mount(const mount_t *m)
{
    unsigned long long used = (m->total_kb > m->free_kb)
                              ? m->total_kb - m->free_kb : 0;
    char size_s[24], used_s[24], avail_s[24], pct_s[8];

    fmt_size(size_s,  sizeof(size_s),  m->total_kb);
    fmt_size(used_s,  sizeof(used_s),  used);
    fmt_size(avail_s, sizeof(avail_s), m->free_kb);
    if (m->total_kb == 0)
        snprintf(pct_s, sizeof(pct_s), "-");          /* ramfs placeholder */
    else
        snprintf(pct_s, sizeof(pct_s), "%llu%%",      /* rounded up */
                 (used * 100 + m->total_kb - 1) / m->total_kb);

    printf("%-15s %-9s %4s %5s %5s %4s %s\n",
           m->dev, m->type, size_s, used_s, avail_s, pct_s, m->mnt);
}

/* Longest-prefix mountpoint match on a component boundary:
 * "/" matches everything; "/tmp" matches "/tmp" and "/tmp/x",
 * not "/tmpfile". */
static const mount_t *find_mount(const char *path)
{
    const mount_t *best = NULL;
    size_t best_len = 0;
    for (int i = 0; i < s_nmounts; i++) {
        const mount_t *m = &s_mounts[i];
        size_t len = strlen(m->mnt);
        if (strncmp(path, m->mnt, len) != 0)
            continue;
        if (len > 1 && path[len] != '\0' && path[len] != '/')
            continue;
        if (!best || len > best_len) {
            best = m;
            best_len = len;
        }
    }
    return best;
}

static void usage(void)
{
    fprintf(stderr, "usage: df [-h | -k] [path...]\n");
    exit(1);
}

int main(int argc, char **argv)
{
    int argi = 1;

    while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
        if (strcmp(argv[argi], "-h") == 0)
            s_human = 1;
        else if (strcmp(argv[argi], "-k") == 0)
            s_human = 0;
        else
            usage();
        argi++;
    }

    if (read_mounts() < 0)
        return 1;

    printf("%-15s %-9s %4s %5s %5s %4s %s\n",
           "Filesystem", "Type", "Size", "Used", "Avail", "Use%",
           "Mounted on");

    if (argi >= argc) {
        for (int i = 0; i < s_nmounts; i++)
            print_mount(&s_mounts[i]);
        return 0;
    }

    int rc = 0;
    for (; argi < argc; argi++) {
        char abs[256];
        const char *path = argv[argi];
        if (path[0] != '/') {
            /* Make relative paths absolute against cwd (no symlink
             * resolution — Aegis has no /proc/self/cwd to chase). */
            char cwd[128];
            if (getcwd(cwd, sizeof(cwd))) {
                snprintf(abs, sizeof(abs), "%s/%s", cwd, path);
                path = abs;
            }
        }
        const mount_t *m = find_mount(path);
        if (!m) {
            fprintf(stderr, "df: %s: no matching mount\n", argv[argi]);
            rc = 1;
            continue;
        }
        print_mount(m);
    }
    return rc;
}
