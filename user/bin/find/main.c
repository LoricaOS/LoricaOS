/* find — recursively walk a path, printing each entry.
 * Usage: find [PATH] [-name PATTERN]   (PATTERN uses fnmatch globs) */
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fnmatch.h>
#include <stdio.h>
#include <string.h>

static const char *s_pattern = NULL;

static void
walk(const char *path)
{
    const char *base = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' && *(p + 1)) base = p + 1;

    if (!s_pattern || fnmatch(s_pattern, base, 0) == 0) {
        write(1, path, strlen(path));
        write(1, "\n", 1);
    }

    struct stat st;
    if (lstat(path, &st) != 0) return;
    if (!S_ISDIR(st.st_mode)) return;

    DIR *d = opendir(path);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".")  == 0) continue;
        if (strcmp(e->d_name, "..") == 0) continue;
        char child[1024];
        int n = snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
        if (n <= 0 || n >= (int)sizeof(child)) continue;
        walk(child);
    }
    closedir(d);
}

int
main(int argc, char **argv)
{
    const char *root = ".";
    int i = 1;
    if (i < argc && argv[i][0] != '-') {
        root = argv[i];
        i++;
    }
    for (; i < argc; i++) {
        if (strcmp(argv[i], "-name") == 0 && i + 1 < argc)
            s_pattern = argv[++i];
    }
    walk(root);
    return 0;
}
