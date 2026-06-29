/* font.h -- TTF vector font renderer for Glyph widget toolkit
 *
 * Uses stb_truetype to bake ASCII glyphs into bitmap atlases at
 * specific pixel sizes. Alpha-blended rendering over existing
 * surface content (transparent background).
 *
 * Falls back gracefully: if TTF fonts are not found on disk,
 * callers should use the existing bitmap draw_char/draw_text
 * functions from draw.h instead.
 */
#ifndef GLYPH_FONT_H
#define GLYPH_FONT_H

#include "draw.h"
#include <stdint.h>

/* Font handle (opaque) */
typedef struct font font_t;

/* Initialize the font system -- call once at startup.
 * Attempts to load Inter (UI) and JetBrains Mono (terminal)
 * from /usr/share/fonts/. Sets g_font_ui and g_font_mono. */
void font_init(void);

/* Load a TTF font from a file path. Returns NULL on failure. */
font_t *font_load(const char *path);

/* Pre-bake a font at a specific pixel size. Must be called before
 * drawing at that size. Bakes ASCII 32-126 into a bitmap atlas.
 * No-op if the size is already baked. */
void font_bake(font_t *f, int size_px);

/* Get metrics for a baked size. Returns 0 if size not baked. */
int font_height(font_t *f, int size_px);
int font_text_width(font_t *f, int size_px, const char *text);

/* Draw a string with anti-aliased TTF font. Alpha-blends text over
 * the existing surface content (transparent background). */
void font_draw_text(surface_t *s, font_t *f, int size_px,
                    int x, int y, const char *text, uint32_t color);

/* Draw a single character. Returns advance width in pixels. */
int font_draw_char(surface_t *s, font_t *f, int size_px,
                   int x, int y, char ch, uint32_t color);

/* Global font instances (set after font_init + font_load) */
extern font_t *g_font_ui;    /* Inter -- for UI elements */
extern font_t *g_font_mono;  /* JetBrains Mono -- for terminal */

#endif /* GLYPH_FONT_H */
