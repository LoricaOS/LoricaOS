/* layout.c — glyph_rect_t geometry helpers (see layout.h). */
#include "layout.h"

glyph_rect_t
glyph_inset(glyph_rect_t r, int pad)
{
    return (glyph_rect_t){ r.x + pad, r.y + pad, r.w - 2 * pad, r.h - 2 * pad };
}

glyph_rect_t
glyph_inset4(glyph_rect_t r, int top, int right, int bottom, int left)
{
    return (glyph_rect_t){ r.x + left, r.y + top,
                           r.w - left - right, r.h - top - bottom };
}

glyph_rect_t
glyph_grid_cell(glyph_rect_t b, int cols, int rows, int gap, int col, int row)
{
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    int cw = (b.w - (cols - 1) * gap) / cols;
    int ch = (b.h - (rows - 1) * gap) / rows;
    return (glyph_rect_t){ b.x + col * (cw + gap),
                           b.y + row * (ch + gap), cw, ch };
}

glyph_rect_t
glyph_cut_top(glyph_rect_t *rem, int h, int gap)
{
    glyph_rect_t s = { rem->x, rem->y, rem->w, h };
    rem->y += h + gap;
    rem->h -= h + gap;
    return s;
}

glyph_rect_t
glyph_cut_bottom(glyph_rect_t *rem, int h, int gap)
{
    glyph_rect_t s = { rem->x, rem->y + rem->h - h, rem->w, h };
    rem->h -= h + gap;
    return s;
}

glyph_rect_t
glyph_cut_left(glyph_rect_t *rem, int w, int gap)
{
    glyph_rect_t s = { rem->x, rem->y, w, rem->h };
    rem->x += w + gap;
    rem->w -= w + gap;
    return s;
}

glyph_rect_t
glyph_cut_right(glyph_rect_t *rem, int w, int gap)
{
    glyph_rect_t s = { rem->x + rem->w - w, rem->y, w, rem->h };
    rem->w -= w + gap;
    return s;
}
