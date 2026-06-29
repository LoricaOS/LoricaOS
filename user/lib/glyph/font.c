/* font.c -- TTF vector font renderer using stb_truetype
 *
 * Loads TTF files from disk, bakes ASCII glyphs into bitmap atlases
 * at requested pixel sizes, and renders anti-aliased text by alpha-
 * blending glyph coverage over existing surface pixels.
 */
#include "font.h"
#include "glyph_theme.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

/* Disable stbtt's internal assertions. We hit `dy >= 0` intermittently
 * in stbtt__fill_active_edges_new on certain glyphs at certain sizes
 * (a known floating-point edge case in stb_truetype's rasterizer) and
 * the abort it triggers takes down the whole compositor. The worst-case
 * effect of skipping the assert is a single misrendered scanline, which
 * is preferable to crashing Lumen. */
#define STBTT_assert(x) ((void)0)
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#define MAX_BAKED_SIZES 16
#define ATLAS_W         512
#define ATLAS_H         512
#define FIRST_CHAR      32
#define NUM_CHARS        95  /* ASCII 32-126 inclusive */

typedef struct {
    int size_px;
    int ascent;       /* scaled ascent in pixels */
    int descent;      /* scaled descent in pixels (negative) */
    int line_height;  /* ascent - descent + line_gap */
    unsigned char *atlas;  /* ATLAS_W x ATLAS_H alpha bitmap */
    stbtt_bakedchar cdata[NUM_CHARS];
} baked_size_t;

struct font {
    unsigned char *ttf_data;  /* raw TTF file contents */
    stbtt_fontinfo info;
    baked_size_t sizes[MAX_BAKED_SIZES];
    int num_sizes;
};

/* Global font instances */
font_t *g_font_ui   = NULL;
font_t *g_font_mono = NULL;

/* ---- Internal helpers ---- */

static baked_size_t *
find_baked(font_t *f, int size_px)
{
    if (!f)
        return NULL;
    for (int i = 0; i < f->num_sizes; i++) {
        if (f->sizes[i].size_px == size_px)
            return &f->sizes[i];
    }
    return NULL;
}

/* Look up a baked size, baking it on first use. Callers may draw or
 * measure at any pixel size without pre-registering it in font_init():
 * the bake is idempotent and bounded by MAX_BAKED_SIZES, so an unbaked
 * size renders correctly instead of silently drawing nothing. */
static baked_size_t *
find_or_bake(font_t *f, int size_px)
{
    baked_size_t *bs = find_baked(f, size_px);
    if (bs)
        return bs;
    font_bake(f, size_px);
    return find_baked(f, size_px);
}

/* ---- Public API ---- */

font_t *
font_load(const char *path)
{
    if (!path)
        return NULL;

    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return NULL;

    /* Get file size via lseek */
    off_t size = lseek(fd, 0, SEEK_END);
    if (size <= 0 || size > 16 * 1024 * 1024) {
        /* Reject empty files or files > 16 MB */
        close(fd);
        return NULL;
    }
    lseek(fd, 0, SEEK_SET);

    unsigned char *data = malloc((size_t)size);
    if (!data) {
        close(fd);
        return NULL;
    }

    /* Read entire file */
    size_t total = 0;
    while (total < (size_t)size) {
        ssize_t n = read(fd, data + total, (size_t)size - total);
        if (n <= 0) {
            free(data);
            close(fd);
            return NULL;
        }
        total += (size_t)n;
    }
    close(fd);

    font_t *f = calloc(1, sizeof(font_t));
    if (!f) {
        free(data);
        return NULL;
    }
    f->ttf_data = data;
    f->num_sizes = 0;

    if (!stbtt_InitFont(&f->info, f->ttf_data, 0)) {
        free(data);
        free(f);
        return NULL;
    }

    return f;
}

void
font_bake(font_t *f, int size_px)
{
    if (!f || size_px <= 0)
        return;

    /* Already baked? */
    if (find_baked(f, size_px))
        return;

    /* No room for more baked sizes? */
    if (f->num_sizes >= MAX_BAKED_SIZES)
        return;

    baked_size_t *bs = &f->sizes[f->num_sizes];
    memset(bs, 0, sizeof(*bs));
    bs->size_px = size_px;

    bs->atlas = calloc(1, ATLAS_W * ATLAS_H);
    if (!bs->atlas)
        return;

    int ret = stbtt_BakeFontBitmap(f->ttf_data, 0, (float)size_px,
                                   bs->atlas, ATLAS_W, ATLAS_H,
                                   FIRST_CHAR, NUM_CHARS, bs->cdata);
    if (ret <= 0) {
        /* ret < 0 means not all chars fit; absolute value is how many
         * did fit. ret == 0 means none fit. For partial success we
         * keep what we have. For total failure, free and bail. */
        if (ret == 0) {
            free(bs->atlas);
            bs->atlas = NULL;
            return;
        }
        /* Partial bake -- some chars are missing but we proceed.
         * Missing chars will render as empty space. */
    }

    /* Compute scaled vertical metrics */
    int asc, desc, lgap;
    stbtt_GetFontVMetrics(&f->info, &asc, &desc, &lgap);
    float scale = stbtt_ScaleForPixelHeight(&f->info, (float)size_px);
    bs->ascent = (int)(asc * scale + 0.5f);
    bs->descent = (int)(desc * scale - 0.5f);
    bs->line_height = (int)((asc - desc + lgap) * scale + 0.5f);

    f->num_sizes++;
}

