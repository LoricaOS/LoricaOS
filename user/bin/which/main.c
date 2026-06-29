/* which — locate a command in PATH (or /bin if PATH is unset). */
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static int
search_one(const char *path, const char *name)
{
    const char *p = path;
    char buf[1024];
    while (*p) {
        const char *end = p;
        while (*end && *end != ':') end++;
        int dlen = (int)(end - p);
        if (dlen > 0 && dlen + 1 + (int)strlen(name) + 1 < (int)sizeof(buf)) {
            int n = snprintf(buf, sizeof(buf), "%.*s/%s", dlen, p, name);
            if (n > 0 && access(buf, X_OK) == 0) {
                write(1, buf, (size_t)n);
                write(1, "\n", 1);
                return 0;
            }
        }
        p = (*end == ':') ? end + 1 : end;
    }
    return 1;
}

int
main(int argc, char **argv)
{
    if (argc < 2) {
        dprintf(2, "usage: which <command>...\n");
        return 1;
    }
    const char *path = getenv("PATH");
    if (!path || !*path) path = "/bin";
    int rc = 0;
    for (int i = 1; i < argc; i++)
        if (search_one(path, argv[i]) != 0) rc = 1;
    return rc;
}
