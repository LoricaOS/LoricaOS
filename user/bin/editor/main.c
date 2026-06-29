/* user/bin/editor/main.c — Aegis Text Editor (external Lumen client)
 *
 * A small load/edit/save text editor speaking the Lumen external window
 * protocol (same pattern as settings / terminal). Line-buffer model with
 * a cursor; insert/delete/newline/backspace; arrow navigation; vertical
 * and horizontal scrolling; save via Ctrl+S or the on-screen Save button.
 *
 * Path: argv[1] if given, else /root/untitled.txt (desktop launches pass
 * no args). A missing file opens as an empty new document.
 *
 * The text area uses the fixed 10x20 bitmap font (draw.h FONT_W/FONT_H)
 * so cursor<->cell math is exact; the chrome uses the TTF UI font.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <stdint.h>
#include <fcntl.h>
#include <errno.h>

#include <glyph.h>
#include <lumen_client.h>
#include "font.h"

#define WIN_W 640
#define WIN_H 460

#define MAX_LINES 1024
#define MAX_COL   512      /* including the NUL terminator */

#define TITLE_H   36
#define STATUS_H  24
#define TEXT_X    8
#define TEXT_Y    (TITLE_H + 4)
#define TEXT_W    (WIN_W - 2 * TEXT_X)
#define TEXT_H    (WIN_H - TITLE_H - STATUS_H - 8)

#define TAB_WIDTH 4

/* Title-bar buttons (right-aligned). Lumen's window chrome already
 * provides a close button, so the editor only offers Save. */
#define BTN_W 70
#define BTN_H 24
#define SAVE_X (WIN_W - BTN_W - 12)
#define BTN_Y  ((TITLE_H - BTN_H) / 2)

/* Synthetic arrow codes from Lumen's CSI translator. */
#define KEY_UP    ((char)0xF1)
#define KEY_DOWN  ((char)0xF2)
#define KEY_RIGHT ((char)0xF3)
#define KEY_LEFT  ((char)0xF4)
#define KEY_CTRL_S 0x13

typedef struct {
    int             lfd;
    lumen_window_t *lwin;
    surface_t       surf;
    int             fb_w, fb_h;
    int             dirty;
    int             done;

    char  path[256];
    char  lines[MAX_LINES][MAX_COL];
    int   nlines;

    int   cur_row, cur_col;   /* cursor */
    int   top, leftcol;       /* scroll offsets */
    int   modified;

    int   editing_name;       /* 1 = the title is an editable path field */
    char  name_edit[256];

    char  status[96];
    uint32_t status_color;
} ed_state_t;

static ed_state_t g_ed;
static volatile sig_atomic_t s_term;
static void sigterm_handler(int s) { (void)s; s_term = 1; }

/* ── Drawing helpers ──────────────────────────────────────────────────── */

static void chrome_text(int sz, int x, int y, const char *s, uint32_t color)
{
    if (g_font_ui)
        font_draw_text(&g_ed.surf, g_font_ui, sz, x, y, s, color);
    else
        draw_text_t(&g_ed.surf, x, y, s, color);
}

static int chrome_w(int sz, const char *s)
{
    if (g_font_ui) return font_text_width(g_font_ui, sz, s);
    return (int)strlen(s) * FONT_W;
}

static int vis_rows(void) { return TEXT_H / FONT_H; }
static int vis_cols(void) { return TEXT_W / FONT_W; }

static void set_status(const char *msg, uint32_t color)
{
    snprintf(g_ed.status, sizeof(g_ed.status), "%s", msg);
    g_ed.status_color = color;
}

/* ── File load / save ─────────────────────────────────────────────────── */

static void ensure_one_line(void)
{
    if (g_ed.nlines == 0) {
        g_ed.lines[0][0] = '\0';
        g_ed.nlines = 1;
    }
}

