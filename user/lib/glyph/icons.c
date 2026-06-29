/* icons.c — shared procedural app icons (see icons.h).
 *
 * Ported from the citadel-dock draw_icon_* renderers and parameterized
 * by `size` so the same artwork scales from the 48px dock to the larger
 * Applications launcher. Every offset/dimension is an integer fraction
 * of `size`, chosen so that size == 48 reproduces the original dock
 * pixel geometry exactly (e.g. the folder's 40px body is size*5/6, the
 * gear's 18px radius is size*3/8).
 */
#include "icons.h"
#include "draw.h"
#include "font.h"
#include "image_load.h"
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>

/* Icon-local colors not in the draw.h palette (verbatim from the old
 * dock renderers). */
#define ICON_FOLDER_BODY 0x00CC9944
#define ICON_FOLDER_DARK 0x00BB8833
#define ICON_DARK_TINT   0x001A1A2E   /* == C_BAR; gear hub + term bg */
#define ICON_GREEN_LT    0x0050FA7B
#define ICON_ORANGE      0x00FF7744
#define ICON_PURPLE      0x00CC66FF
#define ICON_CALC_BODY   0x00323848
#define ICON_CALC_KEY    0x00C0C8D0
#define ICON_PAPER       0x00E8E8F0
#define ICON_PAPER_LINE  0x00808890
#define ICON_DRIVE_BODY  0x00606878

/* Thin features (highlight bars, text lines) must not vanish to 0px at
 * small sizes. */
static int
at_least(int v, int min)
{
    return v < min ? min : v;
}

/* ── Authored PNG icons (portable: ship in the app bundle) ─────────────────
 *
 * Before falling back to the procedural artwork below, glyph_icon_draw tries
 * /apps/<id>/icon.png so a package can ship its own icon (it travels with the
 * bundle, no toolkit change needed). Decoding + scaling per frame is wasteful,
 * so results are cached — positively (the scaled size×size pixels) and
 * negatively (NULL: there is no PNG for this id) — keyed by (id,size).
 *
 * ponytail: a fixed round-robin ring of ICON_CACHE_MAX slots, no LRU. The
 * working set (dock + launcher icons at two sizes) fits comfortably; a larger
 * churning set just re-decodes on eviction. Pixels are 0x00RRGGBB (alpha
 * dropped by image_load); the icon is blitted with pure black (0x000000) keyed
 * out so the bundle's transparent background lets the desktop show through. */
#define ICON_CACHE_MAX 16
typedef struct {
    char     id[64];
    int      size;
    uint32_t *px;   /* size*size scaled pixels, owned; NULL = no PNG (neg cache) */
} icon_cache_t;
static icon_cache_t s_icon_cache[ICON_CACHE_MAX];
static int s_icon_cache_next;   /* round-robin eviction cursor */

/* Return a cached size×size icon for id, or NULL if id has no icon.png.
 * Decodes + scales on the first request and caches the (positive or negative)
 * result. The returned buffer is owned by the cache — do not free it. */
static const uint32_t *
icon_png_cached(const char *id, int size)
{
    char path[128];
    glyph_pixbuf_t img;
    icon_cache_t *slot;
    uint32_t *scaled = NULL;
    int i;

    if (size <= 0)
        return NULL;

    for (i = 0; i < ICON_CACHE_MAX; i++) {
        slot = &s_icon_cache[i];
        if (slot->id[0] && slot->size == size && strcmp(slot->id, id) == 0)
            return slot->px;   /* hit (NULL when negatively cached) */
    }

    snprintf(path, sizeof(path), "/apps/%s/icon.png", id);
    if (glyph_pixbuf_load_file(path, &img) == 0 &&
        img.px && img.w > 0 && img.h > 0) {
        scaled = malloc((size_t)size * size * 4);
        if (scaled) {
            surface_t tmp;
            tmp.buf = scaled;
            tmp.w = size;
            tmp.h = size;
            tmp.pitch = size;
            memset(scaled, 0, (size_t)size * size * 4);
            draw_blit_scaled(&tmp, 0, 0, size, size, img.px, img.w, img.h);
        }
    }
    glyph_pixbuf_free(&img);

    /* Insert into the ring, freeing whatever we evict. */
    slot = &s_icon_cache[s_icon_cache_next];
    s_icon_cache_next = (s_icon_cache_next + 1) % ICON_CACHE_MAX;
    if (slot->px)
        free(slot->px);
    memset(slot->id, 0, sizeof(slot->id));
    strncpy(slot->id, id, sizeof(slot->id) - 1);
    slot->size = size;
    slot->px = scaled;
    return scaled;
}

