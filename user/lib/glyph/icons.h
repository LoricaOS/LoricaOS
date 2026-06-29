/* icons.h — shared procedural app icons.
 *
 * One icon source for citadel-dock and the Applications launcher so an
 * app looks the same everywhere. An app may ship its own /apps/<id>/icon.png
 * (it travels with the bundle); if present it is decoded, scaled and used.
 * Otherwise the built-in procedural artwork is drawn with draw.c primitives
 * (no image assets); `size` is the icon box edge in pixels — all
 * geometry must scale from it (the dock draws at 48, the launcher
 * larger).
 *
 * Known procedural ids (bundle dir names + shell builtins): "applications"
 * (grid), "settings", "filemanager" (alias "files" — folder), "terminal",
 * "calculator", "editor", "gui-installer" (drive + arrow).
 * Unknown ids with no icon.png get a letter-tile fallback: rounded rect in a
 * color hashed from id, first letter of `label` (or id) centered.
 */
#ifndef GLYPH_ICONS_H
#define GLYPH_ICONS_H

#include "draw.h"

/* Draw the icon for `id` filling the size x size box at (x,y) on s.
 * label is the display name (used only by the fallback tile; may be
 * NULL). */
void glyph_icon_draw(surface_t *s, const char *id, const char *label,
                     int x, int y, int size);

#endif /* GLYPH_ICONS_H */
