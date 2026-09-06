/* glyph_cursor.h — shared save-under arrow cursor for framebuffer clients.
 *
 * A singleton cursor drawn directly into a caller-supplied surface (the
 * framebuffer, or a backbuffer aliasing it). Before drawing, the pixels under
 * the cursor are saved; glyph_cursor_hide() restores them. Callers that write
 * to the target surface must hide the cursor first and show it after.
 *
 * Used by the Lumen compositor and the Bastion greeter (both draw straight to
 * the framebuffer via a glyph surface_t).
 */
#ifndef GLYPH_CURSOR_H__
#define GLYPH_CURSOR_H__

#include "draw.h"

#define GLYPH_CURSOR_W 16
#define GLYPH_CURSOR_H 20

/* Bind the cursor to a target surface (typically the mapped framebuffer). */
void glyph_cursor_init(surface_t *target);
/* Restore the pixels saved under the cursor (call before writing the target). */
void glyph_cursor_hide(void);
/* Save the pixels under (x,y) and blit the arrow sprite there. */
void glyph_cursor_show(int x, int y);
/* hide() then show(x,y) — move the cursor to a new position. */
void glyph_cursor_move(int x, int y);

#endif /* GLYPH_CURSOR_H__ */