static void load_file(const char *path)
{
    snprintf(g_ed.path, sizeof(g_ed.path), "%s", path);
    g_ed.nlines = 0;
    g_ed.cur_row = g_ed.cur_col = g_ed.top = g_ed.leftcol = 0;

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        ensure_one_line();
        set_status("new file", THEME_TEXT_DIM);
        return;
    }

    int col = 0;
    char ch;
    ssize_t n;
    int truncated = 0;
    while ((n = read(fd, &ch, 1)) == 1) {
        if (g_ed.nlines >= MAX_LINES) { truncated = 1; break; }
        if (ch == '\n') {
            g_ed.lines[g_ed.nlines][col] = '\0';
            g_ed.nlines++;
            col = 0;
        } else if (ch == '\t') {
            for (int i = 0; i < TAB_WIDTH && col < MAX_COL - 1; i++)
                g_ed.lines[g_ed.nlines][col++] = ' ';
        } else if (ch != '\r') {
            if (col < MAX_COL - 1)
                g_ed.lines[g_ed.nlines][col++] = ch;
            /* else: silently drop overflow chars on this line */
        }
    }
    /* Trailing partial line (no final newline). */
    if (g_ed.nlines < MAX_LINES) {
        g_ed.lines[g_ed.nlines][col] = '\0';
        g_ed.nlines++;
    }
    close(fd);
    ensure_one_line();
    if (truncated)
        set_status("file too large — truncated", THEME_WARN);
    else
        set_status("opened", THEME_OK);
}

static void save_file(void)
{
    int fd = open(g_ed.path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        char m[96];
        snprintf(m, sizeof(m), "save failed: %s", strerror(errno));
        set_status(m, THEME_ERROR);
        dprintf(2, "[EDIT] save failed errno=%d path=%s\n", errno, g_ed.path);
        g_ed.dirty = 1;
        return;
    }
    for (int i = 0; i < g_ed.nlines; i++) {
        int len = (int)strlen(g_ed.lines[i]);
        if (len > 0 && write(fd, g_ed.lines[i], (size_t)len) != len)
            break;
        if (i < g_ed.nlines - 1)
            write(fd, "\n", 1);
    }
    close(fd);
    g_ed.modified = 0;
    char m[96];
    snprintf(m, sizeof(m), "saved %d line%s", g_ed.nlines,
             g_ed.nlines == 1 ? "" : "s");
    set_status(m, THEME_OK);
    dprintf(2, "[EDIT] saved %s (%d lines)\n", g_ed.path, g_ed.nlines);
    g_ed.dirty = 1;
}

/* ── Editing primitives ───────────────────────────────────────────────── */

static void mark_modified(void)
{
    if (!g_ed.modified) { g_ed.modified = 1; }
    g_ed.status[0] = '\0';
}

static void insert_char(char c)
{
    char *ln = g_ed.lines[g_ed.cur_row];
    int len = (int)strlen(ln);
    if (len >= MAX_COL - 1) return;           /* line full */
    memmove(&ln[g_ed.cur_col + 1], &ln[g_ed.cur_col],
            (size_t)(len - g_ed.cur_col + 1));
    ln[g_ed.cur_col] = c;
    g_ed.cur_col++;
    mark_modified();
}

static void insert_newline(void)
{
    if (g_ed.nlines >= MAX_LINES) return;
    /* Shift lines below down by one. */
    for (int i = g_ed.nlines; i > g_ed.cur_row + 1; i--)
        memcpy(g_ed.lines[i], g_ed.lines[i - 1], MAX_COL);
    char *cur = g_ed.lines[g_ed.cur_row];
    char *next = g_ed.lines[g_ed.cur_row + 1];
    /* Tail of current line moves to the new line (distinct row buffers). */
    int taillen = (int)strlen(&cur[g_ed.cur_col]);
    memcpy(next, &cur[g_ed.cur_col], (size_t)taillen + 1);
    cur[g_ed.cur_col] = '\0';
    g_ed.nlines++;
    g_ed.cur_row++;
    g_ed.cur_col = 0;
    mark_modified();
}

static void delete_back(void)
{
    if (g_ed.cur_col > 0) {
        char *ln = g_ed.lines[g_ed.cur_row];
        int len = (int)strlen(ln);
        memmove(&ln[g_ed.cur_col - 1], &ln[g_ed.cur_col],
                (size_t)(len - g_ed.cur_col + 1));
        g_ed.cur_col--;
        mark_modified();
    } else if (g_ed.cur_row > 0) {
        /* Merge into previous line. */
        char *prev = g_ed.lines[g_ed.cur_row - 1];
        char *cur = g_ed.lines[g_ed.cur_row];
        int plen = (int)strlen(prev);
        int clen = (int)strlen(cur);
        if (plen + clen < MAX_COL - 1)
            memcpy(&prev[plen], cur, (size_t)(clen + 1));
        else
            prev[plen] = '\0';
        for (int i = g_ed.cur_row; i < g_ed.nlines - 1; i++)
            memcpy(g_ed.lines[i], g_ed.lines[i + 1], MAX_COL);
        g_ed.nlines--;
        g_ed.cur_row--;
        g_ed.cur_col = plen;
        mark_modified();
    }
}

