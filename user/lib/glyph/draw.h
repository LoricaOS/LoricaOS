/* draw.h — Framebuffer drawing primitives for Glyph widget toolkit */
#ifndef GLYPH_DRAW_H
#define GLYPH_DRAW_H

#include <stdint.h>
#include "glyph_theme.h"

#define FONT_W 10
#define FONT_H 20

typedef struct {
    uint32_t *buf;
    int w, h, pitch;  /* pitch in pixels */
} surface_t;

void draw_px(surface_t *s, int x, int y, uint32_t c);
void draw_fill_rect(surface_t *s, int x, int y, int w, int h, uint32_t c);
void draw_rect(surface_t *s, int x, int y, int w, int h, uint32_t c);
void draw_gradient_v(surface_t *s, int x, int y, int w, int h, uint32_t c1, uint32_t c2);
void draw_char(surface_t *s, int x, int y, char ch, uint32_t fg, uint32_t bg);
void draw_text(surface_t *s, int x, int y, const char *str, uint32_t fg, uint32_t bg);
void draw_text_t(surface_t *s, int x, int y, const char *str, uint32_t fg);
void draw_blit(surface_t *dst, int dx, int dy, const uint32_t *src, int sw, int sh);
void draw_line(surface_t *s, int x0, int y0, int x1, int y1, uint32_t color);
void draw_circle(surface_t *s, int cx, int cy, int r, uint32_t color);
void draw_circle_filled(surface_t *s, int cx, int cy, int r, uint32_t color);
void draw_rounded_rect(surface_t *s, int x, int y, int w, int h, int r, uint32_t color);
void draw_blit_scaled(surface_t *dst, int dx, int dy, int dw, int dh,
                      const uint32_t *src, int sw, int sh);
void draw_text_center(surface_t *s, int x, int y, int w, const char *str,
                      uint32_t fg, uint32_t bg);
void draw_box_blur(surface_t *s, int x, int y, int w, int h, int radius);
void draw_blend_rect(surface_t *s, int x, int y, int w, int h,
                     uint32_t color, int alpha);
void draw_blend_rounded_rect(surface_t *s, int x, int y, int w, int h,
                             int r, uint32_t color, int alpha);
void draw_blit_keyed(surface_t *dst, int dx, int dy,
                     const uint32_t *src, int sw, int sh, uint32_t key_color);
/* Blit an ARGB image (alpha in the high byte, 0xAARRGGBB) with per-pixel
 * alpha compositing over existing surface content, nearest-neighbor scaled
 * to dw x dh. Fully-transparent pixels are skipped. */
void draw_blit_alpha_scaled(surface_t *dst, int dx, int dy, int dw, int dh,
                            const uint32_t *src, int sw, int sh);
/* Nearest-neighbor scale of an OPAQUE source (row stride src_pitch px) to
 * dw x dh, composited over the destination at a uniform global alpha (0-255).
 * Source pixels equal to key_color are skipped (pass 0xFFFFFFFF to disable
 * keying). Used for window open/close scale+fade animation. */
void draw_blit_scaled_alpha(surface_t *dst, int dx, int dy, int dw, int dh,
                            const uint32_t *src, int sw, int sh, int src_pitch,
                            int global_alpha, uint32_t key_color);

/* TTF-aware text measurement helpers (fall back to bitmap FONT_W/FONT_H) */
int glyph_text_width(const char *text);
int glyph_text_height(void);
int glyph_char_width(void);
/* Draw text using TTF UI font if available, bitmap fallback */
void draw_text_ui(surface_t *s, int x, int y, const char *str, uint32_t fg);

/* ---- Legacy palette — now aliases onto the semantic tokens in theme.h ----
 * These C_* names are kept so existing call sites compile unchanged; the
 * single source of truth for every value is theme.h. Prefer THEME_* in new
 * code. The two color-KEYS (C_SHADOW, C_TERM_BG) are a compositor protocol
 * contract — they alias the fixed THEME_KEY_* values and must never change. */
#define C_BG1       THEME_DESKTOP_TOP
#define C_BG2       THEME_DESKTOP_BOT
#define C_BAR       THEME_BG
#define C_BAR_T     THEME_TEXT_DIM
#define C_WIN       THEME_TEXT          /* primary text on dark backgrounds */
#define C_WIN_BG    THEME_SURFACE       /* widget surface / card background */
#define C_BORDER    THEME_BORDER
#define C_TITLE1    THEME_SURFACE_2     /* focused titlebar gradient (top)  */
#define C_TITLE2    THEME_SURFACE       /* focused titlebar gradient (bot)  */
#define C_UTITLE1   THEME_SURFACE       /* unfocused titlebar gradient      */
#define C_UTITLE2   THEME_BG
#define C_TITLE_T   THEME_TEXT
#define C_TEXT      THEME_TEXT
#define C_RED       THEME_ERROR
#define C_GREEN     THEME_OK
#define C_YELLOW    THEME_WARN
#define C_BTN       THEME_ACCENT        /* primary action color */
#define C_BTN_T     THEME_TEXT_ON_ACCENT
#define C_ACCENT    THEME_ACCENT
#define C_SUBTLE    THEME_TEXT_DIM
#define C_SHADOW    THEME_KEY_SHADOW    /* chrome transparency key — fixed */
#define C_TERM_FG   THEME_TEXT
#define C_TERM_BG   THEME_KEY_FROST     /* frosted-window key — fixed      */
/* Widget-specific colors */
#define C_INPUT_BG  THEME_INPUT_BG      /* text field / list background */
#define C_INPUT_BD  THEME_BORDER        /* text field / list border */
#define C_SEL_BG    THEME_SELECTION     /* selection / hover highlight */

/* Focus-ring helper: 2px accent rounded outline drawn just outside a control
 * of corner radius r. Widgets call this after drawing their body when they are
 * the window's focused widget. (Implemented in draw.c.) */
void draw_rounded_outline(surface_t *s, int x, int y, int w, int h,
                          int r, int thickness, uint32_t color);

#endif
