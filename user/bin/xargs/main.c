/* xargs — build and run command lines from standard input.
 *
 *   xargs [-r] [-t] [-n MAX] [-I REPL] [command [initial-args...]]
 *
 * Reads items from stdin and runs `command initial-args... item...`
 * (default command: echo) once per batch, sequentially via
 * fork + execvp + waitpid.
 *
 * Input tokenization (POSIX xargs rules, simplified): items are
 * separated by blanks/newlines; double quotes, single quotes and
 * backslash escapes are honored ("a b" or a\ b is one item). There is
 * no -0 — Aegis find has no -print0, so NUL-delimited input would have
 * no producer.
 *
 *   -n MAX    at most MAX items per command invocation
 *   -I REPL   one item per invocation (implies -n 1); items are full
 *             LINES (leading blanks stripped), and every occurrence of
 *             REPL in the initial-args is replaced by the item
 *   -r        no-run-if-empty: skip the command entirely if stdin
 *             produced zero items (default runs it once, like GNU)
 *   -t        echo each constructed command line to stderr before
 *             running it
 *
 * HARD KERNEL LIMIT: Aegis execve copies at most 64 argv entries of
 * <=255 bytes each. We stay well inside it: each invocation is capped
 * at 32 total argv entries (command + initial args + items), and any
 * single item longer than 250 bytes is rejected with an error.
 *
 * Exit status (POSIX): 0 all invocations OK; 123 if any invocation
 * exited 1-125; 124 if a command exited 255 (stop); 125 killed by a
 * signal (stop); 126 command found but not executable (stop); 127
 * command not found (stop); 1 other errors.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/wait.h>

#define MAX_INVOKE_ARGS 32   /* command + initial args + items, per exec */
#define ITEM_MAX        250  /* longest accepted item, bytes */
#define ARG_BUF         256  /* per-argv-entry storage (kernel cap 255) */

static int s_trace;          /* -t */
static int s_exit;           /* sticky 123 from 1-125 child exits */

static char  s_items[MAX_INVOKE_ARGS][ARG_BUF]; /* batch item storage */
static char  s_sub[MAX_INVOKE_ARGS][ARG_BUF];   /* -I substituted args */
static char *s_argv[MAX_INVOKE_ARGS + 1];

static void usage(void)
{
    fprintf(stderr,
            "usage: xargs [-r] [-t] [-n max] [-I repl] [command [args...]]\n");
    exit(1);
}

static void item_too_long(void)
{
    fprintf(stderr, "xargs: item exceeds %d bytes (kernel argv limit)\n",
            ITEM_MAX);
    exit(1);
}

/* Read one whitespace-separated item, honoring quotes and backslash.
 * Returns 1 on item, 0 on EOF. Exits on malformed input. */
static int next_word(char *buf, size_t cap)
{
    int c;
    size_t n = 0;

    do {
        c = getchar();
    } while (c == ' ' || c == '\t' || c == '\n');
    if (c == EOF) return 0;

    while (c != EOF && c != ' ' && c != '\t' && c != '\n') {
        if (c == '"' || c == '\'') {
            int q = c;
            for (;;) {
                c = getchar();
                if (c == EOF) {
                    fprintf(stderr, "xargs: unterminated %s quote\n",
                            q == '"' ? "double" : "single");
                    exit(1);
                }
                if (c == q) break;
                if (n >= ITEM_MAX || n + 1 >= cap) item_too_long();
                buf[n++] = (char)c;
            }
        } else {
            if (c == '\\') {
                c = getchar();
                if (c == EOF) {
                    fprintf(stderr, "xargs: backslash at end of input\n");
                    exit(1);
                }
            }
            if (n >= ITEM_MAX || n + 1 >= cap) item_too_long();
            buf[n++] = (char)c;
        }
        c = getchar();
    }
    buf[n] = '\0';
    return 1;
}

/* -I mode: read one non-blank line, leading blanks stripped, newline
 * dropped, taken literally (no quote processing). 1 on line, 0 on EOF. */
static int next_line(char *buf, size_t cap)
{
    for (;;) {
        int c = getchar();
        size_t n = 0;
        if (c == EOF) return 0;
        while (c == ' ' || c == '\t')
            c = getchar();
        while (c != '\n' && c != EOF) {
            if (n >= ITEM_MAX || n + 1 >= cap) item_too_long();
            buf[n++] = (char)c;
            c = getchar();
        }
        buf[n] = '\0';
        if (n > 0) return 1;
        if (c == EOF) return 0;
        /* blank line: keep scanning */
    }
}

