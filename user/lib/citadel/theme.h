/* theme.h — shared visual constants for Aegis desktop shell (Citadel).
 * Color constants like C_ACCENT and C_TERM_FG are defined in glyph/draw.h. */
#ifndef CITADEL_THEME_H
#define CITADEL_THEME_H

#include <glyph_theme.h>   /* THEME_* semantic tokens (single source of truth) */

/* Top bar — colors come from glyph/glyph_theme.h. */
#define TOPBAR_HEIGHT   28
#define TOPBAR_BG       THEME_BG
#define TOPBAR_TEXT     THEME_TEXT_DIM
#define AEGIS_AREA_W    80

/* Background */
#define DESKTOP_BG      THEME_BG

#endif /* CITADEL_THEME_H */
