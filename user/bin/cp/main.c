/* cp — copy files and directories.
 *
 *   cp [-r] SRC DST          copy SRC to DST (file, or into DST if it's a dir)
 *   cp [-r] SRC... DESTDIR   copy each SRC into the existing directory DESTDIR
 *   cp -r DIR DST            recursively copy a directory tree
 *
 * Supports the common coreutils subset: multiple sources into a directory,
 * recursive directory copy (-r/-R), copy-into-directory semantics, and source
 * permission preservation. No -p/-a/-u/-i, no symlink-following options.
 */
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>

static int g_recursive = 0;

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

/* Join a + "/" + b into out (avoiding a doubled slash when a ends in '/'). */
static void join_path(char *out, size_t cap, const char *a, const char *b)
{
    size_t la = strlen(a);
    if (la > 0 && a[la - 1] == '/')
        snprintf(out, cap, "%s%s", a, b);
    else
        snprintf(out, cap, "%s/%s", a, b);
}

static int is_dir(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static int copy_file(const char *src, const char *dst)
{
    int s = open(src, O_RDONLY);
    if (s < 0) { fprintf(stderr, "cp: %s: %s\n", src, strerror(errno)); return 1; }

    mode_t mode = 0644;
    struct stat st;
    if (fstat(s, &st) == 0)
        mode = st.st_mode & 0777;

    int d = open(dst, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (d < 0) {
        fprintf(stderr, "cp: %s: %s\n", dst, strerror(errno));
        close(s);
        return 1;
    }

    char buf[4096];
    int n, rc = 0;
    while ((n = read(s, buf, sizeof buf)) > 0) {
        char *p = buf;
        int left = n;
        while (left > 0) {
            int w = write(d, p, left);
            if (w <= 0) {
                fprintf(stderr, "cp: write %s: %s\n", dst, strerror(errno));
                rc = 1;
                left = 0;
                n = 0;  /* suppress the read-error branch below */
                break;
            }
            p += w;
            left -= w;
        }
        if (rc) break;
    }
    if (n < 0) {
        fprintf(stderr, "cp: read %s: %s\n", src, strerror(errno));
        rc = 1;
    }
    close(s);
    close(d);
    return rc;
}

static int copy_any(const char *src, const char *dst);

static int copy_dir(const char *src, const char *dst)
{
    if (mkdir(dst, 0755) < 0 && errno != EEXIST) {
        fprintf(stderr, "cp: %s: %s\n", dst, strerror(errno));
        return 1;
    }
    DIR *dir = opendir(src);
    if (!dir) { fprintf(stderr, "cp: %s: %s\n", src, strerror(errno)); return 1; }

    struct dirent *de;
    int rc = 0;
    while ((de = readdir(dir)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        char sp[1024], dp[1024];
        join_path(sp, sizeof sp, src, de->d_name);
        join_path(dp, sizeof dp, dst, de->d_name);
        rc |= copy_any(sp, dp);
    }
    closedir(dir);
    return rc;
}

static int copy_any(const char *src, const char *dst)
{
    if (is_dir(src)) {
        if (!g_recursive) {
            fprintf(stderr, "cp: -r not specified; omitting directory %s\n", src);
            return 1;
        }
        return copy_dir(src, dst);
    }
    return copy_file(src, dst);
}

int main(int argc, char **argv)
{
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        for (const char *f = argv[i] + 1; *f; f++) {
            if (*f == 'r' || *f == 'R')
                g_recursive = 1;
            else {
                fprintf(stderr, "cp: unknown option -%c\n", *f);
                return 1;
            }
        }
    }

    if (argc - i < 2) {
        fprintf(stderr, "usage: cp [-r] src... dest\n");
        return 1;
    }

    const char *dst = argv[argc - 1];
    int dst_is_dir = is_dir(dst);

    if ((argc - i) > 2 && !dst_is_dir) {
        fprintf(stderr, "cp: target '%s' is not a directory\n", dst);
        return 1;
    }

    int rc = 0;
    for (; i < argc - 1; i++) {
        const char *src = argv[i];
        const char *target = dst;
        char path[1024];
        if (dst_is_dir) {
            join_path(path, sizeof path, dst, base_name(src));
            target = path;
        }
        rc |= copy_any(src, target);
    }
    return rc;
}
