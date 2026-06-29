/* user/bin/applications/main.c — Aegis Applications launcher (external Lumen client)
 *
 * Launchpad-style fullscreen app grid. Creates a chromeless fullscreen
 * window (LUMEN_WIN_FLAG_FULLSCREEN) and fills it with C_TERM_BG, which
 * the compositor color-keys to frosted glass — the desktop shows through
 * dimmed. Tiles come from glyph_apps_scan() (/apps bundles); clicking or
 * pressing Enter on a tile asks Lumen to spawn it via LUMEN_OP_INVOKE,
 * then the launcher exits. Clicking outside any tile or pressing Esc
 * dismisses (macOS launchpad behavior).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <stdint.h>

#include <glyph.h>
#include <lumen_client.h>
#include "font.h"
#include "apps.h"
#include "icons.h"

/* ── Layout ───────────────────────────────────────────────────────────── */

#define CELL_W      160
#define CELL_H      130
#define ICON_SIZE   72
#define ICON_TOP    14            /* icon y offset inside the cell */
#define LABEL_GAP   10            /* gap between icon bottom and label top */
#define SIDE_MARGIN 80            /* minimum left/right margin for the grid */

#define HOVER_ALPHA 28
#define SEL_ALPHA   60

/* Synthetic arrow codes (Lumen folds CSI arrows to these for proxy windows). */
#define KEY_ESC          0x1B
#define KEY_ARROW_UP     0xF1
#define KEY_ARROW_DOWN   0xF2
#define KEY_ARROW_RIGHT  0xF3
#define KEY_ARROW_LEFT   0xF4

/* ── State ────────────────────────────────────────────────────────────── */

typedef struct {
    int             lfd;
    lumen_window_t *lwin;
    surface_t       surf;
    int             fb_w, fb_h;

    glyph_app_t     apps[GLYPH_APPS_MAX];
    int             count;

    int             cols, rows;
    int             grid_x, grid_y;

    int             hover;        /* tile under the mouse, -1 = none */
    int             sel;          /* keyboard-selected tile */
    int             dirty;
} apps_state_t;

static apps_state_t g_st;
static volatile sig_atomic_t s_term;
static void sigterm_handler(int s) { (void)s; s_term = 1; }

/* ── Layout helpers ───────────────────────────────────────────────────── */

static void layout_grid(void)
{
    int cols = (g_st.fb_w - 2 * SIDE_MARGIN) / CELL_W;
    if (cols < 1) cols = 1;
    if (g_st.count > 0 && cols > g_st.count) cols = g_st.count;
    g_st.cols = cols;
    g_st.rows = g_st.count > 0 ? (g_st.count + cols - 1) / cols : 0;

    g_st.grid_x = (g_st.fb_w - g_st.cols * CELL_W) / 2;
    g_st.grid_y = (g_st.fb_h - g_st.rows * CELL_H) / 2;
    if (g_st.grid_y < 16) g_st.grid_y = 16;
}

/* Tile index at client point (x, y), or -1. */
static int tile_at(int x, int y)
{
    if (g_st.count == 0) return -1;
    if (x < g_st.grid_x || y < g_st.grid_y) return -1;
    int c = (x - g_st.grid_x) / CELL_W;
    int r = (y - g_st.grid_y) / CELL_H;
    if (c >= g_st.cols || r >= g_st.rows) return -1;
    int idx = r * g_st.cols + c;
    return idx < g_st.count ? idx : -1;
}

/* ── Rendering ────────────────────────────────────────────────────────── */

static void render_tile(int idx)
{
    surface_t *s = &g_st.surf;
    int x = g_st.grid_x + (idx % g_st.cols) * CELL_W;
    int y = g_st.grid_y + (idx / g_st.cols) * CELL_H;

    /* Highlight: stronger for the keyboard selection, subtle for hover.
     * Off-key pixels by design — the blend lifts them off C_TERM_BG so
     * they composite as a translucent pane over the frosted backdrop. */
    if (idx == g_st.sel)
        draw_blend_rounded_rect(s, x + 8, y + 4, CELL_W - 16, CELL_H - 8,
                                10, 0x00FFFFFF, SEL_ALPHA);
    else if (idx == g_st.hover)
        draw_blend_rounded_rect(s, x + 8, y + 4, CELL_W - 16, CELL_H - 8,
                                10, 0x00FFFFFF, HOVER_ALPHA);

    glyph_app_t *app = &g_st.apps[idx];
    glyph_icon_draw(s, app->id, app->name,
                    x + (CELL_W - ICON_SIZE) / 2, y + ICON_TOP, ICON_SIZE);

    int tw = glyph_text_width(app->name);
    int tx = x + (CELL_W - tw) / 2;
    if (tx < x + 2) tx = x + 2;            /* long names: clip, don't bleed */
    draw_text_ui(s, tx, y + ICON_TOP + ICON_SIZE + LABEL_GAP,
                 app->name, C_WIN);
}

