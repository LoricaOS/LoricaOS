/* cursor.h -- Hardware cursor with save-under for Lumen compositor */
#ifndef LUMEN_CURSOR_H
#define LUMEN_CURSOR_H

#include <glyph.h>

#define CURSOR_W 16
#define CURSOR_H 20

void cursor_init(surface_t *fb);
void cursor_hide(void);
void cursor_show(int x, int y);
void cursor_move(int x, int y);

#endif