/* Gear: filled circle, four straight teeth, four diagonal nubs, dark
 * hub. size 48: r=18, teeth 8x6 reaching 22 out, nubs 6x6 at d=14,
 * hub r=8. */
static void
icon_settings(surface_t *s, int x, int y, int size)
{
    int cx = x + size / 2, cy = y + size / 2;
    int r = size * 3 / 8;          /* 18 @48 */
    int tw = size / 6;             /* tooth width  8 @48 */
    int th = size / 8;             /* tooth height 6 @48 */
    int out = size * 11 / 24;      /* tooth outer reach 22 @48 */
    int d = size * 7 / 24;         /* diagonal nub offset 14 @48 */
    int nub = size / 8;            /* nub edge 6 @48 */

    draw_circle_filled(s, cx, cy, r, C_ACCENT);
    draw_fill_rect(s, cx - tw / 2, cy - out, tw, th, C_ACCENT);
    draw_fill_rect(s, cx - tw / 2, cy + out - th, tw, th, C_ACCENT);
    draw_fill_rect(s, cx - out, cy - tw / 2, th, tw, C_ACCENT);
    draw_fill_rect(s, cx + out - th, cy - tw / 2, th, tw, C_ACCENT);
    draw_fill_rect(s, cx + d - nub / 2, cy - d - nub / 2, nub, nub, C_ACCENT);
    draw_fill_rect(s, cx - d - nub / 2, cy - d - nub / 2, nub, nub, C_ACCENT);
    draw_fill_rect(s, cx + d - nub / 2, cy + d - nub / 2, nub, nub, C_ACCENT);
    draw_fill_rect(s, cx - d - nub / 2, cy + d - nub / 2, nub, nub, C_ACCENT);
    draw_circle_filled(s, cx, cy, size / 6, ICON_DARK_TINT);
}

/* Folder: tab + body + darker top highlight. size 48: body 40x28
 * shifted down 2, tab 16x8 above. */
static void
icon_folder(surface_t *s, int x, int y, int size)
{
    int fw = size * 5 / 6;         /* 40 @48 */
    int fh = size * 7 / 12;        /* 28 @48 */
    int fx = x + (size - fw) / 2;
    int fy = y + (size - fh) / 2 + size / 24;

    draw_rounded_rect(s, fx, fy - size / 8, size / 3, size / 6,
                      at_least(size / 16, 1), ICON_FOLDER_BODY);
    draw_rounded_rect(s, fx, fy, fw, fh, size / 12, ICON_FOLDER_BODY);
    draw_fill_rect(s, fx + size / 24, fy + size / 24, fw - size / 12,
                   at_least(size / 16, 1), ICON_FOLDER_DARK);
}

/* Terminal: dark rounded screen with a ">_" prompt. size 48: 42x32
 * body, 14px TTF text. */
static void
icon_terminal(surface_t *s, int x, int y, int size)
{
    int tw = size * 7 / 8;         /* 42 @48 */
    int th = size * 2 / 3;         /* 32 @48 */
    int tx = x + (size - tw) / 2;
    int ty = y + (size - th) / 2;
    int fs = at_least(size * 7 / 24, 8);   /* font px, 14 @48 */

    draw_rounded_rect(s, tx, ty, tw, th, size / 8, ICON_DARK_TINT);
    if (g_font_ui) {
        int textw = font_text_width(g_font_ui, fs, ">_");
        font_draw_text(s, g_font_ui, fs, tx + (tw - textw) / 2,
                       ty + (th - font_height(g_font_ui, fs)) / 2,
                       ">_", C_TERM_FG);
    } else {
        /* Bitmap fallback is fixed 10x20; just center it. */
        draw_text(s, tx + (tw - 2 * FONT_W) / 2, ty + (th - FONT_H) / 2,
                  ">_", C_TERM_FG, ICON_DARK_TINT);
    }
}

