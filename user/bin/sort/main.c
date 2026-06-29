#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define MAX_LINES 512
#define MAX_LINE  512

static char  s_lines[MAX_LINES][MAX_LINE];
static int   s_nlines = 0;

static void
read_lines_fd(int fd)
{
    char buf[4096];
    char cur[MAX_LINE];
    int  cur_len = 0;
    ssize_t n;

    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        ssize_t i;
        for (i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n' || cur_len == MAX_LINE - 1) {
                cur[cur_len] = '\0';
                if (s_nlines < MAX_LINES) {
                    memcpy(s_lines[s_nlines], cur, (size_t)(cur_len + 1));
                    s_nlines++;
                }
                cur_len = 0;
            } else {
                cur[cur_len++] = c;
            }
        }
    }
    /* flush last partial line without trailing newline */
    if (cur_len > 0 && s_nlines < MAX_LINES) {
        cur[cur_len] = '\0';
        memcpy(s_lines[s_nlines], cur, (size_t)(cur_len + 1));
        s_nlines++;
    }
}

static int
cmp_str(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        read_lines_fd(0);
    } else {
        int i;
        for (i = 1; i < argc; i++) {
            int fd = open(argv[i], 0);
            if (fd < 0) { perror(argv[i]); return 1; }
            read_lines_fd(fd);
            close(fd);
        }
    }

    qsort(s_lines, (size_t)s_nlines, MAX_LINE, cmp_str);

    int i;
    for (i = 0; i < s_nlines; i++) {
        write(1, s_lines[i], strlen(s_lines[i]));
        write(1, "\n", 1);
    }
    return 0;
}
