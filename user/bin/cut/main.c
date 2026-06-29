/* cut — extract delimiter-separated fields from each input line.
 * Usage: cut -d <c> -f <list> [FILE]
 * <list> is comma-separated 1-based field numbers (no ranges). */
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define MAX_FIELDS 64
#define MAX_LINE   4096

int
main(int argc, char **argv)
{
    char delim = '\t';
    int  fields[MAX_FIELDS];
    int  nfields = 0;
    int  i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            delim = argv[++i][0];
        } else if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
            char *spec = argv[++i];
            char *tok = strtok(spec, ",");
            while (tok && nfields < MAX_FIELDS) {
                fields[nfields++] = atoi(tok);
                tok = strtok(NULL, ",");
            }
        } else {
            break;
        }
    }
    if (nfields == 0) {
        dprintf(2, "usage: cut -d <c> -f <list> [file]\n");
        return 1;
    }

    int fd = (i < argc) ? open(argv[i], 0) : 0;
    if (fd < 0) { perror(argv[i]); return 1; }

    char line[MAX_LINE];
    int  li = 0;
    char c;
    while (read(fd, &c, 1) == 1) {
        if (c == '\n' || li == MAX_LINE - 1) {
            line[li] = '\0';
            int field = 1, start = 0, written = 0;
            for (int j = 0; j <= li; j++) {
                if (line[j] == delim || line[j] == '\0') {
                    for (int k = 0; k < nfields; k++) {
                        if (fields[k] == field) {
                            if (written) write(1, &delim, 1);
                            write(1, line + start, (size_t)(j - start));
                            written = 1;
                            break;
                        }
                    }
                    field++;
                    start = j + 1;
                }
            }
            write(1, "\n", 1);
            li = 0;
        } else {
            line[li++] = c;
        }
    }
    if (fd > 0) close(fd);
    return 0;
}
