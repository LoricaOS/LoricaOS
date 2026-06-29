/* draw.c — Framebuffer drawing primitives for Lumen compositor
 *
 * All functions take a surface_t pointer and perform bounds-checked
 * pixel writes. Font rendering uses the Terminus 10x20 bitmap font.
 */
#include "draw.h"
#include "font.h"
#include "terminus20.h"
#include <stdlib.h>
#include <string.h>

/* ---- TTF-aware text measurement helpers ---- */

/* UI font size used by Glyph widgets */
#define UI_FONT_SIZE 14

int glyph_text_width(const char *text)
{
    if (g_font_ui)
        return font_text_width(g_font_ui, UI_FONT_SIZE, text);
    int len = 0;
    while (text[len]) len++;
    return len * FONT_W;
}

int glyph_text_height(void)
{
    if (g_font_ui)
        return font_height(g_font_ui, UI_FONT_SIZE);
    return FONT_H;
}

int glyph_char_width(void)
{
    if (g_font_ui)
        return font_text_width(g_font_ui, UI_FONT_SIZE, "M");
    return FONT_W;
}

/* Draw text using TTF if available, bitmap fallback. Transparent bg. */
void
draw_text_ui(surface_t *s, int x, int y, const char *str, uint32_t fg)
{
    if (g_font_ui)
        font_draw_text(s, g_font_ui, UI_FONT_SIZE, x, y, str, fg);
    else
        draw_text_t(s, x, y, str, fg);
}

void draw_px(surface_t *s, int x, int y, uint32_t c)
{
    if (x >= 0 && x < s->w && y >= 0 && y < s->h)
        s->buf[y * s->pitch + x] = c;
}

void draw_fill_rect(surface_t *s, int x, int y, int w, int h, uint32_t c)
{
    /* Clamp to surface bounds once — no per-pixel checks needed */
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > s->w) w = s->w - x;
    if (y + h > s->h) h = s->h - y;
    if (w <= 0 || h <= 0) return;

    for (int j = y; j < y + h; j++) {
        uint32_t *row = &s->buf[j * s->pitch + x];
        for (int i = 0; i < w; i++)
            row[i] = c;
    }
}

void draw_rect(surface_t *s, int x, int y, int w, int h, uint32_t c)
{
    /* Top and bottom edges */
    if (y >= 0 && y < s->h) {
        int x0 = x < 0 ? 0 : x, x1 = x + w > s->w ? s->w : x + w;
        for (int i = x0; i < x1; i++) s->buf[y * s->pitch + i] = c;
    }
    if (y + h - 1 >= 0 && y + h - 1 < s->h) {
        int x0 = x < 0 ? 0 : x, x1 = x + w > s->w ? s->w : x + w;
        for (int i = x0; i < x1; i++) s->buf[(y + h - 1) * s->pitch + i] = c;
    }
    /* Left and right edges */
    int y0 = y < 0 ? 0 : y, y1 = y + h > s->h ? s->h : y + h;
    if (x >= 0 && x < s->w)
        for (int i = y0; i < y1; i++) s->buf[i * s->pitch + x] = c;
    if (x + w - 1 >= 0 && x + w - 1 < s->w)
        for (int i = y0; i < y1; i++) s->buf[i * s->pitch + x + w - 1] = c;
}

void draw_gradient_v(surface_t *s, int x, int y, int w, int h,
                     uint32_t c1, uint32_t c2)
{
    int i, j;
    for (j = 0; j < h; j++) {
        int t = j * 255 / (h > 1 ? h - 1 : 1);
        uint32_t r = ((c1 >> 16 & 0xFF) * (255 - t) + (c2 >> 16 & 0xFF) * t) / 255;
        uint32_t g = ((c1 >> 8 & 0xFF) * (255 - t) + (c2 >> 8 & 0xFF) * t) / 255;
        uint32_t b = ((c1 & 0xFF) * (255 - t) + (c2 & 0xFF) * t) / 255;
        uint32_t c = (r << 16) | (g << 8) | b;
        for (i = 0; i < w; i++)
            draw_px(s, x + i, y + j, c);
    }
}

void draw_char(surface_t *s, int x, int y, char ch, uint32_t fg, uint32_t bg)
{
    const uint8_t *glyph = &font_terminus[(unsigned char)ch * FONT_H * 2];
    int row, col;
    for (row = 0; row < FONT_H; row++) {
        uint16_t bits = ((uint16_t)glyph[row * 2] << 8) | glyph[row * 2 + 1];
        for (col = 0; col < FONT_W; col++) {
            uint32_t c = (bits & (0x8000 >> col)) ? fg : bg;
            draw_px(s, x + col, y + row, c);
        }
    }
}