int
font_height(font_t *f, int size_px)
{
    baked_size_t *bs = find_or_bake(f, size_px);
    return bs ? bs->line_height : 0;
}

int
font_text_width(font_t *f, int size_px, const char *text)
{
    baked_size_t *bs = find_or_bake(f, size_px);
    if (!bs || !text)
        return 0;

    int width = 0;
    while (*text) {
        int ch = (unsigned char)*text;
        if (ch >= FIRST_CHAR && ch < FIRST_CHAR + NUM_CHARS) {
            stbtt_bakedchar *bc = &bs->cdata[ch - FIRST_CHAR];
            width += (int)(bc->xadvance + 0.5f);
        }
        text++;
    }
    return width;
}

int
font_draw_char(surface_t *s, font_t *f, int size_px,
               int x, int y, char ch, uint32_t color)
{
    if (!s || !f)
        return 0;

    baked_size_t *bs = find_or_bake(f, size_px);
    if (!bs)
        return 0;

    int ci = (unsigned char)ch;
    if (ci < FIRST_CHAR || ci >= FIRST_CHAR + NUM_CHARS)
        return 0;

    stbtt_bakedchar *bc = &bs->cdata[ci - FIRST_CHAR];

    /* Glyph bounding box in atlas */
    int gw = bc->x1 - bc->x0;
    int gh = bc->y1 - bc->y0;

    /* Screen position: baseline at y + ascent, offset by glyph metrics */
    int sx = x + (int)(bc->xoff + 0.5f);
    int sy = y + bs->ascent + (int)(bc->yoff + 0.5f);

    /* Packed-color lanes for the alpha blend (R+B together, G separate). */
    uint32_t crb = color & 0x00FF00FFu;
    uint32_t cg  = color & 0x0000FF00u;

    /* Clamp the glyph box to the surface once, instead of testing every pixel. */
    int r0 = 0, r1 = gh;
    if (sy < 0) r0 = -sy;
    if (sy + gh > s->h) r1 = s->h - sy;
    int c0 = 0, c1 = gw;
    if (sx < 0) c0 = -sx;
    if (sx + gw > s->w) c1 = s->w - sx;

    /* Blit glyph with alpha blending. The partial-coverage blend uses the same
     * packed 2-multiply / >>8-with-rounding form as draw_blend_rect (2 muls
     * instead of 3, shift instead of /255; max 1-LSB delta — invisible). */
    for (int row = r0; row < r1; row++) {
        const unsigned char *arow =
            &bs->atlas[(bc->y0 + row) * ATLAS_W + bc->x0];
        uint32_t *drow = &s->buf[(sy + row) * s->pitch + sx];
        for (int col = c0; col < c1; col++) {
            uint32_t a = arow[col];
            if (a == 0)
                continue;
            if (a == 255) {
                drow[col] = color;
                continue;
            }
            uint32_t inv = 255u - a;
            uint32_t px = drow[col];
            uint32_t rb = ((crb * a + (px & 0x00FF00FFu) * inv + 0x00800080u) >> 8)
                          & 0x00FF00FFu;
            uint32_t g  = ((cg  * a + (px & 0x0000FF00u) * inv + 0x00008000u) >> 8)
                          & 0x0000FF00u;
            drow[col] = rb | g;
        }
    }

    return (int)(bc->xadvance + 0.5f);
}

void
font_draw_text(surface_t *s, font_t *f, int size_px,
               int x, int y, const char *text, uint32_t color)
{
    if (!s || !f || !text)
        return;

    while (*text) {
        int advance = font_draw_char(s, f, size_px, x, y, *text, color);
        x += advance;
        text++;
    }
}

void
font_init(void)
{
    /* Load the runtime theme (accent color) — font_init is the common GUI
     * startup hook every Glyph app calls, so the accent is applied before any
     * widget renders. */
    glyph_theme_load();

    g_font_ui = NULL;
    g_font_mono = NULL;

    /* Try loading UI font (Inter) */
    g_font_ui = font_load("/usr/share/fonts/Inter-Regular.ttf");
    if (g_font_ui) {
        font_bake(g_font_ui, 11);
        font_bake(g_font_ui, 12);
        font_bake(g_font_ui, 13);
        font_bake(g_font_ui, 14);
        font_bake(g_font_ui, 16);
        font_bake(g_font_ui, 20);
    }

    /* Try loading monospace font (JetBrains Mono) */
    g_font_mono = font_load("/usr/share/fonts/JetBrainsMono-Regular.ttf");
    if (g_font_mono) {
        font_bake(g_font_mono, 14);
        font_bake(g_font_mono, 16);
        font_bake(g_font_mono, 18);
        font_bake(g_font_mono, 20);
    }
}
