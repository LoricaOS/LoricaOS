/* yes — print "y" (or argv[1]) repeatedly until write fails. */
#include <unistd.h>
#include <string.h>

int
main(int argc, char **argv)
{
    const char *s = (argc >= 2) ? argv[1] : "y";
    int len = (int)strlen(s);
    char buf[512];
    int filled = 0;
    while (filled + len + 1 < (int)sizeof(buf)) {
        for (int j = 0; j < len; j++) buf[filled++] = s[j];
        buf[filled++] = '\n';
    }
    for (;;) {
        if (write(1, buf, (size_t)filled) <= 0) break;
    }
    return 0;
}
