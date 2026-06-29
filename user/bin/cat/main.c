#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>

static void
cat_fd(int fd)
{
    char buf[512];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        write(1, buf, (size_t)n);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        cat_fd(0);  /* no args: read stdin */
        return 0;
    }
    int i;
    for (i = 1; i < argc; i++) {
        int fd = open(argv[i], 0);
        if (fd < 0) { perror(argv[i]); return 1; }
        cat_fd(fd);
        close(fd);
    }
    return 0;
}
