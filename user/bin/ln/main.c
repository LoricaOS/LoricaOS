#include <stdio.h>
#include <unistd.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc == 4 && strcmp(argv[1], "-s") == 0)
        return symlink(argv[2], argv[3]) == 0 ? 0 : 1;
    if (argc == 3)
        return symlink(argv[1], argv[2]) == 0 ? 0 : 1;
    fprintf(stderr, "usage: ln [-s] target linkname\n");
    return 1;
}