void draw_text(surface_t *s, int x, int y, const char *str,
               uint32_t fg, uint32_t bg)
{
    while (*str) {
        draw_char(s, x, y, *str, fg, bg);
        x += FONT_W;
        str++;
    }
}

void draw_text_t(surface_t *s, int x, int y, const char *str, uint32_t fg)
{
    while (*str) {
        const uint8_t *glyph = &font_terminus[(unsigned char)*str * FONT_H * 2];
        int row, col;
        for (row = 0; row < FONT_H; row++) {
            uint16_t bits = ((uint16_t)glyph[row * 2] << 8) | glyph[row * 2 + 1];
            for (col = 0; col < FONT_W; col++)
                if (bits & (0x8000 >> col))
                    draw_px(s, x + col, y + row, fg);
        }
        x += FONT_W;
        str++;
    }
}

void draw_blit(surface_t *dst, int dx, int dy,
               const uint32_t *src, int sw, int sh)
{
    /* Clamp source region to destination bounds.
     * src_stride is the original image width (row pitch in pixels). */
    int src_stride = sw;
    int sx0 = 0, sy0 = 0;
    if (dx < 0) { sx0 = -dx; sw += dx; dx = 0; }
    if (dy < 0) { sy0 = -dy; sh += dy; dy = 0; }
    if (dx + sw > dst->w) sw = dst->w - dx;
    if (dy + sh > dst->h) sh = dst->h - dy;
    if (sw <= 0 || sh <= 0) return;

    for (int y = 0; y < sh; y++)
        __builtin_memcpy(&dst->buf[(dy + y) * dst->pitch + dx],
                         &src[(sy0 + y) * src_stride + sx0],
                         (unsigned long)sw * 4);
}

