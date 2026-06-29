/* tr — translate or delete characters. Literal sets only (no [a-z]).
 * Usage: tr SET1 SET2  |  tr -d SET */
#include <unistd.h>
#include <stdio.h>
#include <string.h>

int
main(int argc, char **argv)
{
    int delete_mode = 0, i = 1;
    if (i < argc && strcmp(argv[i], "-d") == 0) { delete_mode = 1; i++; }

    if (delete_mode) {
        if (i + 1 != argc) {
            dprintf(2, "usage: tr -d <chars>\n");
            return 1;
        }
        const char *del = argv[i];
        char buf[1];
        while (read(0, buf, 1) == 1) {
            int drop = 0;
            for (const char *d = del; *d; d++)
                if (*d == buf[0]) { drop = 1; break; }
            if (!drop) write(1, buf, 1);
        }
        return 0;
    }

    if (i + 2 != argc) {
        dprintf(2, "usage: tr <set1> <set2>  |  tr -d <chars>\n");
        return 1;
    }
    const char *s1 = argv[i], *s2 = argv[i + 1];
    int s1l = (int)strlen(s1), s2l = (int)strlen(s2);
    char buf[1];
    while (read(0, buf, 1) == 1) {
        char c = buf[0];
        for (int j = 0; j < s1l; j++) {
            if (s1[j] == buf[0]) {
                c = (j < s2l) ? s2[j] : s2[s2l - 1];
                break;
            }
        }
        write(1, &c, 1);
    }
    return 0;
}
