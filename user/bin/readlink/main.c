#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: readlink PATH\n");
        return 1;
    }
    char buf[256];
    ssize_t n = readlink(argv[1], buf, sizeof(buf) - 1);
    if (n < 0) {
        perror("readlink");
        return 1;
    }
    buf[n] = '\n';
    write(1, buf, (size_t)n + 1);
    return 0;
}