void draw_line(surface_t *s, int x0, int y0, int x1, int y1, uint32_t color)
{
    int dx = x1 - x0;
    int dy = y1 - y0;
    int sx = dx >= 0 ? 1 : -1;
    int sy = dy >= 0 ? 1 : -1;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    int err = dx - dy;

    for (;;) {
        draw_px(s, x0, y0, color);
        if (x0 == x1 && y0 == y1)
            break;
        int e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void draw_circle(surface_t *s, int cx, int cy, int r, uint32_t color)
{
    int x = r, y = 0;
    int d = 1 - r;

    while (x >= y) {
        draw_px(s, cx + x, cy + y, color);
        draw_px(s, cx - x, cy + y, color);
        draw_px(s, cx + x, cy - y, color);
        draw_px(s, cx - x, cy - y, color);
        draw_px(s, cx + y, cy + x, color);
        draw_px(s, cx - y, cy + x, color);
        draw_px(s, cx + y, cy - x, color);
        draw_px(s, cx - y, cy - x, color);
        y++;
        if (d <= 0) {
            d += 2 * y + 1;
        } else {
            x--;
            d += 2 * (y - x) + 1;
        }
    }
}

static void draw_hline(surface_t *s, int x0, int x1, int y, uint32_t color)
{
    int i;
    if (y < 0 || y >= s->h)
        return;
    if (x0 > x1) {
        int tmp = x0; x0 = x1; x1 = tmp;
    }
    if (x0 < 0) x0 = 0;
    if (x1 >= s->w) x1 = s->w - 1;
    for (i = x0; i <= x1; i++)
        s->buf[y * s->pitch + i] = color;
}

void draw_circle_filled(surface_t *s, int cx, int cy, int r, uint32_t color)
{
    int x = r, y = 0;
    int d = 1 - r;

    while (x >= y) {
        draw_hline(s, cx - x, cx + x, cy + y, color);
        draw_hline(s, cx - x, cx + x, cy - y, color);
        draw_hline(s, cx - y, cx + y, cy + x, color);
        draw_hline(s, cx - y, cx + y, cy - x, color);
        y++;
        if (d <= 0) {
            d += 2 * y + 1;
        } else {
            x--;
            d += 2 * (y - x) + 1;
        }
    }
}

void draw_rounded_rect(surface_t *s, int x, int y, int w, int h,
                       int r, uint32_t color)
{
    if (r < 0) r = 0;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;

    /* Center rectangle */
    draw_fill_rect(s, x + r, y, w - 2 * r, h, color);
    /* Left strip */
    draw_fill_rect(s, x, y + r, r, h - 2 * r, color);
    /* Right strip */
    draw_fill_rect(s, x + w - r, y + r, r, h - 2 * r, color);

    /* Four corner quarter-circles */
    {
        int cx_tl = x + r, cy_tl = y + r;
        int cx_tr = x + w - r - 1, cy_tr = y + r;
        int cx_bl = x + r, cy_bl = y + h - r - 1;
        int cx_br = x + w - r - 1, cy_br = y + h - r - 1;
        int px = r, py = 0;
        int d = 1 - r;

        while (px >= py) {
            /* Top-left corner */
            draw_hline(s, cx_tl - px, cx_tl, cy_tl - py, color);
            draw_hline(s, cx_tl - py, cx_tl, cy_tl - px, color);
            /* Top-right corner */
            draw_hline(s, cx_tr, cx_tr + px, cy_tr - py, color);
            draw_hline(s, cx_tr, cx_tr + py, cy_tr - px, color);
            /* Bottom-left corner */
            draw_hline(s, cx_bl - px, cx_bl, cy_bl + py, color);
            draw_hline(s, cx_bl - py, cx_bl, cy_bl + px, color);
            /* Bottom-right corner */
            draw_hline(s, cx_br, cx_br + px, cy_br + py, color);
            draw_hline(s, cx_br, cx_br + py, cy_br + px, color);

            py++;
            if (d <= 0) {
                d += 2 * py + 1;
            } else {
                px--;
                d += 2 * (py - px) + 1;
            }
        }
    }
}

/* Alpha-blended horizontal line (for rounded shadow) */
static void blend_hline(surface_t *s, int x0, int x1, int y,
                        uint32_t color, int alpha)
{
    if (y < 0 || y >= s->h) return;
    if (x0 > x1) { int tmp = x0; x0 = x1; x1 = tmp; }
    if (x0 < 0) x0 = 0;
    if (x1 >= s->w) x1 = s->w - 1;
    int inv = 255 - alpha;
    uint32_t cr = (color >> 16) & 0xFF;
    uint32_t cg = (color >> 8) & 0xFF;
    uint32_t cb = color & 0xFF;
    for (int i = x0; i <= x1; i++) {
        uint32_t bg = s->buf[y * s->pitch + i];
        uint32_t br = (bg >> 16) & 0xFF;
        uint32_t bg2 = (bg >> 8) & 0xFF;
        uint32_t bb = bg & 0xFF;
        uint32_t r = (cr * (unsigned)alpha + br * (unsigned)inv) / 255;
        uint32_t g = (cg * (unsigned)alpha + bg2 * (unsigned)inv) / 255;
        uint32_t b = (cb * (unsigned)alpha + bb * (unsigned)inv) / 255;
        s->buf[y * s->pitch + i] = (r << 16) | (g << 8) | b;
    }
}

void draw_blend_rounded_rect(surface_t *s, int x, int y, int w, int h,
                             int r, uint32_t color, int alpha)
{
    if (alpha <= 0 || w <= 0 || h <= 0) return;
    if (alpha > 255) alpha = 255;
    if (r < 0) r = 0;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;

    /* Interior: three non-overlapping strips so no pixel is double-blended.
     * Top strip of corners is [y, y+r), middle is [y+r, y+h-r), bottom is [y+h-r, y+h). */
    /* Center column — full width minus corners, full height */
    draw_blend_rect(s, x + r, y, w - 2 * r, h, color, alpha);
    /* Left strip — only the middle vertical range (corners handled below) */
    draw_blend_rect(s, x, y + r, r, h - 2 * r, color, alpha);
    /* Right strip */
    draw_blend_rect(s, x + w - r, y + r, r, h - 2 * r, color, alpha);

    /* Quarter-circle corners — fill scanlines from the edge of the circle
     * to the edge of the center column (x+r or x+w-r-1), avoiding overlap. */
    int cx_tl = x + r, cy_tl = y + r;
    int cx_tr = x + w - r - 1, cy_tr = y + r;
    int cx_bl = x + r, cy_bl = y + h - r - 1;
    int cx_br = x + w - r - 1, cy_br = y + h - r - 1;
    int bx = r, by = 0, d = 1 - r;

    while (bx >= by) {
        /* Each hline goes from the circle edge to the column boundary.
         * Top-left: rightward to cx_tl-1 (center column starts at cx_tl) */
        blend_hline(s, cx_tl - bx, cx_tl - 1, cy_tl - by, color, alpha);
        blend_hline(s, cx_tl - by, cx_tl - 1, cy_tl - bx, color, alpha);
        /* Top-right: leftward from cx_tr+1 */
        blend_hline(s, cx_tr + 1, cx_tr + bx, cy_tr - by, color, alpha);
        blend_hline(s, cx_tr + 1, cx_tr + by, cy_tr - bx, color, alpha);
        /* Bottom-left */
        blend_hline(s, cx_bl - bx, cx_bl - 1, cy_bl + by, color, alpha);
        blend_hline(s, cx_bl - by, cx_bl - 1, cy_bl + bx, color, alpha);
        /* Bottom-right */
        blend_hline(s, cx_br + 1, cx_br + bx, cy_br + by, color, alpha);
        blend_hline(s, cx_br + 1, cx_br + by, cy_br + bx, color, alpha);
        by++;
        if (d <= 0) d += 2 * by + 1;
        else { bx--; d += 2 * (by - bx) + 1; }
    }
}

void draw_blit_scaled(surface_t *dst, int dx, int dy, int dw, int dh,
                      const uint32_t *src, int sw, int sh)
{
    int y;
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0)
        return;

    /* Fixed-point 16.16 stepping — eliminates per-pixel division in inner loop */
    int x_step = (sw << 16) / dw;

    /* Clamp destination to surface bounds for row-level skip */
    int y0 = 0, y1 = dh;
    if (dy < 0) y0 = -dy;
    if (dy + dh > dst->h) y1 = dst->h - dy;
    int x0 = 0, x1 = dw;
    if (dx < 0) x0 = -dx;
    if (dx + dw > dst->w) x1 = dst->w - dx;

    for (y = y0; y < y1; y++) {
        int src_y = y * sh / dh;
        const uint32_t *src_row = src + src_y * sw;
        uint32_t *dst_row = dst->buf + (dy + y) * dst->pitch + dx;
        int x_acc = x0 * x_step;
        for (int x = x0; x < x1; x++) {
            dst_row[x] = src_row[x_acc >> 16];
            x_acc += x_step;
        }
    }
}

void draw_text_center(surface_t *s, int x, int y, int w, const char *str,
                      uint32_t fg, uint32_t bg)
{
    int len = 0;
    const char *p = str;
    while (*p++) len++;
    int text_w = len * FONT_W;
    int offset = (w - text_w) / 2;
    if (offset < 0) offset = 0;
    draw_text(s, x + offset, y, str, fg, bg);
}

void draw_box_blur(surface_t *s, int x, int y, int w, int h, int radius)
{
    /* Clamp region to surface bounds */
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > s->w) w = s->w - x;
    if (y + h > s->h) h = s->h - y;
    if (w <= 0 || h <= 0 || radius <= 0)
        return;

    int span = 2 * radius + 1;
    /* Fixed-point reciprocal: avg = (sum * recip) >> 16 instead of sum/span.
     * span is a runtime value, so the compiler cannot strength-reduce the
     * division — this removes a real divide from the inner loop. Rounded up so
     * the +-1 quantization never darkens; imperceptible in a blur. */
    uint32_t recip = (65536u + span - 1) / span;

    /* Persistent scratch buffer holding a row/column of ORIGINAL packed pixels,
     * so all three channels are blurred in a single pass per direction (was 3).
     * Reused across calls to avoid malloc/free per blur (topbar + dock + every
     * frosted window). */
    static uint32_t *tmp = NULL;
    static int tmp_cap = 0;
    int scratch_len = w > h ? w : h;
    if (scratch_len > tmp_cap) {
        free(tmp);
        tmp = (uint32_t *)malloc((size_t)scratch_len * sizeof(uint32_t));
        if (!tmp) { tmp_cap = 0; return; }
        tmp_cap = scratch_len;
    }

    /* Horizontal pass — blur each row (R, G, B together) */
    for (int row = y; row < y + h; row++) {
        uint32_t *rowp = &s->buf[row * s->pitch];
        for (int i = 0; i < w; i++)
            tmp[i] = rowp[x + i];

        /* Running per-channel sums with clamped edges */
        int sr = 0, sg = 0, sb = 0;
        uint32_t first = tmp[0], last = tmp[w - 1];
        for (int i = -radius; i <= radius; i++) {
            uint32_t p = i < 0 ? first : (i >= w ? last : tmp[i]);
            sr += (p >> 16) & 0xFF; sg += (p >> 8) & 0xFF; sb += p & 0xFF;
        }

        for (int i = 0; i < w; i++) {
            uint32_t ar = (sr * recip) >> 16;
            uint32_t ag = (sg * recip) >> 16;
            uint32_t ab = (sb * recip) >> 16;
            rowp[x + i] = (tmp[i] & 0xFF000000u) | (ar << 16) | (ag << 8) | ab;

            uint32_t ov = (i - radius) < 0 ? first : tmp[i - radius];
            uint32_t nv = (i + radius + 1) >= w ? last : tmp[i + radius + 1];
            sr += (int)((nv >> 16) & 0xFF) - (int)((ov >> 16) & 0xFF);
            sg += (int)((nv >> 8) & 0xFF)  - (int)((ov >> 8) & 0xFF);
            sb += (int)(nv & 0xFF)         - (int)(ov & 0xFF);
        }
    }

    /* Vertical pass — blur each column (R, G, B together) */
    for (int col = x; col < x + w; col++) {
        for (int i = 0; i < h; i++)
            tmp[i] = s->buf[(y + i) * s->pitch + col];

        int sr = 0, sg = 0, sb = 0;
        uint32_t first = tmp[0], last = tmp[h - 1];
        for (int i = -radius; i <= radius; i++) {
            uint32_t p = i < 0 ? first : (i >= h ? last : tmp[i]);
            sr += (p >> 16) & 0xFF; sg += (p >> 8) & 0xFF; sb += p & 0xFF;
        }

        for (int i = 0; i < h; i++) {
            uint32_t ar = (sr * recip) >> 16;
            uint32_t ag = (sg * recip) >> 16;
            uint32_t ab = (sb * recip) >> 16;
            s->buf[(y + i) * s->pitch + col] =
                (tmp[i] & 0xFF000000u) | (ar << 16) | (ag << 8) | ab;

            uint32_t ov = (i - radius) < 0 ? first : tmp[i - radius];
            uint32_t nv = (i + radius + 1) >= h ? last : tmp[i + radius + 1];
            sr += (int)((nv >> 16) & 0xFF) - (int)((ov >> 16) & 0xFF);
            sg += (int)((nv >> 8) & 0xFF)  - (int)((ov >> 8) & 0xFF);
            sb += (int)(nv & 0xFF)         - (int)(ov & 0xFF);
        }
    }

    /* tmp is retained for next call */
}

