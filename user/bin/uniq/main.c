/* uniq — collapse adjacent identical lines. */
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>

#define BUF 4096

int
main(int argc, char **argv)
{
    int fd = 0;
    if (argc >= 2) {
        fd = open(argv[1], 0);
        if (fd < 0) { perror(argv[1]); return 1; }
    }
    char prev[BUF] = "";
    int  prev_len  = -1;
    char cur[BUF];
    int  cur_len = 0;
    char c;
    while (read(fd, &c, 1) == 1) {
        if (c == '\n' || cur_len == BUF - 1) {
            cur[cur_len] = '\0';
            if (prev_len < 0 || cur_len != prev_len ||
                memcmp(cur, prev, (size_t)cur_len) != 0) {
                write(1, cur, (size_t)cur_len);
                write(1, "\n", 1);
                memcpy(prev, cur, (size_t)cur_len);
                prev[cur_len] = '\0';
                prev_len = cur_len;
            }
            cur_len = 0;
        } else {
            cur[cur_len++] = c;
        }
    }
    if (fd > 0) close(fd);
    return 0;
}
