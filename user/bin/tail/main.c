/* tail — print last N lines of a file (or stdin). Default N=10.
 * No -f. Ring buffer of last N lines, max line 4096, max ring 1024. */
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define MAX_LINES 1024
#define MAX_LINE  4096

static int  s_n = 10;
static char s_ring[MAX_LINES][MAX_LINE];
static int  s_lens[MAX_LINES];
static int  s_count;       /* number of lines stored (capped at s_n) */
static int  s_head;        /* index of next slot to write */

static void
push(const char *line, int len)
{
    if (len > MAX_LINE - 1) len = MAX_LINE - 1;
    memcpy(s_ring[s_head], line, len);
    s_lens[s_head] = len;
    s_head = (s_head + 1) % s_n;
    if (s_count < s_n) s_count++;
}

static void
emit_all(void)
{
    int start = (s_head + s_n - s_count) % s_n;
    for (int k = 0; k < s_count; k++) {
        int idx = (start + k) % s_n;
        write(1, s_ring[idx], s_lens[idx]);
        write(1, "\n", 1);
    }
}

static void
tail_fd(int fd)
{
    char line[MAX_LINE];
    int len = 0;
    char c;
    while (read(fd, &c, 1) == 1) {
        if (c == '\n') {
            push(line, len);
            len = 0;
        } else if (len < MAX_LINE - 1) {
            line[len++] = c;
        }
    }
    if (len > 0) push(line, len);  /* trailing line w/o newline */
}

int
main(int argc, char **argv)
{
    int i = 1;
    if (argc >= 3 && strcmp(argv[1], "-n") == 0) {
        s_n = atoi(argv[2]);
        if (s_n < 1)         s_n = 1;
        if (s_n > MAX_LINES) s_n = MAX_LINES;
        i = 3;
    }
    if (i >= argc) {
        tail_fd(0);
    } else {
        for (; i < argc; i++) {
            int fd = open(argv[i], 0);
            if (fd < 0) { perror(argv[i]); return 1; }
            tail_fd(fd);
            close(fd);
        }
    }
    emit_all();
    return 0;
}
