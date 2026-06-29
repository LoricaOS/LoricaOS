/* dirname — strip the last path component. */
#include <unistd.h>
#include <stdio.h>
#include <string.h>

int
main(int argc, char **argv)
{
    if (argc != 2) {
        dprintf(2, "usage: dirname <path>\n");
        return 1;
    }
    const char *p = argv[1];
    int len = (int)strlen(p);
    while (len > 1 && p[len - 1] == '/') len--;
    int last = -1;
    for (int i = 0; i < len; i++)
        if (p[i] == '/') last = i;
    if (last < 0)  { write(1, ".\n", 2); return 0; }
    if (last == 0) { write(1, "/\n", 2); return 0; }
    while (last > 0 && p[last - 1] == '/') last--;
    write(1, p, (size_t)last);
    write(1, "\n", 1);
    return 0;
}
