/* test (and `[`) — POSIX-ish file/string/integer tests.
 * Supported:
 *   -e -r -w -x -f -d -L -h -s   (file tests)
 *   -n STR  -z STR
 *   STR1 = STR2,  STR1 != STR2
 *   N1 -eq N2, -ne, -lt, -le, -gt, -ge
 * If invoked as `[`, requires final argument to be `]`. */
#include <unistd.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int
file_test(char op, const char *path)
{
    struct stat st;
    int rc;
    if (op == 'L' || op == 'h')
        rc = lstat(path, &st);
    else
        rc = stat(path, &st);
    if (rc != 0) return 1;

    switch (op) {
    case 'e': return 0;
    case 'r': return access(path, R_OK) == 0 ? 0 : 1;
    case 'w': return access(path, W_OK) == 0 ? 0 : 1;
    case 'x': return access(path, X_OK) == 0 ? 0 : 1;
    case 'f': return S_ISREG(st.st_mode) ? 0 : 1;
    case 'd': return S_ISDIR(st.st_mode) ? 0 : 1;
    case 'L': /* fallthrough */
    case 'h': return S_ISLNK(st.st_mode) ? 0 : 1;
    case 's': return st.st_size > 0 ? 0 : 1;
    default:  return 1;
    }
}

int
main(int argc, char **argv)
{
    /* If invoked as `[`, last arg must be `]`. */
    int prog_is_bracket = 0;
    {
        const char *prog = argv[0];
        const char *base = prog;
        for (const char *q = prog; *q; q++) if (*q == '/') base = q + 1;
        if (strcmp(base, "[") == 0) prog_is_bracket = 1;
    }
    if (prog_is_bracket) {
        if (argc < 2 || strcmp(argv[argc - 1], "]") != 0) {
            dprintf(2, "[: missing ']'\n");
            return 2;
        }
        argc--;  /* drop trailing ] */
    }

    if (argc <= 1) return 1;  /* no args: false */

    /* Single-arg form: true iff non-empty. */
    if (argc == 2) return argv[1][0] ? 0 : 1;

    /* Two-arg form: -OP STR */
    if (argc == 3 && argv[1][0] == '-' && argv[1][1] && !argv[1][2]) {
        char op = argv[1][1];
        if (op == 'n') return argv[2][0] ? 0 : 1;
        if (op == 'z') return argv[2][0] ? 1 : 0;
        return file_test(op, argv[2]);
    }

    /* Three-arg form: STR1 OP STR2 */
    if (argc == 4) {
        const char *a = argv[1], *op = argv[2], *b = argv[3];
        if (strcmp(op, "=")  == 0) return strcmp(a, b) == 0 ? 0 : 1;
        if (strcmp(op, "!=") == 0) return strcmp(a, b) != 0 ? 0 : 1;
        long la = atol(a), lb = atol(b);
        if (strcmp(op, "-eq") == 0) return la == lb ? 0 : 1;
        if (strcmp(op, "-ne") == 0) return la != lb ? 0 : 1;
        if (strcmp(op, "-lt") == 0) return la <  lb ? 0 : 1;
        if (strcmp(op, "-le") == 0) return la <= lb ? 0 : 1;
        if (strcmp(op, "-gt") == 0) return la >  lb ? 0 : 1;
        if (strcmp(op, "-ge") == 0) return la >= lb ? 0 : 1;
    }

    dprintf(2, "test: unsupported expression\n");
    return 2;
}
