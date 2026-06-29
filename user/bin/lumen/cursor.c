/* cursor.c — Anti-aliased arrow cursor with drop shadow and save-under.
 *
 * The cursor is rendered from a pre-built RGBA sprite. Before drawing,
 * the framebuffer pixels under the cursor are saved to s_save[].
 * cursor_hide() restores them. The compositor must call cursor_hide()
 * before any framebuffer write and cursor_show() after. */
#include "cursor.h"
#include <string.h>

/* 16x20 arrow cursor — ARGB pixels (A=0 transparent, A=FF opaque).
 * Generated procedurally: white fill, black 1px outline, 2px shadow. */

#define CW 16
#define CH 20

/* Pre-rendered ARGB cursor sprite */
static uint32_t s_sprite[CW * CH];
static int s_sprite_ready;

static void
build_sprite(void)
{
    memset(s_sprite, 0, sizeof(s_sprite));

    /* Arrow shape: each row defines how many pixels wide the arrow is.
     * Row 0 = tip (1px), widening to row 11 (12px), then narrowing
     * for the tail section. */
    static const int widths[CH] = {
        1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 7, 5, 6, 5, 4, 3, 2, 1
    };

    /* Pass 1: draw shadow (offset 1,1, dark translucent) */
    for (int r = 0; r < CH; r++) {
        int w = widths[r];
        for (int c = 0; c < w && c + 1 < CW; c++) {
            int idx = (r + 1 < CH ? r + 1 : r) * CW + (c + 1);
            if (idx < CW * CH && s_sprite[idx] == 0)
                s_sprite[idx] = 0x40000000; /* shadow: alpha=0x40 */
        }
    }

    /* Pass 2: draw outline (black, fully opaque) */
    for (int r = 0; r < CH; r++) {
        int w = widths[r];
        for (int c = 0; c < w; c++) {
            /* Outline = first pixel, last pixel of each row, and
             * first row and last row of each column */
            int is_border = (c == 0 || c == w - 1 || r == 0 ||
                             (r < CH - 1 && c >= widths[r + 1]) ||
                             (r > 0 && c >= widths[r - 1]));
            if (is_border)
                s_sprite[r * CW + c] = 0xFF000000;
        }
    }

    /* Pass 3: fill interior (white, fully opaque) */
    for (int r = 1; r < CH; r++) {
        int w = widths[r];
        for (int c = 1; c < w - 1; c++) {
            if (s_sprite[r * CW + c] == 0 ||
                (s_sprite[r * CW + c] & 0xFF000000) != 0xFF000000)
                s_sprite[r * CW + c] = 0xFFFFFFFF;
        }
    }

    s_sprite_ready = 1;
}

static surface_t *s_fb;
static uint32_t s_save[CW * CH];
static int s_x, s_y;
static int s_visible;

void cursor_init(surface_t *fb)
{
    s_fb = fb;
    s_visible = 0;
    if (!s_sprite_ready)
        build_sprite();
}

void cursor_hide(void)
{
    if (!s_visible || !s_fb)
        return;

    /* Clamp restore region to screen bounds */
    int x0 = s_x < 0 ? 0 : s_x;
    int y0 = s_y < 0 ? 0 : s_y;
    int x1 = s_x + CW > s_fb->w ? s_fb->w : s_x + CW;
    int y1 = s_y + CH > s_fb->h ? s_fb->h : s_y + CH;

    for (int py = y0; py < y1; py++) {
        int sr = py - s_y;
        uint32_t *fb_row = &s_fb->buf[py * s_fb->pitch];
        uint32_t *sv_row = &s_save[sr * CW];
        int sc0 = x0 - s_x;
        memcpy(&fb_row[x0], &sv_row[sc0], (unsigned)(x1 - x0) * sizeof(uint32_t));
    }
    s_visible = 0;
}

void cursor_show(int x, int y)
{
    if (!s_fb)
        return;
    s_x = x;
    s_y = y;

    /* Clamp to screen bounds */
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + CW > s_fb->w ? s_fb->w : x + CW;
    int y1 = y + CH > s_fb->h ? s_fb->h : y + CH;

    /* Save pixels under cursor */
    for (int py = y0; py < y1; py++) {
        int sr = py - y;
        uint32_t *fb_row = &s_fb->buf[py * s_fb->pitch];
        uint32_t *sv_row = &s_save[sr * CW];
        int sc0 = x0 - x;
        memcpy(&sv_row[sc0], &fb_row[x0], (unsigned)(x1 - x0) * sizeof(uint32_t));
    }

    /* Draw cursor sprite with alpha blending */
    for (int py = y0; py < y1; py++) {
        int sr = py - y;
        uint32_t *fb_row = &s_fb->buf[py * s_fb->pitch];
        uint32_t *sp_row = &s_sprite[sr * CW];
        for (int px = x0; px < x1; px++) {
            int sc = px - x;
            uint32_t sp = sp_row[sc];
            uint32_t a = (sp >> 24) & 0xFF;
            if (a == 0) continue;
            if (a == 0xFF) {
                fb_row[px] = sp & 0x00FFFFFF;
            } else {
                uint32_t bg = fb_row[px];
                uint32_t inv = 255 - a;
                uint32_t r = (((sp >> 16) & 0xFF) * a + ((bg >> 16) & 0xFF) * inv) / 255;
                uint32_t g = (((sp >> 8) & 0xFF) * a + ((bg >> 8) & 0xFF) * inv) / 255;
                uint32_t b = ((sp & 0xFF) * a + (bg & 0xFF) * inv) / 255;
                fb_row[px] = (r << 16) | (g << 8) | b;
            }
        }
    }
    s_visible = 1;
}

void cursor_move(int x, int y)
{
    if (!s_fb)
        return;
    cursor_hide();
    cursor_show(x, y);
}
