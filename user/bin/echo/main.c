#include <stdio.h>
int main(int argc, char **argv) {
    int i;
    for (i = 1; i < argc; i++) {
        fputs(argv[i], stdout);
        if (i + 1 < argc) fputc(' ', stdout);
    }
    fputc('\n', stdout);
    return 0;
}