void draw_blit_keyed(surface_t *dst, int dx, int dy,
                     const uint32_t *src, int sw, int sh, uint32_t key_color)
{
    for (int y = 0; y < sh; y++) {
        if (dy + y < 0 || dy + y >= dst->h)
            continue;
        for (int x = 0; x < sw; x++) {
            if (dx + x < 0 || dx + x >= dst->w)
                continue;
            uint32_t px = src[y * sw + x];
            if (px != key_color)
                dst->buf[(dy + y) * dst->pitch + (dx + x)] = px;
        }
    }
}

void draw_blend_rect(surface_t *s, int x, int y, int w, int h,
                     uint32_t color, int alpha)
{
    /* Clamp region to surface bounds */
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > s->w) w = s->w - x;
    if (y + h > s->h) h = s->h - y;
    if (w <= 0 || h <= 0)
        return;

    if (alpha < 0) alpha = 0;
    if (alpha > 255) alpha = 255;
    uint32_t a = (uint32_t)alpha;
    uint32_t inv = 255u - a;

    /* Packed alpha blend: process the R+B lanes together and G separately, so
     * each pixel costs 2 multiplies (was 3 with per-channel unpack) and a
     * shift-by-8 instead of a divide-by-255. The +0x80 per lane rounds to
     * nearest; vs the exact /255 this differs by at most 1 LSB per channel —
     * imperceptible for the translucent frosted-glass tints this draws. */
    uint32_t crb = color & 0x00FF00FFu;
    uint32_t cg  = color & 0x0000FF00u;

    for (int j = y; j < y + h; j++) {
        uint32_t *rowp = &s->buf[j * s->pitch];
        for (int i = x; i < x + w; i++) {
            uint32_t px = rowp[i];
            uint32_t rb = ((crb * a + (px & 0x00FF00FFu) * inv + 0x00800080u) >> 8)
                          & 0x00FF00FFu;
            uint32_t g  = ((cg  * a + (px & 0x0000FF00u) * inv + 0x00008000u) >> 8)
                          & 0x0000FF00u;
            rowp[i] = rb | g;
        }
    }
}

