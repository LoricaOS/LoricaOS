#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>

static void
count_fd(int fd, long *lines, long *words, long *bytes)
{
    char buf[512];
    ssize_t n;
    int in_word = 0;

    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        ssize_t i;
        *bytes += n;
        for (i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n') (*lines)++;
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                in_word = 0;
            } else {
                if (!in_word) { (*words)++; in_word = 1; }
            }
        }
    }
}

int main(int argc, char **argv) {
    long lines = 0, words = 0, bytes = 0;
    int do_l = 0, do_w = 0, do_c = 0;
    int first_file = 1;

    /* Parse flags: arguments starting with '-' are flag strings, not files */
    if (argc >= 2 && argv[1][0] == '-' && argv[1][1] != '\0') {
        const char *p = argv[1] + 1;
        while (*p) {
            if (*p == 'l') do_l = 1;
            else if (*p == 'w') do_w = 1;
            else if (*p == 'c') do_c = 1;
            p++;
        }
        first_file = 2;
    }

    /* Default: all three counts */
    if (!do_l && !do_w && !do_c) { do_l = 1; do_w = 1; do_c = 1; }

    if (first_file >= argc) {
        count_fd(0, &lines, &words, &bytes);
    } else {
        int i;
        for (i = first_file; i < argc; i++) {
            int fd = open(argv[i], 0);
            if (fd < 0) { perror(argv[i]); return 1; }
            count_fd(fd, &lines, &words, &bytes);
            close(fd);
        }
    }

    char buf[64];
    int len = 0;
    if (do_l && do_w && do_c) {
        len = snprintf(buf, sizeof(buf), "%ld %ld %ld\n", lines, words, bytes);
    } else if (do_c) {
        len = snprintf(buf, sizeof(buf), "%ld\n", bytes);
    } else if (do_l) {
        len = snprintf(buf, sizeof(buf), "%ld\n", lines);
    } else {
        len = snprintf(buf, sizeof(buf), "%ld\n", words);
    }
    write(1, buf, (size_t)len);
    return 0;
}