/* ── Cursor / scrolling ───────────────────────────────────────────────── */

static int line_len(int row) { return (int)strlen(g_ed.lines[row]); }

static void clamp_col(void)
{
    int len = line_len(g_ed.cur_row);
    if (g_ed.cur_col > len) g_ed.cur_col = len;
    if (g_ed.cur_col < 0) g_ed.cur_col = 0;
}

static void move_left(void)
{
    if (g_ed.cur_col > 0) g_ed.cur_col--;
    else if (g_ed.cur_row > 0) {
        g_ed.cur_row--;
        g_ed.cur_col = line_len(g_ed.cur_row);
    }
}

static void move_right(void)
{
    int len = line_len(g_ed.cur_row);
    if (g_ed.cur_col < len) g_ed.cur_col++;
    else if (g_ed.cur_row < g_ed.nlines - 1) {
        g_ed.cur_row++;
        g_ed.cur_col = 0;
    }
}

static void move_up(void)
{
    if (g_ed.cur_row > 0) { g_ed.cur_row--; clamp_col(); }
}

static void move_down(void)
{
    if (g_ed.cur_row < g_ed.nlines - 1) { g_ed.cur_row++; clamp_col(); }
}

static void scroll_to_cursor(void)
{
    int vr = vis_rows(), vc = vis_cols();
    if (g_ed.cur_row < g_ed.top) g_ed.top = g_ed.cur_row;
    if (g_ed.cur_row >= g_ed.top + vr) g_ed.top = g_ed.cur_row - vr + 1;
    if (g_ed.cur_col < g_ed.leftcol) g_ed.leftcol = g_ed.cur_col;
    if (g_ed.cur_col >= g_ed.leftcol + vc) g_ed.leftcol = g_ed.cur_col - vc + 1;
    if (g_ed.top < 0) g_ed.top = 0;
    if (g_ed.leftcol < 0) g_ed.leftcol = 0;
}

/* ── Render ───────────────────────────────────────────────────────────── */

static void draw_button(int x, const char *label, int hot)
{
    draw_fill_rect(&g_ed.surf, x, BTN_Y, BTN_W, BTN_H,
                   hot ? THEME_ACCENT : THEME_SURFACE_2);
    int lw = chrome_w(14, label);
    chrome_text(14, x + (BTN_W - lw) / 2, BTN_Y + (BTN_H - 16) / 2,
                label, 0x00FFFFFF);   /* white text on colored button */
}