/* Is pixel (px,py) — given relative to a rect's top-left — inside a rounded
 * rectangle of size w x h with corner radius r? Used by the outline routine. */
static int
rr_inside(int px, int py, int w, int h, int r)
{
    if (px < 0 || py < 0 || px >= w || py >= h)
        return 0;
    if (r <= 0)
        return 1;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    int dx, dy;
    if (px < r && py < r)             { dx = r - px;               dy = r - py; }
    else if (px >= w - r && py < r)   { dx = px - (w - r - 1);     dy = r - py; }
    else if (px < r && py >= h - r)   { dx = r - px;               dy = py - (h - r - 1); }
    else if (px >= w - r && py >= h - r){ dx = px - (w - r - 1);   dy = py - (h - r - 1); }
    else
        return 1;   /* straight edges / center */
    return dx * dx + dy * dy <= r * r;
}

void
draw_rounded_outline(surface_t *s, int x, int y, int w, int h,
                     int r, int thickness, uint32_t color)
{
    if (w <= 0 || h <= 0)
        return;
    if (thickness < 1) thickness = 1;
    for (int py = 0; py < h; py++) {
        for (int px = 0; px < w; px++) {
            int outer = rr_inside(px, py, w, h, r);
            int inner = rr_inside(px - thickness, py - thickness,
                                  w - 2 * thickness, h - 2 * thickness,
                                  r - thickness);
            if (outer && !inner)
                draw_px(s, x + px, y + py, color);
        }
    }
}

