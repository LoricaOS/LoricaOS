#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>

/* Naive literal string grep — no regex, sufficient for shell tests */
static int
grep_fd(int fd, const char *pattern, int print_name, const char *name)
{
    char buf[4096];
    char line[512];
    int  line_len = 0;
    int  found    = 0;
    ssize_t n;
    size_t  pat_len = strlen(pattern);

    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        ssize_t i;
        for (i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n' || line_len == (int)sizeof(line) - 1) {
                line[line_len] = '\0';
                /* Search for pattern in line */
                if (line_len >= (int)pat_len) {
                    int j;
                    for (j = 0; j <= line_len - (int)pat_len; j++) {
                        if (memcmp(line + j, pattern, pat_len) == 0) {
                            if (print_name) {
                                write(1, name, strlen(name));
                                write(1, ":", 1);
                            }
                            write(1, line, (size_t)line_len);
                            write(1, "\n", 1);
                            found = 1;
                            break;
                        }
                    }
                }
                line_len = 0;
            } else {
                line[line_len++] = c;
            }
        }
    }
    return found;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        write(2, "usage: grep pattern [file...]\n", 30);
        return 2;
    }
    const char *pattern = argv[1];
    int found = 0;

    if (argc < 3) {
        found = grep_fd(0, pattern, 0, "(stdin)");
    } else {
        int i;
        int print_name = (argc > 3);
        for (i = 2; i < argc; i++) {
            int fd = open(argv[i], 0);
            if (fd < 0) { perror(argv[i]); continue; }
            if (grep_fd(fd, pattern, print_name, argv[i]))
                found = 1;
            close(fd);
        }
    }
    return found ? 0 : 1;
}