static void render(void)
{
    if (!g_st.dirty) return;
    g_st.dirty = 0;
    surface_t *s = &g_st.surf;

    /* Exactly C_TERM_BG everywhere: the compositor's color key, rendered
     * as frosted glass over the desktop. */
    draw_fill_rect(s, 0, 0, g_st.fb_w, g_st.fb_h, C_TERM_BG);

    if (g_st.count == 0) {
        const char *msg = "No applications installed";
        int tw = glyph_text_width(msg);
        draw_text_ui(s, (g_st.fb_w - tw) / 2,
                     (g_st.fb_h - glyph_text_height()) / 2, msg, C_SUBTLE);
    } else {
        for (int i = 0; i < g_st.count; i++)
            render_tile(i);
    }

    lumen_window_present(g_st.lwin);
}

/* ── Actions ──────────────────────────────────────────────────────────── */

static int launch(int idx)
{
    if (idx < 0 || idx >= g_st.count) return 0;
    lumen_invoke(g_st.lfd, g_st.apps[idx].id);
    dprintf(2, "[APPS] launch %s\n", g_st.apps[idx].id);
    return 1;
}

static void move_sel(int dx, int dy)
{
    if (g_st.count == 0) return;
    int next = g_st.sel + dx + dy * g_st.cols;
    if (next < 0 || next >= g_st.count) return;
    if (next != g_st.sel) {
        g_st.sel = next;
        g_st.dirty = 1;
    }
}

/* Returns 1 to keep running, 0 to dismiss, 2 when an app was launched. */
static int handle_key(uint8_t k)
{
    switch (k) {
    case KEY_ESC:
        return 0;
    case '\r':
    case '\n':
        return launch(g_st.sel) ? 2 : 1;
    case KEY_ARROW_UP:    move_sel(0, -1); return 1;
    case KEY_ARROW_DOWN:  move_sel(0, 1);  return 1;
    case KEY_ARROW_RIGHT: move_sel(1, 0);  return 1;
    case KEY_ARROW_LEFT:  move_sel(-1, 0); return 1;
    default:
        return 1;
    }
}

static int handle_mouse(lumen_event_t *ev)
{
    if (ev->mouse.evtype == LUMEN_MOUSE_MOVE) {
        int h = tile_at(ev->mouse.x, ev->mouse.y);
        if (h != g_st.hover) {
            g_st.hover = h;
            g_st.dirty = 1;
        }
        return 1;
    }
    if (ev->mouse.evtype == LUMEN_MOUSE_DOWN && (ev->mouse.buttons & 1)) {
        int idx = tile_at(ev->mouse.x, ev->mouse.y);
        if (idx < 0) return 0;             /* click-outside dismisses */
        return launch(idx) ? 2 : 1;
    }
    return 1;
}

/* ── Main ─────────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    g_st.lfd = lumen_connect_retry();
    if (g_st.lfd < 0) {
        dprintf(2, "[APPS] lumen_connect failed (%d)\n", g_st.lfd);
        return 1;
    }

    g_st.lwin = lumen_window_create_ex(g_st.lfd, "Applications", 0, 0,
                                       LUMEN_WIN_FLAG_FULLSCREEN);
    if (!g_st.lwin) {
        dprintf(2, "[APPS] lumen_window_create failed\n");
        close(g_st.lfd);
        return 1;
    }
    g_st.fb_w = g_st.lwin->w;
    g_st.fb_h = g_st.lwin->h;
    g_st.surf = (surface_t){
        .buf = (uint32_t *)g_st.lwin->backbuf,
        .w = g_st.fb_w, .h = g_st.fb_h, .pitch = g_st.lwin->stride,
    };

    g_st.count = glyph_apps_scan(g_st.apps, GLYPH_APPS_MAX);
    if (g_st.count < 0) g_st.count = 0;
    layout_grid();

    dprintf(2, "[APPS] connected %dx%d apps=%d\n",
            g_st.fb_w, g_st.fb_h, g_st.count);

    font_init();

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigterm_handler;
    sigaction(SIGTERM, &sa, NULL);

    g_st.hover = -1;
    g_st.sel = 0;
    g_st.dirty = 1;
    render();

    while (!s_term) {
        lumen_event_t ev;
        int r = lumen_wait_event(g_st.lfd, &ev, 500);
        if (r < 0) break;                  /* compositor went away */

        if (r == 1) {
            int act = 1;
            if (ev.type == LUMEN_EV_CLOSE_REQUEST)
                act = 0;
            else if (ev.type == LUMEN_EV_KEY && ev.key.pressed)
                act = handle_key((uint8_t)ev.key.keycode);
            else if (ev.type == LUMEN_EV_MOUSE)
                act = handle_mouse(&ev);
            if (act != 1)
                break;                     /* 0 = dismiss, 2 = launched */
        }
        render();
    }

    lumen_window_destroy(g_st.lwin);
    close(g_st.lfd);
    dprintf(2, "[APPS] exit\n");
    return 0;
}