static void render(void)
{
    if (!g_ed.dirty) return;
    g_ed.dirty = 0;
    surface_t *s = &g_ed.surf;

    /* Background + text area share THEME_SURFACE so the editing area is
     * not a different shade from the window. THEME_SURFACE is deliberately
     * NOT C_TERM_BG: the compositor color-keys external window pixels on
     * exactly C_TERM_BG (frosted glass for the terminal), so a flat
     * C_TERM_BG fill renders as translucent glass ghosting whatever is
     * behind the window. */
    draw_fill_rect(s, 0, 0, g_ed.fb_w, g_ed.fb_h, THEME_SURFACE);
    draw_fill_rect(s, TEXT_X, TEXT_Y, TEXT_W, TEXT_H, THEME_SURFACE);

    /* Title bar: editable path field, or filename + modified marker. */
    draw_fill_rect(s, 0, 0, g_ed.fb_w, TITLE_H, THEME_SURFACE_2);
    draw_fill_rect(s, 0, TITLE_H, g_ed.fb_w, 1, THEME_BORDER);
    if (g_ed.editing_name) {
        int fx = 8, fy = 5, fw = SAVE_X - 16, fh = TITLE_H - 10;
        draw_fill_rect(s, fx, fy, fw, fh, THEME_INPUT_BG);
        draw_rect(s, fx, fy, fw, fh, THEME_ACCENT);
        chrome_text(15, fx + 6, fy + (fh - 16) / 2, g_ed.name_edit, THEME_TEXT);
        int tw = chrome_w(15, g_ed.name_edit);
        draw_fill_rect(s, fx + 6 + tw + 1, fy + 5, 2, fh - 10, THEME_OK);
    } else {
        char title[300];
        const char *base = strrchr(g_ed.path, '/');
        base = base ? base + 1 : g_ed.path;
        snprintf(title, sizeof(title), "%s%s", base, g_ed.modified ? " *" : "");
        chrome_text(16, 12, (TITLE_H - 18) / 2, title, THEME_TEXT);
    }
    draw_button(SAVE_X, "Save", 0);

    /* Text lines (bitmap font, fixed cells). */
    int vr = vis_rows(), vc = vis_cols();
    for (int sr = 0; sr < vr; sr++) {
        int row = g_ed.top + sr;
        if (row >= g_ed.nlines) break;
        const char *ln = g_ed.lines[row];
        int len = line_len(row);
        int y = TEXT_Y + sr * FONT_H;
        for (int sc = 0; sc < vc; sc++) {
            int col = g_ed.leftcol + sc;
            if (col >= len) break;
            draw_char(s, TEXT_X + sc * FONT_W, y, ln[col],
                      THEME_TEXT, THEME_SURFACE);
        }
    }

    /* Cursor caret. */
    if (g_ed.cur_row >= g_ed.top && g_ed.cur_row < g_ed.top + vr &&
        g_ed.cur_col >= g_ed.leftcol && g_ed.cur_col < g_ed.leftcol + vc) {
        int cx = TEXT_X + (g_ed.cur_col - g_ed.leftcol) * FONT_W;
        int cy = TEXT_Y + (g_ed.cur_row - g_ed.top) * FONT_H;
        draw_fill_rect(s, cx, cy, 2, FONT_H, THEME_OK);
    }

    /* Status bar. */
    int sy = WIN_H - STATUS_H;
    draw_fill_rect(s, 0, sy, g_ed.fb_w, STATUS_H, THEME_SURFACE_2);
    draw_fill_rect(s, 0, sy, g_ed.fb_w, 1, THEME_BORDER);
    char pos[48];
    snprintf(pos, sizeof(pos), "Ln %d, Col %d", g_ed.cur_row + 1,
             g_ed.cur_col + 1);
    chrome_text(13, 12, sy + (STATUS_H - 15) / 2, pos, THEME_TEXT_DIM);
    if (g_ed.status[0]) {
        int w = chrome_w(13, g_ed.status);
        chrome_text(13, (g_ed.fb_w - w) / 2, sy + (STATUS_H - 15) / 2,
                    g_ed.status, g_ed.status_color);
    }
    const char *hint = "Ctrl+S save  click name to rename  Esc close";
    int hw = chrome_w(13, hint);
    chrome_text(13, g_ed.fb_w - hw - 12, sy + (STATUS_H - 15) / 2,
                hint, THEME_TEXT_DIM);

    lumen_window_present(g_ed.lwin);
}

/* ── Input ────────────────────────────────────────────────────────────── */

/* Editing the filename field (title bar) consumes keys until Enter/Esc. */
static void handle_name_key(char c)
{
    int len = (int)strlen(g_ed.name_edit);
    if (c == '\x1b') {                 /* Esc — cancel rename */
        g_ed.editing_name = 0;
    } else if (c == '\r' || c == '\n') { /* Enter — accept new path */
        if (g_ed.name_edit[0]) {
            snprintf(g_ed.path, sizeof(g_ed.path), "%s", g_ed.name_edit);
            set_status("renamed — Ctrl+S to save here", THEME_TEXT_DIM);
        }
        g_ed.editing_name = 0;
    } else if (c == '\b' || c == 127) {
        if (len > 0) g_ed.name_edit[len - 1] = '\0';
    } else if ((unsigned char)c >= ' ' && (unsigned char)c < 127 &&
               len < (int)sizeof(g_ed.name_edit) - 1) {
        g_ed.name_edit[len] = c;
        g_ed.name_edit[len + 1] = '\0';
    }
    g_ed.dirty = 1;
}

