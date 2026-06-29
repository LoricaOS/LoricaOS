/* window.c -- Glyph window: chrome rendering, dirty tracking, dispatch */
#include "glyph.h"
#include "font.h"
#include <stdlib.h>
#include <string.h>

/* Rectangle geometry helpers — used by window dirty-tracking and by the
 * compositor's dirty-rect coalescing. (Formerly in the removed widget.c.) */
int
glyph_rect_empty(glyph_rect_t r)
{
    return r.w <= 0 || r.h <= 0;
}

glyph_rect_t
glyph_rect_union(glyph_rect_t a, glyph_rect_t b)
{
    if (glyph_rect_empty(a)) return b;
    if (glyph_rect_empty(b)) return a;
    int x1 = a.x < b.x ? a.x : b.x;
    int y1 = a.y < b.y ? a.y : b.y;
    int x2a = a.x + a.w, x2b = b.x + b.w;
    int y2a = a.y + a.h, y2b = b.y + b.h;
    int x2 = x2a > x2b ? x2a : x2b;
    int y2 = y2a > y2b ? y2a : y2b;
    return (glyph_rect_t){ x1, y1, x2 - x1, y2 - y1 };
}

int
glyph_rect_intersects(glyph_rect_t a, glyph_rect_t b)
{
    return a.x < b.x + b.w && a.x + a.w > b.x &&
           a.y < b.y + b.h && a.y + a.h > b.y;
}

#define TB_H  GLYPH_TITLEBAR_HEIGHT
#define BD_W  GLYPH_BORDER_WIDTH
#define SH_OFF GLYPH_SHADOW_OFFSET

/* Close button position within chrome (relative to surface origin) */
#define CLOSE_BTN_X  8
#define CLOSE_BTN_Y  (BD_W + 7)
#define CLOSE_BTN_SZ 14

/* Chrome colors -- frosted glass aesthetic (all from theme.h) */
#define C_CHROME_SHADOW  THEME_KEY_SHADOW
#define C_CHROME_BORDER  THEME_BORDER_STRONG
#define C_CHROME_TITLE   THEME_SURFACE_2
#define C_CHROME_UTITLE  THEME_SURFACE
#define C_CHROME_TITLE_T THEME_TEXT
#define C_CHROME_CLIENT  THEME_SURFACE       /* was a near-white 0xF4F4F8 */
#define C_CHROME_RED     THEME_TL_RED
#define C_CHROME_ADMIN   0x00B0202A          /* admin-session titlebar (danger red) */
#define C_CHROME_YELLOW  THEME_TL_YELLOW
#define C_CHROME_GREEN   THEME_TL_GREEN
#define C_CHROME_TL_DIM  0x00555E6E          /* unfocused traffic lights */

/* Traffic-light button spacing */
#define BTN_RADIUS  7
#define BTN_SPACING 22
#define CORNER_R    R_MD

/* Client area origin within the surface */
static int
client_ox(void)
{
    return BD_W;
}

static int
client_oy(void)
{
    return BD_W + TB_H;
}

glyph_window_t *
glyph_window_create(const char *title, int client_w, int client_h)
{
    glyph_window_t *win = calloc(1, sizeof(*win));
    if (!win)
        return NULL;

    win->client_w = client_w;
    win->client_h = client_h;
    win->surf_w = client_w + 2 * BD_W + SH_OFF;
    win->surf_h = client_h + TB_H + 2 * BD_W + SH_OFF;

    win->surface.buf = calloc((unsigned)(win->surf_w * win->surf_h), sizeof(uint32_t));
    if (!win->surface.buf) {
        free(win);
        return NULL;
    }
    win->surface.w = win->surf_w;
    win->surface.h = win->surf_h;
    win->surface.pitch = win->surf_w;

    if (title) {
        int len = 0;
        while (title[len] && len < 63) {
            win->title[len] = title[len];
            len++;
        }
        win->title[len] = '\0';
    }

    win->visible = 1;
    win->presented = 1;   /* in-process windows paint immediately */
    win->closeable = 1;
    win->frosted = 1;
    win->has_dirty = 1;
    win->tag = -1;  /* sentinel: callers set to a valid PTY fd if applicable */
    win->dirty_rect.x = 0;
    win->dirty_rect.y = 0;
    win->dirty_rect.w = win->surf_w;
    win->dirty_rect.h = win->surf_h;

    return win;
}

