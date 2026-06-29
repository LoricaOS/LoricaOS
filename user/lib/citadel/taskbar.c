/* taskbar.c -- Top bar and context menu for Lumen compositor
 *
 * This file replaces the old bottom taskbar with a macOS-style top bar.
 * The "Aegis" text on the left side is clickable and opens a context menu.
 * The right side carries a volume slider and the clock.
 */
#include <glyph.h>   /* pulls glyph/theme.h tokens — must precede citadel theme.h */
#include "theme.h"
#include "taskbar.h"
#include <font.h>
#include <stdlib.h>
#include <string.h>

/* Volume slider geometry (right side, just left of the clock). */
#define VOL_TRACK_W 56
#define VOL_GAP     18      /* gap between the track and the clock */
#define VOL_ICON_W  16      /* speaker icon + its gap, left of the track */

static int
clock_width(const char *clock_str)
{
    const char *c = (clock_str && clock_str[0]) ? clock_str : "00:00";
    if (g_font_ui) return font_text_width(g_font_ui, 14, c);
    return (int)strlen(c) * FONT_W;
}

/* Left x of the volume slider track. */
static int
vol_track_x(int screen_w, const char *clock_str)
{
    int clock_x = screen_w - clock_width(clock_str) - 12;
    return clock_x - VOL_GAP - VOL_TRACK_W;
}

static void
draw_clock(surface_t *s, int screen_w, const char *clock_str)
{
    const char *c = (clock_str && clock_str[0]) ? clock_str : "00:00";
    if (g_font_ui) {
        int cw = font_text_width(g_font_ui, 14, c);
        int cx = screen_w - cw - 12;
        int ty = (TOPBAR_HEIGHT - font_height(g_font_ui, 14)) / 2;
        font_draw_text(s, g_font_ui, 14, cx, ty, c, TOPBAR_TEXT);
    } else {
        draw_text_t(s, screen_w - (int)strlen(c) * FONT_W - 12, 4, c, TOPBAR_TEXT);
    }
}

void
topbar_draw(surface_t *s, int screen_w, const char *clock_str, int volume,
            int backdrop_dirty)
{
    /* Frosted glass top bar: blur the top TOPBAR_HEIGHT pixels of the
     * backbuffer, apply a dark tint at 50% alpha, then draw text with
     * transparent background so the glass shows through.
     *
     * topbar_draw runs (via on_draw_desktop) BEFORE any window is composited,
     * so the blur input is ALWAYS the desktop background — windows are drawn
     * over the bar afterward. The desktop background only changes on a full
     * redraw (wallpaper/preset/night-light/resolution, all of which set
     * comp.full_redraw → backdrop_dirty here). So blur once and cache the
     * blurred strip; on every other frame stamp the cache instead of
     * recomputing the box blur (the tint + clock + volume are still drawn fresh
     * on top). Process-lifetime static (one top bar; reclaimed at exit, like
     * draw_box_blur's own scratch). */
    static uint32_t *bg_cache = NULL;
    static int cache_w = 0;
    static int cache_wp = -1;
    int wp = glyph_theme_wallpaper();
    int rebuild = backdrop_dirty || !bg_cache || cache_w != screen_w ||
                  cache_wp != wp;

    if (rebuild) {
        draw_box_blur(s, 0, 0, screen_w, TOPBAR_HEIGHT, 8);
        if (cache_w != screen_w) {
            free(bg_cache);
            bg_cache = malloc((size_t)screen_w * TOPBAR_HEIGHT * sizeof(uint32_t));
            cache_w = bg_cache ? screen_w : 0;
        }
        if (bg_cache) {
            for (int y = 0; y < TOPBAR_HEIGHT; y++)
                memcpy(&bg_cache[y * screen_w], &s->buf[y * s->pitch],
                       (size_t)screen_w * sizeof(uint32_t));
            cache_wp = wp;
        }
    } else {
        /* Reuse: stamp the cached blurred desktop strip back into the bar. */
        for (int y = 0; y < TOPBAR_HEIGHT; y++)
            memcpy(&s->buf[y * s->pitch], &bg_cache[y * screen_w],
                   (size_t)screen_w * sizeof(uint32_t));
    }

    draw_blend_rect(s, 0, 0, screen_w, TOPBAR_HEIGHT, TOPBAR_BG, 128);

    /* Subtle bottom border for definition */
    draw_blend_rect(s, 0, TOPBAR_HEIGHT - 1, screen_w, 1, 0x00FFFFFF, 20);

    /* Accent dot before "Aegis" text */
    draw_circle_filled(s, 14, TOPBAR_HEIGHT / 2, 4, C_ACCENT);

    /* "Aegis" text — transparent background */
    if (g_font_ui) {
        int ty = (TOPBAR_HEIGHT - font_height(g_font_ui, 14)) / 2;
        font_draw_text(s, g_font_ui, 14, 24, ty, "Aegis", 0x00FFFFFF);
    } else {
        draw_text_t(s, 24, 4, "Aegis", 0x00FFFFFF);
    }

    draw_clock(s, screen_w, clock_str);

    /* Volume widget: a small speaker icon + slider, left of the clock. */
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    int vx = vol_track_x(screen_w, clock_str);
    int vy = TOPBAR_HEIGHT / 2;

    /* Speaker: a small body rectangle + a cone widening to the right. */
    int ix = vx - VOL_ICON_W + 2;
    draw_fill_rect(s, ix, vy - 2, 3, 5, TOPBAR_TEXT);
    for (int dx = 0; dx <= 4; dx++)
        draw_fill_rect(s, ix + 3 + dx, vy - dx, 1, 2 * dx + 1, TOPBAR_TEXT);

    /* Track (faint), filled portion (accent), knob. */
    draw_blend_rect(s, vx, vy - 1, VOL_TRACK_W, 3, 0x00FFFFFF, 55);
    int fillw = VOL_TRACK_W * volume / 100;
    if (fillw > 0)
        draw_fill_rect(s, vx, vy - 1, fillw, 3, C_ACCENT);
    draw_circle_filled(s, vx + fillw, vy, 5, 0x00FFFFFF);
}

int
topbar_hit_aegis(int mx, int my, int screen_w)
{
    (void)screen_w;
    return mx >= 0 && mx < AEGIS_AREA_W &&
           my >= 0 && my < TOPBAR_HEIGHT;
}

int
topbar_volume_at(int mx, int my, int screen_w, const char *clock_str)
{
    if (my < 0 || my >= TOPBAR_HEIGHT) return -1;
    int vx = vol_track_x(screen_w, clock_str);
    if (mx < vx - 4 || mx > vx + VOL_TRACK_W + 4) return -1;   /* a touch wider */
    return topbar_volume_from_x(mx, screen_w, clock_str);
}

int
topbar_volume_from_x(int mx, int screen_w, const char *clock_str)
{
    int vx = vol_track_x(screen_w, clock_str);
    int v = (mx - vx) * 100 / VOL_TRACK_W;
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    return v;
}