static void handle_key(char c)
{
    if (g_ed.editing_name) { handle_name_key(c); return; }
    switch (c) {
    case '\x1b': g_ed.done = 1; return;
    case KEY_CTRL_S: save_file(); scroll_to_cursor(); g_ed.dirty = 1; return;
    case KEY_UP:    move_up();    break;
    case KEY_DOWN:  move_down();  break;
    case KEY_LEFT:  move_left();  break;
    case KEY_RIGHT: move_right(); break;
    case '\r': case '\n': insert_newline(); break;
    case '\b': case 127:  delete_back();    break;
    case '\t':
        for (int i = 0; i < TAB_WIDTH; i++) insert_char(' ');
        break;
    default:
        if ((unsigned char)c >= ' ' && (unsigned char)c < 127)
            insert_char(c);
        else
            return;
        break;
    }
    scroll_to_cursor();
    g_ed.dirty = 1;
}

static void handle_click(int x, int y)
{
    if (y < TITLE_H) {
        if (x >= SAVE_X && x < SAVE_X + BTN_W) save_file();
        else {
            /* Click the title text → edit the path (rename / save-as). */
            g_ed.editing_name = 1;
            snprintf(g_ed.name_edit, sizeof(g_ed.name_edit), "%s", g_ed.path);
            set_status("edit path, Enter to set, Esc to cancel", THEME_TEXT_DIM);
        }
        g_ed.dirty = 1;
        return;
    }
    if (x >= TEXT_X && x < TEXT_X + TEXT_W &&
        y >= TEXT_Y && y < TEXT_Y + TEXT_H) {
        int row = g_ed.top + (y - TEXT_Y) / FONT_H;
        int col = g_ed.leftcol + (x - TEXT_X) / FONT_W;
        if (row >= g_ed.nlines) row = g_ed.nlines - 1;
        if (row < 0) row = 0;
        g_ed.cur_row = row;
        g_ed.cur_col = col;
        clamp_col();
        scroll_to_cursor();
        g_ed.dirty = 1;
    }
}

/* ── Main ─────────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    /* Default to $HOME/untitled.txt (Lumen propagates the session's HOME)
     * so a non-root user saves into their own home, not /root. */
    char defpath[300];
    const char *home = getenv("HOME");
    snprintf(defpath, sizeof(defpath), "%s/untitled.txt",
             (home && home[0]) ? home : "/root");
    const char *path = (argc > 1) ? argv[1] : defpath;

    g_ed.lfd = lumen_connect_retry();
    if (g_ed.lfd < 0) {
        dprintf(2, "[EDIT] lumen_connect failed (%d)\n", g_ed.lfd);
        return 1;
    }

    g_ed.lwin = lumen_window_create(g_ed.lfd, "Text Editor", WIN_W, WIN_H);
    if (!g_ed.lwin) {
        dprintf(2, "[EDIT] lumen_window_create failed\n");
        close(g_ed.lfd);
        return 1;
    }
    g_ed.fb_w = g_ed.lwin->w;
    g_ed.fb_h = g_ed.lwin->h;
    g_ed.surf = (surface_t){
        .buf = (uint32_t *)g_ed.lwin->backbuf,
        .w = g_ed.fb_w, .h = g_ed.fb_h, .pitch = g_ed.lwin->stride,
    };

    font_init();

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigterm_handler;
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    load_file(path);
    g_ed.dirty = 1;
    render();

    dprintf(2, "[EDIT] connected %dx%d file=%s\n",
            g_ed.lwin->w, g_ed.lwin->h, g_ed.path);

    while (!s_term && !g_ed.done) {
        lumen_event_t ev;
        int r = lumen_wait_event(g_ed.lfd, &ev, 16);
        if (r < 0) break;
        if (r == 1) {
            if (ev.type == LUMEN_EV_CLOSE_REQUEST) break;
            if (ev.type == LUMEN_EV_KEY && ev.key.pressed)
                handle_key((char)ev.key.keycode);
            if (ev.type == LUMEN_EV_MOUSE &&
                ev.mouse.evtype == LUMEN_MOUSE_DOWN &&
                (ev.mouse.buttons & 1))
                handle_click(ev.mouse.x, ev.mouse.y);
        }
        render();
    }

    lumen_window_destroy(g_ed.lwin);
    close(g_ed.lfd);
    dprintf(2, "[EDIT] exit\n");
    return 0;
}
