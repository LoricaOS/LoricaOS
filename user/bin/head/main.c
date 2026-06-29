/* head — print first N lines of each file (or stdin). Default N=10.
 * Usage: head [-n N] [FILE...] */
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static int s_n = 10;

static void
head_fd(int fd)
{
    char buf[1];
    int lines = 0;
    while (lines < s_n && read(fd, buf, 1) == 1) {
        write(1, buf, 1);
        if (buf[0] == '\n') lines++;
    }
}

int
main(int argc, char **argv)
{
    int i = 1;
    if (argc >= 3 && strcmp(argv[1], "-n") == 0) {
        s_n = atoi(argv[2]);
        if (s_n < 0) s_n = 0;
        i = 3;
    }
    if (i >= argc) { head_fd(0); return 0; }
    for (; i < argc; i++) {
        int fd = open(argv[i], 0);
        if (fd < 0) { perror(argv[i]); return 1; }
        head_fd(fd);
        close(fd);
    }
    return 0;
}
