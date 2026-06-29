/* env — print the environment, or run a program with KEY=VAL set. */
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

extern char **environ;

int
main(int argc, char **argv)
{
    int i = 1;
    while (i < argc && strchr(argv[i], '=') != NULL) {
        putenv(argv[i]);
        i++;
    }
    if (i >= argc) {
        for (char **e = environ; *e; e++) {
            int len = (int)strlen(*e);
            write(1, *e, (size_t)len);
            write(1, "\n", 1);
        }
        return 0;
    }
    execvp(argv[i], &argv[i]);
    perror(argv[i]);
    return 127;
}
