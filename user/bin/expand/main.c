/* expand — convert tabs to spaces.  -t N sets tab stops (default 8). */
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>

int
main(int argc, char **argv)
{
    int tab = 8;
    int i = 1;
    if (argc >= 3 && argv[1][0] == '-' && argv[1][1] == 't') {
        tab = atoi(argv[2]);
        if (tab <= 0) tab = 8;
        i = 3;
    }
    int fd = (i < argc) ? open(argv[i], 0) : 0;
    if (fd < 0) { perror(argv[i]); return 1; }

    int  col = 0;
    char c;
    while (read(fd, &c, 1) == 1) {
        if (c == '\t') {
            int spaces = tab - (col % tab);
            for (int j = 0; j < spaces; j++) write(1, " ", 1);
            col += spaces;
        } else if (c == '\n') {
            write(1, &c, 1);
            col = 0;
        } else {
            write(1, &c, 1);
            col++;
        }
    }
    if (fd > 0) close(fd);
    return 0;
}
