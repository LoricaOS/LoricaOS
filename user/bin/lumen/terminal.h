/* terminal.h -- Lumen dropdown terminal (wraps glyph_term core)
 *
 * Regular terminal windows are the standalone /bin/terminal external
 * client since Phase 47b's terminal peel; only the dropdown remains
 * in-process. All helpers below no-op unless `win` IS the dropdown
 * window (main.c may pass any focused window, including proxies). */
#ifndef LUMEN_TERMINAL_H
#define LUMEN_TERMINAL_H

#include <glyph.h>

glyph_window_t *terminal_create_dropdown(int screen_w, int screen_h,
                                         int *master_fd_out);
void terminal_write(glyph_window_t *win, const char *data, int len);

/* Selection / clipboard helpers */
int terminal_has_selection(glyph_window_t *win);
int terminal_copy_selection(glyph_window_t *win, char *buf, int max);
void terminal_clear_selection(glyph_window_t *win);

#endif