/* Composite one ARGB source pixel over a destination pixel. */
static inline uint32_t
blend_argb_over(uint32_t px, uint32_t bg)
{
    uint32_t a = (px >> 24) & 0xFF;
    if (a == 0xFF)
        return px & 0x00FFFFFF;
    uint32_t inv = 255 - a;
    uint32_t r = (((px >> 16) & 0xFF) * a + ((bg >> 16) & 0xFF) * inv) / 255;
    uint32_t g = (((px >> 8) & 0xFF) * a + ((bg >> 8) & 0xFF) * inv) / 255;
    uint32_t b = ((px & 0xFF) * a + (bg & 0xFF) * inv) / 255;
    return (r << 16) | (g << 8) | b;
}

void
draw_blit_alpha_scaled(surface_t *s, int dx, int dy, int dw, int dh,
                       const uint32_t *src, int sw, int sh)
{
    if (!src || dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0)
        return;
    for (int row = 0; row < dh; row++) {
        int y = dy + row;
        if (y < 0 || y >= s->h) continue;
        int sr = row * sh / dh;
        const uint32_t *srow = &src[sr * sw];
        uint32_t *drow = &s->buf[y * s->pitch];
        for (int col = 0; col < dw; col++) {
            int x = dx + col;
            if (x < 0 || x >= s->w) continue;
            uint32_t px = srow[col * sw / dw];
            if (((px >> 24) & 0xFF) == 0) continue;
            drow[x] = blend_argb_over(px, drow[x]);
        }
    }
}

void
draw_blit_scaled_alpha(surface_t *s, int dx, int dy, int dw, int dh,
                       const uint32_t *src, int sw, int sh, int src_pitch,
                       int global_alpha, uint32_t key_color)
{
    if (!src || dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0)
        return;
    if (global_alpha <= 0) return;
    if (global_alpha > 255) global_alpha = 255;
    int a = global_alpha, inv = 255 - a;
    uint32_t key = key_color & 0x00FFFFFF;
    for (int row = 0; row < dh; row++) {
        int y = dy + row;
        if (y < 0 || y >= s->h) continue;
        int sr = row * sh / dh;
        const uint32_t *srow = &src[sr * src_pitch];
        uint32_t *drow = &s->buf[y * s->pitch];
        for (int col = 0; col < dw; col++) {
            int x = dx + col;
            if (x < 0 || x >= s->w) continue;
            uint32_t px = srow[col * sw / dw] & 0x00FFFFFF;
            if (px == key) continue;
            if (a == 255) { drow[x] = px; continue; }
            uint32_t bg = drow[x];
            uint32_t r = (((px >> 16) & 0xFF) * a + ((bg >> 16) & 0xFF) * inv) / 255;
            uint32_t g = (((px >> 8) & 0xFF) * a + ((bg >> 8) & 0xFF) * inv) / 255;
            uint32_t b = ((px & 0xFF) * a + (bg & 0xFF) * inv) / 255;
            drow[x] = (r << 16) | (g << 8) | b;
        }
    }
}