void
glyph_window_destroy(glyph_window_t *win)
{
    if (!win)
        return;
    free(win->blur_cache);   /* frosted-glass backdrop cache (may be NULL) */
    free(win->surface.buf);
    free(win);
}

/* Check if pixel (px,py) is outside a rounded rectangle with corner radius r */
static int
outside_rounded(int px, int py, int w, int h, int r)
{
    int cx, cy;
    /* Top-left */
    if (px < r && py < r) {
        cx = r; cy = r;
        int dx = cx - px, dy = cy - py;
        return dx * dx + dy * dy > r * r;
    }
    /* Top-right */
    if (px >= w - r && py < r) {
        cx = w - r - 1; cy = r;
        int dx = px - cx, dy = cy - py;
        return dx * dx + dy * dy > r * r;
    }
    /* Bottom-left */
    if (px < r && py >= h - r) {
        cx = r; cy = h - r - 1;
        int dx = cx - px, dy = py - cy;
        return dx * dx + dy * dy > r * r;
    }
    /* Bottom-right */
    if (px >= w - r && py >= h - r) {
        cx = w - r - 1; cy = h - r - 1;
        int dx = px - cx, dy = py - cy;
        return dx * dx + dy * dy > r * r;
    }
    return 0;
}

static void
render_chrome(glyph_window_t *win)
{
    surface_t *s = &win->surface;
    int total_w = win->client_w + 2 * BD_W;
    int total_h = win->client_h + TB_H + 2 * BD_W;
    int cx = client_ox();
    (void)client_oy();
    int focused = win->focused_window;

    /* Clear entire surface to key color (C_SHADOW) for frosted transparency */
    draw_fill_rect(s, 0, 0, win->surf_w, win->surf_h, C_CHROME_SHADOW);

    /* Draw the rounded window body.
     * Frosted windows: titlebar uses key color (C_SHADOW) so compositor's
     * blur+tint shows through. Non-frosted: solid titlebar. */
    uint32_t title_bg = win->frosted ? C_CHROME_SHADOW
                        : (focused ? C_CHROME_TITLE : C_CHROME_UTITLE);
    /* Admin-session windows get a danger-red titlebar so an elevated shell
     * is unmistakable. Overrides even the frosted key color: a solid opaque
     * red reads clearly as "danger" and the compositor blits it as-is (it
     * only frosts the key-colored regions). */
    if (win->admin)
        title_bg = C_CHROME_ADMIN;
    uint32_t client_bg = win->frosted ? C_CHROME_SHADOW
                         : C_CHROME_CLIENT;
    if (win->priv)
        client_bg = C_TERM_BG;

    if (title_bg == client_bg) {
        /* Fast path: single color body — use Bresenham rounded rect */
        draw_rounded_rect(s, 0, 0, total_w, total_h, CORNER_R, title_bg);
    } else {
        /* Slow path (non-frosted only): two colors need per-pixel corner test */
        for (int py = 0; py < TB_H + BD_W; py++)
            for (int px = 0; px < total_w; px++)
                if (!outside_rounded(px, py, total_w, total_h, CORNER_R))
                    draw_px(s, px, py, title_bg);
        for (int py = TB_H + BD_W; py < total_h; py++)
            for (int px = 0; px < total_w; px++)
                if (!outside_rounded(px, py, total_w, total_h, CORNER_R))
                    draw_px(s, px, py, client_bg);
    }

    /* Subtle border outline along the rounded rect (skip for frosted —
     * the frost tint provides enough definition) */
    if (!win->frosted) {
        for (int py = 0; py < total_h; py++) {
            for (int px = 0; px < total_w; px++) {
                if (outside_rounded(px, py, total_w, total_h, CORNER_R))
                    continue;
                if (px == 0 || py == 0 || px == total_w - 1 || py == total_h - 1 ||
                    outside_rounded(px - 1, py, total_w, total_h, CORNER_R) ||
                    outside_rounded(px + 1, py, total_w, total_h, CORNER_R) ||
                    outside_rounded(px, py - 1, total_w, total_h, CORNER_R) ||
                    outside_rounded(px, py + 1, total_w, total_h, CORNER_R))
                    draw_px(s, px, py, C_CHROME_BORDER);
            }
        }
    }

    /* Divider line between titlebar and client */
    for (int px = 1; px < total_w - 1; px++)
        draw_px(s, px, TB_H + BD_W - 1, C_CHROME_BORDER);

    /* Title text — centered */
    if (g_font_ui) {
        int tw = font_text_width(g_font_ui, 13, win->title);
        int tx = (total_w - tw) / 2;
        int ty = (TB_H + BD_W - font_height(g_font_ui, 13)) / 2;
        font_draw_text(s, g_font_ui, 13, tx, ty, win->title,
                       C_CHROME_TITLE_T);
    } else {
        int len = 0;
        const char *p = win->title;
        while (*p++) len++;
        int tx = (total_w - len * FONT_W) / 2;
        draw_text_t(s, tx, BD_W + 5, win->title, C_CHROME_TITLE_T);
    }

    /* Traffic-light circles */
    int btn_cy = (TB_H + BD_W) / 2;
    int btn_x = cx + CLOSE_BTN_X + BTN_RADIUS;

    /* Focused windows show full-color lights; unfocused dim to grey (macOS). */
    uint32_t tl_r = focused ? C_CHROME_RED    : C_CHROME_TL_DIM;
    uint32_t tl_y = focused ? C_CHROME_YELLOW : C_CHROME_TL_DIM;
    uint32_t tl_g = focused ? C_CHROME_GREEN  : C_CHROME_TL_DIM;
    draw_circle_filled(s, btn_x, btn_cy, BTN_RADIUS, tl_r);
    draw_circle_filled(s, btn_x + BTN_SPACING, btn_cy, BTN_RADIUS, tl_y);
    draw_circle_filled(s, btn_x + BTN_SPACING * 2, btn_cy, BTN_RADIUS, tl_g);

    /* X on close button when focused */
    if (focused)
        draw_text_t(s, btn_x - 3, btn_cy - FONT_H / 2, "x", 0x00401010);
}

