#include "stsh.h"

static struct termios s_orig;
static int s_raw_active;

static void
raw_mode_on(void)
{
    if (s_raw_active)
        return;
    tcgetattr(STDIN_FILENO, &s_orig);
    struct termios raw = s_orig;
    raw.c_lflag &= ~(ICANON | ECHO | ISIG);
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    s_raw_active = 1;
}

static void
raw_mode_off(void)
{
    if (!s_raw_active)
        return;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &s_orig);
    s_raw_active = 0;
}

static void
redraw(const char *prompt, const char *buf, int len, int pos)
{
    int plen = (int)strlen(prompt);

    /* Carriage return, prompt, buffer, clear to EOL */
    write(STDOUT_FILENO, "\r", 1);
    write(STDOUT_FILENO, prompt, plen);
    write(STDOUT_FILENO, buf, len);
    write(STDOUT_FILENO, "\033[K", 3); /* clear to end of line */

    /* Position cursor */
    int cursor_col = plen + pos;
    int end_col    = plen + len;
    if (cursor_col < end_col) {
        /* Move cursor back from end to desired position */
        char seq[32];
        snprintf(seq, sizeof(seq), "\033[%dD", end_col - cursor_col);
        write(STDOUT_FILENO, seq, strlen(seq));
    }
}

/*
 * editor_readline — raw terminal line editor.
 * Returns length of line, or -1 on EOF (Ctrl-D on empty line).
 */
int
editor_readline(const char *prompt, char *buf, int buflen)
{
    int pos = 0; /* cursor position within buf */
    int len = 0; /* total chars in buf */

    buf[0] = '\0';
    raw_mode_on();

    write(STDOUT_FILENO, prompt, strlen(prompt));

    for (;;) {
        unsigned char c;
        int n = read(STDIN_FILENO, &c, 1);
        if (n <= 0) {
            raw_mode_off();
            return -1;
        }

        switch (c) {
        case '\r':  /* Enter */
        case '\n':
            buf[len] = '\0';
            raw_mode_off();
            write(STDOUT_FILENO, "\n", 1);
            hist_reset_cursor();
            return len;

        case 4:  /* Ctrl-D */
            if (len == 0) {
                raw_mode_off();
                write(STDOUT_FILENO, "\n", 1);
                return -1;
            }
            break;

        case 3:  /* Ctrl-C */
            buf[0] = '\0';
            len = 0;
            pos = 0;
            raw_mode_off();
            write(STDOUT_FILENO, "\n", 1);
            return 0;

        case 1:  /* Ctrl-A — Home */
            pos = 0;
            redraw(prompt, buf, len, pos);
            break;
        case 5:  /* Ctrl-E — End */
            pos = len;
            redraw(prompt, buf, len, pos);
            break;
        case 11: /* Ctrl-K — kill to end of line */
            len = pos;
            buf[len] = '\0';
            redraw(prompt, buf, len, pos);
            break;
        case 21: /* Ctrl-U — kill line (to beginning) */
            memmove(buf, &buf[pos], len - pos);
            len -= pos;
            pos = 0;
            buf[len] = '\0';
            redraw(prompt, buf, len, pos);
            break;
        case 23: /* Ctrl-W — delete word backward */
            if (pos > 0) {
                int end = pos;
                while (pos > 0 && buf[pos - 1] == ' ') pos--;
                while (pos > 0 && buf[pos - 1] != ' ') pos--;
                memmove(&buf[pos], &buf[end], len - end);
                len -= (end - pos);
                buf[len] = '\0';
                redraw(prompt, buf, len, pos);
            }
            break;
        case 12: /* Ctrl-L — clear screen */
            write(STDOUT_FILENO, "\033[2J\033[H", 7);
            redraw(prompt, buf, len, pos);
            break;

        case '\t': /* Tab — completion */
            complete(buf, &pos, &len, prompt);
            redraw(prompt, buf, len, pos);
            break;

        case 127: /* Backspace (DEL) */
        case 8:   /* Ctrl-H */
            if (pos > 0) {
                memmove(&buf[pos - 1], &buf[pos], len - pos);
                pos--;
                len--;
                buf[len] = '\0';
                redraw(prompt, buf, len, pos);
            }
            break;

        case 27: /* Escape sequence */
            {
                unsigned char seq[3];
                if (read(STDIN_FILENO, &seq[0], 1) <= 0) break;
                if (seq[0] != '[') break;
                if (read(STDIN_FILENO, &seq[1], 1) <= 0) break;

                switch (seq[1]) {
                case 'A': { /* Up arrow — history prev */
                    const char *h = hist_prev();
                    if (h) {
                        strncpy(buf, h, buflen - 1);
                        buf[buflen - 1] = '\0';
                        len = (int)strlen(buf);
                        pos = len;
                        redraw(prompt, buf, len, pos);
                    }
                    break;
                }
                case 'B': { /* Down arrow — history next */
                    const char *h = hist_next();
                    if (h) {
                        strncpy(buf, h, buflen - 1);
                        buf[buflen - 1] = '\0';
                        len = (int)strlen(buf);
                        pos = len;
                    } else {
                        buf[0] = '\0';
                        len = 0;
                        pos = 0;
                    }
                    redraw(prompt, buf, len, pos);
                    break;
                }
                case 'C': /* Right arrow */
                    if (pos < len) {
                        pos++;
                        write(STDOUT_FILENO, "\033[C", 3);
                    }
                    break;
                case 'D': /* Left arrow */
                    if (pos > 0) {
                        pos--;
                        write(STDOUT_FILENO, "\033[D", 3);
                    }
                    break;
                case 'H': /* Home */
                    pos = 0;
                    redraw(prompt, buf, len, pos);
                    break;
                case 'F': /* End */
                    pos = len;
                    redraw(prompt, buf, len, pos);
                    break;
                case '3': { /* Delete key: ESC[3~ */
                    unsigned char tilde;
                    if (read(STDIN_FILENO, &tilde, 1) == 1 && tilde == '~') {
                        if (pos < len) {
                            memmove(&buf[pos], &buf[pos + 1], len - pos - 1);
                            len--;
                            buf[len] = '\0';
                            redraw(prompt, buf, len, pos);
                        }
                    }
                    break;
                }
                default:
                    break;
                }
            }
            break;

        default:
            /* Printable character — insert at cursor */
            if (c >= 32 && c < 127 && len < buflen - 1) {
                memmove(&buf[pos + 1], &buf[pos], len - pos);
                buf[pos] = c;
                pos++;
                len++;
                buf[len] = '\0';
                redraw(prompt, buf, len, pos);
            }
            break;
        }
    }
}

/*
 * detect_paste — check if multiple bytes are available (paste produces
 * a burst of characters). Available for integration into the editor
 * event loop but not wired in v1.
 *
 * Usage: after reading a byte, call detect_paste(). If it returns > 0,
 * the next N bytes are likely pasted text and can be bulk-inserted
 * without per-char redraw.
 */
#if 0
static int
detect_paste(void)
{
    struct termios tmp;
    tcgetattr(STDIN_FILENO, &tmp);
    tmp.c_cc[VMIN]  = 0;
    tmp.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &tmp);

    unsigned char peek;
    int avail = (read(STDIN_FILENO, &peek, 1) == 1) ? 1 : 0;
    /* Restore blocking read */
    tmp.c_cc[VMIN]  = 1;
    tmp.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &tmp);

    /* If we read a byte, we need to handle it — push it back via ungetc
     * is not possible on raw fd. For v2: use a small lookahead buffer. */
    (void)peek;
    return avail;
}
#endif
