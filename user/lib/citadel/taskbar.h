/* taskbar.h — top bar for Aegis desktop shell */
#ifndef CITADEL_TASKBAR_H
#define CITADEL_TASKBAR_H

#include <glyph.h>

/* Draw the top bar: Aegis menu (left), a volume slider + clock (right).
 * `volume` is the output level 0..100. `backdrop_dirty` must be non-zero
 * whenever the desktop background behind the bar changed (a full redraw) so the
 * cached frosted-glass blur is recomputed; pass 0 to reuse the cache. */
void topbar_draw(surface_t *s, int screen_w, const char *clock_str, int volume,
                 int backdrop_dirty);

int topbar_hit_aegis(int mx, int my, int screen_w);

/* Volume slider hit-testing. _at requires the click to land on the bar +
 * track (returns 0..100 or -1); _from_x maps any x to 0..100 for dragging. */
int topbar_volume_at(int mx, int my, int screen_w, const char *clock_str);
int topbar_volume_from_x(int mx, int screen_w, const char *clock_str);

#endif