void
glyph_window_render(glyph_window_t *win)
{
    if (!win || !win->has_dirty)
        return;

    /* Compositor reads pixels straight from blit_src (a chromeless proxy
     * window's shared buffer) — there is no surface.buf to render into
     * (it was freed), and no chrome/widgets to draw. Skip. */
    if (win->blit_src) {
        win->has_dirty = 0;
        return;
    }

    /* Render chrome (border, titlebar, buttons) */
    render_chrome(win);

    /* Custom content renderer (terminal, info window) — called AFTER chrome
     * so it can draw into the client area without being overwritten. */
    if (win->on_render)
        win->on_render(win);

    win->has_dirty = 0;
    win->dirty_rect.w = 0;
    win->dirty_rect.h = 0;
}

void
glyph_window_dispatch_mouse(glyph_window_t *win, int btn, int x, int y)
{
    if (!win)
        return;

    /* Check close button hit */
    if (win->closeable) {
        int cbx = BD_W + CLOSE_BTN_X;
        int cby = CLOSE_BTN_Y;
        if (x >= cbx && x < cbx + CLOSE_BTN_SZ &&
            y >= cby && y < cby + CLOSE_BTN_SZ) {
            if (win->on_close)
                win->on_close(win);
            return;
        }
    }
    (void)btn;
}

int
glyph_window_get_dirty_rect(glyph_window_t *win, glyph_rect_t *out)
{
    if (!win || !win->has_dirty) {
        if (out)
            *out = (glyph_rect_t){0, 0, 0, 0};
        return 0;
    }
    if (out)
        *out = win->dirty_rect;
    return 1;
}

void
glyph_window_mark_dirty_rect(glyph_window_t *win, glyph_rect_t r)
{
    if (!win)
        return;
    if (win->has_dirty) {
        win->dirty_rect = glyph_rect_union(win->dirty_rect, r);
    } else {
        win->dirty_rect = r;
        win->has_dirty = 1;
    }
}

void
glyph_window_mark_all_dirty(glyph_window_t *win)
{
    if (!win)
        return;
    win->dirty_rect.x = 0;
    win->dirty_rect.y = 0;
    win->dirty_rect.w = win->surf_w;
    win->dirty_rect.h = win->surf_h;
    win->has_dirty = 1;
}