/* Calculator: body, green display strip, 3x3 keypad dots. size 48:
 * 32x40 body, dots 4x4 on an 8x7 grid starting at (6,18). */
static void
icon_calc(surface_t *s, int x, int y, int size)
{
    int bw = size * 2 / 3;         /* 32 @48 */
    int bh = size * 5 / 6;         /* 40 @48 */
    int bx = x + (size - bw) / 2;
    int by = y + (size - bh) / 2;
    int dot = at_least(size / 12, 2);
    int r, c;

    draw_rounded_rect(s, bx, by, bw, bh, size * 5 / 48, ICON_CALC_BODY);
    /* Display */
    draw_rounded_rect(s, bx + size / 12, by + size / 12, bw - size / 6,
                      size / 6, at_least(size / 24, 1), ICON_GREEN_LT);
    /* Keypad dots (3x3) */
    for (r = 0; r < 3; r++)
        for (c = 0; c < 3; c++)
            draw_fill_rect(s, bx + size / 8 + c * (size / 6),
                           by + size * 3 / 8 + r * (size * 7 / 48),
                           dot, dot, ICON_CALC_KEY);
}

/* Editor: sheet of paper with text lines and a pencil across the
 * corner. size 48: 30x38 sheet, 4 lines on a 7px pitch. */
static void
icon_editor(surface_t *s, int x, int y, int size)
{
    int pw = size * 5 / 8;         /* 30 @48 */
    int ph = size * 19 / 24;       /* 38 @48 */
    int px = x + (size - pw) / 2;
    int py = y + (size - ph) / 2;
    int step = size * 7 / 48;      /* line pitch 7 @48 */
    int o = at_least(size / 48, 1);
    int i;

    draw_rounded_rect(s, px, py, pw, ph, at_least(size / 16, 1), ICON_PAPER);
    for (i = 0; i < 4; i++)
        draw_fill_rect(s, px + size * 5 / 48, py + step + i * step,
                       pw - size * 7 / 24, at_least(size / 24, 1),
                       ICON_PAPER_LINE);
    /* Pencil accent across the corner (two parallel lines). */
    draw_line(s, px + pw - size / 4, py + ph - size / 12,
              px + pw + size / 24, py + ph - size * 3 / 8, C_YELLOW);
    draw_line(s, px + pw - size / 4 + o, py + ph - size / 12 + o,
              px + pw + size / 24 + o, py + ph - size * 3 / 8 + o, C_YELLOW);
}

/* Installer: drive with activity LED and a downward install arrow.
 * size 48: 38x12 drive at the bottom, 6x16 shaft, head tapering from
 * half-width 10 over 8 rows. */
static void
icon_installer(surface_t *s, int x, int y, int size)
{
    int dw = size * 19 / 24;       /* 38 @48 */
    int dh = size / 4;             /* 12 @48 */
    int dx = x + (size - dw) / 2;
    int dy = y + size - dh - size / 12;
    int cx = x + size / 2;
    int half = size * 5 / 24;      /* arrowhead half-width 10 @48 */
    int rows = size / 6;           /* arrowhead height 8 @48 */
    int ay = y + size * 5 / 12;    /* arrowhead top 20 @48 */
    int r;

    /* Drive body + activity LED. */
    draw_rounded_rect(s, dx, dy, dw, dh, at_least(size / 16, 1),
                      ICON_DRIVE_BODY);
    draw_fill_rect(s, dx + dw - size / 6, dy + dh / 2 - 1,
                   at_least(size / 12, 2), at_least(size / 16, 2),
                   ICON_GREEN_LT);
    /* Arrow shaft + head pointing into the drive. */
    draw_fill_rect(s, cx - size / 16, y + size / 12, size / 8, size / 3,
                   C_ACCENT);
    for (r = 0; r < rows; r++) {
        int hw = at_least(half - r, 1);
        draw_fill_rect(s, cx - hw, ay + r, 2 * hw, 1, C_ACCENT);
    }
}

