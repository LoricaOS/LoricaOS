/* basename — strip directory and (optional) suffix from a path. */
#include <unistd.h>
#include <stdio.h>
#include <string.h>

int
main(int argc, char **argv)
{
    if (argc < 2) {
        dprintf(2, "usage: basename <path> [suffix]\n");
        return 1;
    }
    const char *p = argv[1];
    const char *base = p;
    const char *end = p + strlen(p);
    while (end > p && *(end - 1) == '/') end--;
    for (const char *q = p; q < end; q++)
        if (*q == '/') base = q + 1;
    int len = (int)(end - base);
    if (len <= 0) {
        write(1, "/\n", 2);
        return 0;
    }
    if (argc >= 3) {
        int slen = (int)strlen(argv[2]);
        if (slen > 0 && slen <= len &&
            strncmp(base + len - slen, argv[2], (size_t)slen) == 0)
            len -= slen;
    }
    write(1, base, (size_t)len);
    write(1, "\n", 1);
    return 0;
}
