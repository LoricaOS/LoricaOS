/* more — page output one screenful at a time.
 *
 * Aegis consoles have no scrollback, so a pager is the only way to read
 * anything longer than one screen. This is the classic more(1):
 *
 *   more file...      page each file; with several files a
 *                     :::::::::::::: / FILENAME header separates them
 *   cmd | more        page stdin (keys come from /dev/console)
 *
 * Keys at the --More-- prompt:
 *   space        next page
 *   Enter        one more line
 *   q, Q, ^C     quit
 *
 * Pagination is by DISPLAY lines: a long line wraps every termwidth
 * columns and each wrap counts (tabs advance to 8-aligned stops). The
 * geometry comes from TIOCGWINSZ when the kernel supports it, else
 * 80x24. Named files show a byte percentage in the prompt. When stdout
 * is not a tty the tool degrades to plain cat.
 *
 * The key fd is put in raw mode (~ICANON ~ECHO ~ISIG, VMIN=1) so ^C
 * arrives as byte 0x03 instead of killing us mid-page; the saved
 * termios is restored on every exit path via atexit().
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

static int            s_rows  = 24;       /* terminal height            */
static int            s_cols  = 80;       /* terminal width             */
static int            s_paged = 1;        /* 0 = stdout not a tty: cat  */
static int            s_keyfd = -1;       /* where prompt keys come from */
static struct termios s_saved;            /* termios to restore         */
static int            s_saved_ok;         /* s_saved is valid           */

static int  s_lines_left;                 /* display lines before prompt */
static long s_bytes_done;                 /* bytes emitted of this file  */
static long s_bytes_total;                /* file size, -1 if unknown    */

static void restore_tty(void)
{
    if (s_saved_ok)
        tcsetattr(s_keyfd, TCSAFLUSH, &s_saved);
}

/* Pick the key source and put it in raw mode. If raw mode cannot be
 * set we keep going: reads may be line-buffered but paging still works. */
static void key_init(void)
{
    if (isatty(STDIN_FILENO))
        s_keyfd = STDIN_FILENO;
    else
        s_keyfd = open("/dev/console", O_RDONLY);

    if (s_keyfd < 0) {
        s_paged = 0;                       /* no key source: behave as cat */
        return;
    }

    if (tcgetattr(s_keyfd, &s_saved) == 0) {
        struct termios raw = s_saved;
        raw.c_lflag &= ~(ICANON | ECHO | ISIG);
        raw.c_cc[VMIN]  = 1;
        raw.c_cc[VTIME] = 0;
        if (tcsetattr(s_keyfd, TCSAFLUSH, &raw) == 0) {
            s_saved_ok = 1;
            atexit(restore_tty);
        }
    }
}

/* TIOCGWINSZ may or may not exist on this kernel — try stdout, then
 * the key fd, and fall back to 80x24 on any failure or nonsense. */
static void win_init(void)
{
    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0 &&
        (s_keyfd < 0 || ioctl(s_keyfd, TIOCGWINSZ, &ws) != 0))
        return;
    if (ws.ws_row >= 2 && ws.ws_col >= 2) {
        s_rows = ws.ws_row;
        s_cols = ws.ws_col;
    }
}

/* Show --More--, wait for one key, erase the prompt. Refills
 * s_lines_left (page or single line) or exits on q/Q/^C. */
static void prompt(void)
{
    char msg[48];
    int  len;

    if (s_bytes_total > 0) {
        long pct = s_bytes_done * 100 / s_bytes_total;
        if (pct > 100) pct = 100;
        len = snprintf(msg, sizeof(msg), "--More--(%ld%%)", pct);
    } else {
        len = snprintf(msg, sizeof(msg), "--More--");
    }

    fputs("\033[7m", stdout);
    fputs(msg, stdout);
    fputs("\033[m", stdout);
    fflush(stdout);

    char c = 0;
    if (read(s_keyfd, &c, 1) <= 0)
        c = 'q';                           /* key source gone: quit */

    /* Erase the prompt: \r, spaces over it, \r. */
    putchar('\r');
    for (int i = 0; i < len; i++)
        putchar(' ');
    putchar('\r');
    fflush(stdout);

    switch (c) {
    case ' ':
        s_lines_left = s_rows - 1;
        break;
    case '\r':
    case '\n':
        s_lines_left = 1;
        break;
    case 'q':
    case 'Q':
    case 0x03:                             /* ^C as a byte (ISIG off) */
        exit(0);
    default:                               /* unknown key: next page */
        s_lines_left = s_rows - 1;
        break;
    }
}

/* One display line consumed (wrap or newline). */
static void line_used(void)
{
    if (s_paged && s_lines_left > 0)
        s_lines_left--;
}

/* Page one stream. total is the byte size for the percentage prompt,
 * or -1 when unknown (stdin/pipes). */
static void page_stream(FILE *f, long total)
{
    int col = 0;
    int c;

    s_bytes_done  = 0;
    s_bytes_total = total;
    if (s_lines_left <= 0)
        s_lines_left = s_rows - 1;

    while ((c = fgetc(f)) != EOF) {
        if (s_paged && s_lines_left <= 0)
            prompt();

        s_bytes_done++;

        switch (c) {
        case '\n':
            putchar('\n');
            col = 0;
            line_used();
            break;
        case '\t':
            if (col >= s_cols) {           /* deferred wrap */
                col = 0;
                line_used();
                if (s_paged && s_lines_left <= 0)
                    prompt();
            }
            putchar('\t');
            col = (col + 8) & ~7;
            break;
        case '\r':
            putchar('\r');
            col = 0;
            break;
        case '\b':
            putchar('\b');
            if (col > 0)
                col--;
            break;
        default:
            if (col >= s_cols) {           /* deferred wrap */
                col = 0;
                line_used();
                if (s_paged && s_lines_left <= 0)
                    prompt();
            }
            putchar(c);
            if (c >= 0x20)                 /* other ctl chars: 0 width */
                col++;
            break;
        }
    }
    if (col > 0) {                         /* file without final newline */
        putchar('\n');
        line_used();
    }
    fflush(stdout);
}

/* ::::::::::::::  classic multi-file banner (3 display lines). */
static void banner(const char *name)
{
    static const char bar[] = "::::::::::::::";
    if (s_paged && s_lines_left < 3 && s_lines_left >= 0)
        prompt();
    printf("%s\n%s\n%s\n", bar, name, bar);
    line_used();
    line_used();
    line_used();
}

int main(int argc, char **argv)
{
    int rc = 0;

    if (!isatty(STDOUT_FILENO))
        s_paged = 0;

    if (s_paged)
        key_init();
    win_init();
    s_lines_left = s_rows - 1;

    if (argc < 2) {
        page_stream(stdin, -1);
        return 0;
    }

    for (int i = 1; i < argc; i++) {
        FILE *f;
        long  total = -1;

        if (strcmp(argv[i], "-") == 0) {
            f = stdin;
        } else {
            f = fopen(argv[i], "r");
            if (!f) {
                fprintf(stderr, "more: %s: cannot open\n", argv[i]);
                rc = 1;
                continue;
            }
            struct stat st;
            if (fstat(fileno(f), &st) == 0 && st.st_size > 0)
                total = (long)st.st_size;
        }

        if (argc > 2)
            banner(argv[i]);

        page_stream(f, total);
        if (f != stdin)
            fclose(f);
    }
    return rc;
}