/* Copy tpl into dst replacing every occurrence of repl with item. */
static void subst(char *dst, size_t cap, const char *tpl,
                  const char *repl, const char *item)
{
    size_t rl = strlen(repl), il = strlen(item), n = 0;

    while (*tpl) {
        if (strncmp(tpl, repl, rl) == 0) {
            if (n + il >= cap) item_too_long();
            memcpy(dst + n, item, il);
            n += il;
            tpl += rl;
        } else {
            if (n + 1 >= cap) item_too_long();
            dst[n++] = *tpl++;
        }
    }
    dst[n] = '\0';
}

/* Run one constructed command line. Returns 0 to continue, or a final
 * exit code on a condition that must stop processing (POSIX). */
static int run_cmd(char **argv)
{
    pid_t pid;
    int st;

    if (s_trace) {
        for (int i = 0; argv[i]; i++)
            fprintf(stderr, i ? " %s" : "%s", argv[i]);
        fputc('\n', stderr);
    }

    pid = fork();
    if (pid < 0) {
        perror("xargs: fork");
        return 1;
    }
    if (pid == 0) {
        execvp(argv[0], argv);
        fprintf(stderr, "xargs: %s: %s\n", argv[0], strerror(errno));
        _exit(errno == ENOENT ? 127 : 126);
    }
    if (waitpid(pid, &st, 0) < 0) {
        perror("xargs: waitpid");
        return 1;
    }
    if (WIFSIGNALED(st)) {
        fprintf(stderr, "xargs: %s: terminated by signal %d\n",
                argv[0], WTERMSIG(st));
        return 125;
    }
    if (WIFEXITED(st)) {
        int code = WEXITSTATUS(st);
        if (code == 0) return 0;
        if (code == 255) {
            fprintf(stderr,
                    "xargs: %s: exited with status 255; aborting\n",
                    argv[0]);
            return 124;
        }
        if (code == 126 || code == 127) return code; /* exec failure */
        s_exit = 123;
    }
    return 0;
}

int main(int argc, char **argv)
{
    long n_max = 0;          /* 0 = unlimited */
    int r_flag = 0;
    const char *i_repl = NULL;
    int i = 1;

    for (; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || a[1] == '\0') break;
        if (strcmp(a, "--") == 0) { i++; break; }
        if (strcmp(a, "-r") == 0) {
            r_flag = 1;
        } else if (strcmp(a, "-t") == 0) {
            s_trace = 1;
        } else if (strncmp(a, "-n", 2) == 0) {
            const char *v = a[2] ? a + 2 : (++i < argc ? argv[i] : NULL);
            if (!v) usage();
            n_max = atol(v);
            if (n_max <= 0) usage();
        } else if (strncmp(a, "-I", 2) == 0) {
            i_repl = a[2] ? a + 2 : (++i < argc ? argv[i] : NULL);
            if (!i_repl || !i_repl[0]) usage();
        } else {
            usage();
        }
    }

    /* Command template: argv[i..argc), defaulting to "echo". */
    int base = 0;
    if (i < argc) {
        for (; i < argc; i++) {
            if (base >= MAX_INVOKE_ARGS) {
                fprintf(stderr, "xargs: too many initial arguments "
                                "(max %d total argv entries)\n",
                        MAX_INVOKE_ARGS);
                return 1;
            }
            s_argv[base++] = argv[i];
        }
    } else {
        s_argv[base++] = "echo";
    }

    if (i_repl) {
        /* One full line per invocation; REPL replaced in every arg. */
        char line[ARG_BUF];
        char *tpl[MAX_INVOKE_ARGS];
        memcpy(tpl, s_argv, sizeof(char *) * (size_t)base);
        while (next_line(line, sizeof(line))) {
            for (int k = 0; k < base; k++) {
                subst(s_sub[k], sizeof(s_sub[k]), tpl[k], i_repl, line);
                s_argv[k] = s_sub[k];
            }
            s_argv[base] = NULL;
            int rc = run_cmd(s_argv);
            if (rc) return rc;
        }
        return s_exit;
    }

    int slots = MAX_INVOKE_ARGS - base;
    if (slots < 1) {
        fprintf(stderr, "xargs: no room for items "
                        "(max %d total argv entries)\n", MAX_INVOKE_ARGS);
        return 1;
    }
    if (n_max > 0 && n_max < slots)
        slots = (int)n_max;

    int got_any = 0;
    int count = 0;
    while (next_word(s_items[count], sizeof(s_items[count]))) {
        got_any = 1;
        s_argv[base + count] = s_items[count];
        count++;
        if (count == slots) {
            s_argv[base + count] = NULL;
            int rc = run_cmd(s_argv);
            if (rc) return rc;
            count = 0;
        }
    }
    if (count > 0 || (!got_any && !r_flag)) {
        s_argv[base + count] = NULL;
        int rc = run_cmd(s_argv);
        if (rc) return rc;
    }
    return s_exit;
}
