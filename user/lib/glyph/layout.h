/* layout.h — geometry helpers for immediate-mode layout in Glyph apps.
 *
 * No widgets and no retained tree: these just compute glyph_rect_t
 * sub-rectangles, so apps stop hardcoding pixel coordinates off WIN_W/WIN_H
 * constants. Feed the live client rect and everything reflows for free when the
 * window resizes. Compose freely — cut a toolbar off the top, a status bar off
 * the bottom, then grid the middle.
 */
#ifndef GLYPH_LAYOUT_H
#define GLYPH_LAYOUT_H

#include "glyph.h"   /* glyph_rect_t */

/* Shrink a rect by uniform padding, or per-side (top,right,bottom,left). */
glyph_rect_t glyph_inset(glyph_rect_t r, int pad);
glyph_rect_t glyph_inset4(glyph_rect_t r, int top, int right, int bottom, int left);

/* The (col,row) cell of a cols×rows grid of equal cells filling `bounds`, with
 * `gap` pixels between cells. Row/col are 0-based. */
glyph_rect_t glyph_grid_cell(glyph_rect_t bounds, int cols, int rows, int gap,
                             int col, int row);

/* Slice a strip off one edge of *rem and shrink *rem by the strip plus `gap`.
 * Returns the strip. Peel a toolbar off the top, a status bar off the bottom, a
 * right-aligned button off a row — *rem is left as the remaining content area,
 * no pre-counting or edge math. */
glyph_rect_t glyph_cut_top(glyph_rect_t *rem, int h, int gap);
glyph_rect_t glyph_cut_bottom(glyph_rect_t *rem, int h, int gap);
glyph_rect_t glyph_cut_left(glyph_rect_t *rem, int w, int gap);
glyph_rect_t glyph_cut_right(glyph_rect_t *rem, int w, int gap);

#endif /* GLYPH_LAYOUT_H */
