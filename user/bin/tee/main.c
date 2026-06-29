/* tee — copy stdin to stdout AND to each named file. -a appends. */
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>

int
main(int argc, char **argv)
{
    int append = 0, i = 1;
    if (i < argc && strcmp(argv[i], "-a") == 0) { append = 1; i++; }

    int fds[16];
    int nfds = 0;
    for (; i < argc && nfds < 16; i++) {
        int flags = O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC);
        int fd = open(argv[i], flags, 0644);
        if (fd < 0) { perror(argv[i]); continue; }
        fds[nfds++] = fd;
    }

    char buf[512];
    ssize_t n;
    while ((n = read(0, buf, sizeof(buf))) > 0) {
        write(1, buf, (size_t)n);
        for (int j = 0; j < nfds; j++)
            write(fds[j], buf, (size_t)n);
    }
    for (int j = 0; j < nfds; j++) close(fds[j]);
    return 0;
}
