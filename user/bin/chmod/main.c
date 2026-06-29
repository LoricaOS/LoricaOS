#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: chmod MODE FILE\n");
        return 1;
    }
    unsigned long mode = strtoul(argv[1], NULL, 8);
    if (chmod(argv[2], (mode_t)mode) != 0) {
        perror("chmod");
        return 1;
    }
    return 0;
}
