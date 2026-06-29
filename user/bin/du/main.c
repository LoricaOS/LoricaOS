/* du — disk usage.
 *
 *   du [PATH...]      per-directory usage (1K units), "." by default
 *   du -a             include files, not just directories
 *   du -s             one summary line per argument
 *   du -c             grand total line
 *   du -h             human-readable sizes
 *   du -b             raw bytes instead of 1K blocks
 *   du -d N           limit printing to N directory levels (still sums all)
 *
 * Sizes come from st_size (Aegis stat does not fill st_blocks). Symlinks
 * are lstat'ed and counted as themselves, never followed — the rootfs has
 * loop-free dirs but link-following du would double-count /lib aliases.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_DEPTH 64

static int s_all, s_summary, s_total, s_human, s_bytes;
static int s_maxdepth = MAX_DEPTH;
static long long s_grand;
static int s_rc;

static void print_size(long long bytes, const char *path)
{
    if (s_human) {
        const char *u = "B";
        long long v10 = bytes * 10;
        if (bytes >= 1024LL * 1024 * 1024) {
            v10 = bytes * 10 / (1024LL * 1024 * 1024); u = "G";
        } else if (bytes >= 1024 * 1024) {
            v10 = bytes * 10 / (1024 * 1024); u = "M";
        } else if (bytes >= 1024) {
            v10 = bytes * 10 / 1024; u = "K";
        }
        if (strcmp(u, "B") == 0)
            printf("%lld\t%s\n", bytes, path);
        else
            printf("%lld.%lld%s\t%s\n", v10 / 10, v10 % 10, u, path);
    } else if (s_bytes) {
        printf("%lld\t%s\n", bytes, path);
    } else {
        printf("%lld\t%s\n", (bytes + 1023) / 1024, path);
    }
}

/* Recursively sum path; prints per-dir (and per-file with -a) lines
 * unless suppressed by -s or depth. Returns total bytes under path. */
static long long walk(const char *path, int depth)
{
    struct stat st;
    if (lstat(path, &st) != 0) {
        fprintf(stderr, "du: %s: cannot stat\n", path);
        s_rc = 1;
        return 0;
    }

    if (!S_ISDIR(st.st_mode)) {
        if (s_all && !s_summary && depth <= s_maxdepth)
            print_size(st.st_size, path);
        return st.st_size;
    }

    long long total = st.st_size;
    if (depth < MAX_DEPTH) {
        DIR *d = opendir(path);
        if (!d) {
            fprintf(stderr, "du: %s: cannot open\n", path);
            s_rc = 1;
        } else {
            struct dirent *de;
            while ((de = readdir(d)) != NULL) {
                if (strcmp(de->d_name, ".") == 0 ||
                    strcmp(de->d_name, "..") == 0)
                    continue;
                char sub[1024];
                if (strcmp(path, "/") == 0)
                    snprintf(sub, sizeof(sub), "/%s", de->d_name);
                else
                    snprintf(sub, sizeof(sub), "%s/%s", path, de->d_name);
                total += walk(sub, depth + 1);
            }
            closedir(d);
        }
    }

    if (!s_summary && depth <= s_maxdepth)
        print_size(total, path);
    return total;
}

static void usage(void)
{
    fprintf(stderr, "usage: du [-a] [-s] [-c] [-h] [-b] [-d N] [path...]\n");
    exit(1);
}

int main(int argc, char **argv)
{
    const char *paths[64];
    int npaths = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-a") == 0)      s_all = 1;
        else if (strcmp(a, "-s") == 0) s_summary = 1;
        else if (strcmp(a, "-c") == 0) s_total = 1;
        else if (strcmp(a, "-h") == 0) s_human = 1;
        else if (strcmp(a, "-b") == 0) s_bytes = 1;
        else if (strcmp(a, "-d") == 0) {
            if (++i >= argc) usage();
            s_maxdepth = atoi(argv[i]);
        } else if (a[0] == '-' && a[1] != '\0') {
            usage();
        } else if (npaths < 64) {
            paths[npaths++] = a;
        }
    }
    if (s_all && s_summary) {
        fprintf(stderr, "du: -a and -s are mutually exclusive\n");
        return 1;
    }
    if (npaths == 0) paths[npaths++] = ".";

    for (int i = 0; i < npaths; i++) {
        long long t = walk(paths[i], 0);
        s_grand += t;
        if (s_summary)
            print_size(t, paths[i]);
    }
    if (s_total)
        print_size(s_grand, "total");
    return s_rc;
}
