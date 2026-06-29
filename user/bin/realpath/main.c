/* realpath — resolve a path to its absolute, symlink-free form. */
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <limits.h>

int
main(int argc, char **argv)
{
    if (argc < 2) {
        dprintf(2, "usage: realpath <path>...\n");
        return 1;
    }
    int  rc = 0;
    char buf[PATH_MAX];
    for (int i = 1; i < argc; i++) {
        if (realpath(argv[i], buf) == NULL) {
            perror(argv[i]);
            rc = 1;
            continue;
        }
        int n = 0;
        while (buf[n]) n++;
        write(1, buf, (size_t)n);
        write(1, "\n", 1);
    }
    return rc;
}
