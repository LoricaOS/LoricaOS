/* terminal.c -- Lumen dropdown terminal (thin wrapper over glyph_term)
 *
 * Phase 47b subsystem peeling, terminal step: regular terminal windows
 * are now the standalone /bin/terminal external-protocol client
 * (user/bin/terminal). Only the quake-style dropdown terminal remains
 * in-process; it wraps the shared emulator core in
 * user/lib/glyph/glyph_term.{c,h}.
 */

#include "terminal.h"
#include "compositor.h"
#include <glyph.h>
#include <glyph_term.h>
#include <font.h>
#include <stdlib.h>
#include <unistd.h>

/* The single in-process terminal window (the dropdown). The selection /
 * write helpers below are called from main.c with comp.focused, which
 * may be a PROXY window whose ->priv is a proxy_window_t — NOT a
 * glyph_term_t. Guard every entry point against that. */
static glyph_window_t *s_dropdown_win;

static glyph_term_t *
term_of(glyph_window_t *win)
{
    if (!win || win != s_dropdown_win)
        return NULL;
    return win->priv;
}

/* ---- Window callbacks ---- */

static void term_on_key(glyph_window_t *self, char key)
{
    glyph_term_t *tp = term_of(self);
    if (tp && tp->master_fd >= 0)
        write(tp->master_fd, &key, 1);
}

static void
term_mouse_down(glyph_window_t *win, int x, int y)
{
    glyph_term_t *tp = term_of(win);
    if (tp && glyph_term_mouse_down(tp, x, y))
        glyph_window_mark_all_dirty(win);
}

static void
term_mouse_move(glyph_window_t *win, int x, int y)
{
    glyph_term_t *tp = term_of(win);
    if (tp && glyph_term_mouse_move(tp, x, y))
        glyph_window_mark_all_dirty(win);
}

static void
term_mouse_up(glyph_window_t *win, int x, int y)
{
    glyph_term_t *tp = term_of(win);
    if (tp && glyph_term_mouse_up(tp, x, y))
        glyph_window_mark_all_dirty(win);
}

static void
term_scroll_cb(glyph_window_t *win, int direction)
{
    glyph_term_t *tp = term_of(win);
    if (tp && glyph_term_scroll(tp, direction))
        glyph_window_mark_all_dirty(win);
}

/* ---- Public API (called from main.c) ---- */

void terminal_write(glyph_window_t *win, const char *data, int len)
{
    glyph_term_t *tp = term_of(win);
    if (!tp)
        return;
    glyph_term_feed(tp, data, len);
    /* Mirror the shell's admin-session state onto the window flag (same hook
     * the standalone /apps/terminal uses). TODO: the dropdown is chromeless
     * (no titlebar — rounded bottom corners only), so render_chrome's red
     * tint isn't drawn here yet; dropdown_render_content would need its own
     * red admin bar to make the indicator visible. The flag is the hook. */
    int admin_now;
    if (glyph_term_admin_take_change(tp, &admin_now))
        win->admin = admin_now;
    glyph_window_mark_all_dirty(win);
}

int
terminal_has_selection(glyph_window_t *win)
{
    glyph_term_t *tp = term_of(win);
    return tp ? glyph_term_has_selection(tp) : 0;
}

int
terminal_copy_selection(glyph_window_t *win, char *buf, int max)
{
    glyph_term_t *tp = term_of(win);
    return tp ? glyph_term_copy_selection(tp, buf, max) : 0;
}

void
terminal_clear_selection(glyph_window_t *win)
{
    glyph_term_t *tp = term_of(win);
    if (!tp)
        return;
    glyph_term_clear_selection(tp);
    glyph_window_mark_all_dirty(win);
}

/* ---- Dropdown terminal (chromeless, rounded bottom corners) ---- */

#define DROPDOWN_CORNER_R 12

static void
dropdown_render_content(glyph_window_t *win)
{
    glyph_term_render(win->priv, &win->surface,
                      0, 0, win->surf_w, win->surf_h);

    /* Round the bottom corners by painting the background color
     * over pixels that fall outside the corner arcs. */
    surface_t *s = &win->surface;
    int r = DROPDOWN_CORNER_R;
    int sw = win->surf_w;
    int sh = win->surf_h;
    for (int y = sh - r; y < sh; y++) {
        int dy = y - (sh - r - 1);
        /* Left corner: fill [0, cutoff) with bg */
        int cutoff_l = r;
        for (int px = 0; px < r; px++) {
            int dx = r - px;
            if (dx * dx + dy * dy <= r * r) { cutoff_l = px; break; }
        }
        if (cutoff_l > 0)
            draw_fill_rect(s, 0, y, cutoff_l, 1, C_BG1);
        /* Right corner: fill [sw - cutoff, sw) with bg */
        int cutoff_r = r;
        for (int px = sw - 1; px >= sw - r; px--) {
            int dx = px - (sw - r - 1);
            if (dx * dx + dy * dy <= r * r) { cutoff_r = sw - 1 - px; break; }
        }
        if (cutoff_r > 0)
            draw_fill_rect(s, sw - cutoff_r, y, cutoff_r, 1, C_BG1);
    }
}

glyph_window_t *
terminal_create_dropdown(int screen_w, int screen_h, int *master_fd_out)
{
    int margin = 20;
    int dd_w = screen_w - 2 * margin;
    int dd_h = screen_h * 45 / 100;

    /* Compute terminal grid size from pixel dimensions with padding */
    int pad_x = 4;
    int pad_y = 4;
    int mono_w = g_font_mono ? font_text_width(g_font_mono, 16, "M") : FONT_W;
    int mono_h = g_font_mono ? font_height(g_font_mono, 16) : FONT_H;
    int cols = (dd_w - 2 * pad_x) / mono_w;
    int rows = (dd_h - 2 * pad_y) / mono_h;
    if (cols < 10) cols = 10;
    if (rows < 4) rows = 4;

    /* Allocate the shared emulator core */
    glyph_term_t *tp = glyph_term_create(cols, rows, mono_w, mono_h,
                                         pad_x, pad_y);
    if (!tp) {
        *master_fd_out = -1;
        return NULL;
    }

    /* Create a glyph window */
    glyph_window_t *win = glyph_window_create("Dropdown", dd_w, dd_h);
    if (!win) {
        glyph_term_destroy(tp);
        *master_fd_out = -1;
        return NULL;
    }

    win->on_key = term_on_key;
    win->on_render = dropdown_render_content;
    win->priv = tp;
    win->closeable = 0;
    win->frosted = 1;
    win->chromeless = 1;  /* no titlebar — pure terminal surface */
    win->visible = 0;  /* starts hidden */
    win->on_mouse_down = term_mouse_down;
    win->on_mouse_move = term_mouse_move;
    win->on_mouse_up = term_mouse_up;
    win->on_scroll = term_scroll_cb;

    /* Position: centered horizontally, below the taskbar (28px) */
    win->x = margin;
    win->y = 28;

    /* Register before any terminal_* helper can be called on it */
    s_dropdown_win = win;

    /* Open PTY and spawn shell */
    const char *fail_reason = NULL;
    int mfd = glyph_pty_open_and_spawn(&fail_reason, NULL);
    if (mfd >= 0) {
        tp->master_fd = mfd;
        *master_fd_out = mfd;
        win->tag = mfd;  /* for PTY polling in main loop */
    } else {
        *master_fd_out = -1;
        glyph_term_puts(tp, "Dropdown: PTY unavailable\n");
        if (fail_reason)
            glyph_term_puts(tp, fail_reason);
    }

    dropdown_render_content(win);
    return win;
}