/* Applications: launchpad-style tile — dark rounded rect with a
 * C_BORDER outline holding a 3x3 grid of small rounded squares in
 * three accent colors. size 48: 42px tile, 8px cells, 4px gaps. */
static void
icon_applications(surface_t *s, int x, int y, int size)
{
    /* Non-static: C_ACCENT is runtime-themed, so this can't be a constant
     * initializer — and this way the icon tiles follow the chosen accent. */
    const uint32_t accents[3] = { C_ACCENT, C_GREEN, C_YELLOW };
    int inset = size / 16;         /* 3 @48 */
    int tsz = size - 2 * inset;    /* 42 @48 */
    int tr = size / 8;
    int pad = size / 6;            /* grid margin from box edge, 8 @48 */
    int cell = size / 6;           /* 8 @48 */
    int gap = (size - 2 * pad - 3 * cell) / 2;   /* 4 @48 */
    int cr = at_least(size / 24, 1);
    int row, col;

    /* Tile: 1px outline around the dark fill. */
    draw_rounded_rect(s, x + inset, y + inset, tsz, tsz, tr, C_BORDER);
    draw_rounded_rect(s, x + inset + 1, y + inset + 1, tsz - 2, tsz - 2,
                      at_least(tr - 1, 1), C_WIN_BG);
    for (row = 0; row < 3; row++)
        for (col = 0; col < 3; col++)
            draw_rounded_rect(s, x + pad + col * (cell + gap),
                              y + pad + row * (cell + gap),
                              cell, cell, cr, accents[(row + col) % 3]);
}

/* Unknown id: rounded tile in a color hashed from id, with the first
 * letter of label (or id) centered, uppercased. No font scaling — one
 * UI-font glyph centered in the box. */
static void
icon_fallback(surface_t *s, const char *id, const char *label,
              int x, int y, int size)
{
    /* Non-static: C_ACCENT is runtime-themed (see icon_applications). */
    const uint32_t palette[6] = {
        C_ACCENT, C_GREEN, C_YELLOW, C_RED, ICON_PURPLE, ICON_ORANGE,
    };
    unsigned hash = 0;
    const char *p, *src;
    char letter[2];
    int inset = size / 16;

    for (p = id; *p; p++)
        hash += (unsigned char)*p;
    draw_rounded_rect(s, x + inset, y + inset, size - 2 * inset,
                      size - 2 * inset, size / 8,
                      palette[hash % 6]);

    src = (label && label[0]) ? label : id;
    letter[0] = src[0] ? (char)toupper((unsigned char)src[0]) : '?';
    letter[1] = '\0';
    draw_text_ui(s, x + (size - glyph_text_width(letter)) / 2,
                 y + (size - glyph_text_height()) / 2, letter, C_TITLE_T);
}

void
glyph_icon_draw(surface_t *s, const char *id, const char *label,
                int x, int y, int size)
{
    if (!id)
        id = "";

    /* Authored PNG (ships in the app bundle) wins over the procedural icons:
     * /apps/<id>/icon.png, decoded + scaled + cached, black keyed out. */
    {
        const uint32_t *png = icon_png_cached(id, size);
        if (png) {
            draw_blit_keyed(s, x, y, png, size, size, 0x000000);
            return;
        }
    }

    if (strcmp(id, "applications") == 0)
        icon_applications(s, x, y, size);
    else if (strcmp(id, "settings") == 0)
        icon_settings(s, x, y, size);
    else if (strcmp(id, "filemanager") == 0 || strcmp(id, "files") == 0)
        icon_folder(s, x, y, size);
    else if (strcmp(id, "terminal") == 0)
        icon_terminal(s, x, y, size);
    else if (strcmp(id, "calculator") == 0)
        icon_calc(s, x, y, size);
    else if (strcmp(id, "editor") == 0)
        icon_editor(s, x, y, size);
    else if (strcmp(id, "gui-installer") == 0)
        icon_installer(s, x, y, size);
    else
        icon_fallback(s, id, label, x, y, size);
}
