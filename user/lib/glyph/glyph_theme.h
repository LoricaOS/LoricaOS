/* theme.h — Aegis design language: the single source of truth.
 *
 * Phase 1 of the GUI beautification program ("one material"). Every Glyph
 * widget, every Lumen/Citadel/Bastion surface, and every /apps bundle should
 * derive its colors, spacing, radii and type sizes from THESE tokens — never
 * from inline hex literals or a private per-component palette.
 *
 * Naming: tokens are SEMANTIC (THEME_SURFACE, THEME_ACCENT, THEME_TEXT_DIM),
 * not raw color names, so a future light theme is a one-file change.
 *
 * draw.h includes this header and re-defines its legacy C_* names as aliases
 * to these tokens, so existing toolkit code keeps working unchanged while the
 * palette collapses to one definition. New/edited code should prefer THEME_*.
 *
 * Pixel format is 0x00RRGGBB (XRGB, top byte ignored by the framebuffer).
 *
 * LOAD-BEARING EXCEPTIONS — do NOT route these through the theme:
 *   - The compositor color-keys frosted windows on 0x000A0A14 and window
 *     chrome on 0x00080810. Those two values are a protocol contract between
 *     Glyph and Lumen, not palette choices. They live as THEME_KEY_* below for
 *     reference but their numeric values must never change.
 */
#ifndef GLYPH_THEME_H
#define GLYPH_THEME_H

#include <stdint.h>

/* ── RUNTIME theme state ──────────────────────────────────────────────────
 * The surface/text/border tokens AND the accent are now runtime values, so a
 * light theme (and the accent picker) is a live, persisted switch rather than
 * a recompile. The token macros below read g_glyph_theme — they are
 * expressions, not constants, so do NOT use them in static initializers or
 * case labels (store a role/index and resolve at draw time instead).
 *
 * theme.c owns g_glyph_theme, sets the palette fields from `light`, and loads
 * everything from config at startup (glyph_theme_load, via font_init). */
typedef struct glyph_theme {
    /* Accent (user-selectable). */
    uint32_t accent, accent_hover, accent_active;

    /* Runtime preferences (persisted in theme.conf). */
    int      animations;      /* 1 = window open/effect animations on  */
    int      clock_24h;       /* 1 = 24-hour clock, 0 = 12-hour AM/PM  */
    int      natural_scroll;  /* 1 = invert mouse wheel direction      */
    int      tz_offset_min;   /* timezone offset from UTC, in minutes  */
    int      light;           /* 0 = dark theme, 1 = light theme       */
    int      pointer_speed;   /* cursor speed, percent (100/150/250)   */
    int      wallpaper;       /* desktop background preset index       */
    int      night_light;     /* 1 = warm color cast on the output     */
    int      ntp_auto;        /* 1 = chronos may sync time (NTP)        */

    /* Palette — derived from `light` (set by theme.c, never edited directly). */
    uint32_t bg, surface, surface_2, hover, input_bg;
    uint32_t desktop_top, desktop_bot;
    uint32_t border, border_strong;
    uint32_t text, text_dim, text_faint, text_on_accent;
} glyph_theme_t;

extern glyph_theme_t g_glyph_theme;

/* ── Surfaces ──────────────────────────────────────────────────────────────
 * BG: deepest chrome (top bar). SURFACE: window/card body. SURFACE_2: raised
 * (titlebar/menu/dock/panel). HOVER: row hover. INPUT_BG: inset fields. */
#define THEME_BG            (g_glyph_theme.bg)
#define THEME_SURFACE       (g_glyph_theme.surface)
#define THEME_SURFACE_2     (g_glyph_theme.surface_2)
#define THEME_HOVER         (g_glyph_theme.hover)
#define THEME_INPUT_BG      (g_glyph_theme.input_bg)

/* Desktop backdrop gradient (used when no wallpaper image is loaded). */
#define THEME_DESKTOP_TOP   (g_glyph_theme.desktop_top)
#define THEME_DESKTOP_BOT   (g_glyph_theme.desktop_bot)

/* ── Borders / separators ────────────────────────────────────────────────── */
#define THEME_BORDER        (g_glyph_theme.border)
#define THEME_BORDER_STRONG (g_glyph_theme.border_strong)

/* ── Text ────────────────────────────────────────────────────────────────── */
#define THEME_TEXT          (g_glyph_theme.text)
#define THEME_TEXT_DIM      (g_glyph_theme.text_dim)
#define THEME_TEXT_FAINT    (g_glyph_theme.text_faint)
#define THEME_TEXT_ON_ACCENT (g_glyph_theme.text_on_accent)

/* ── Accent ──────────────────────────────────────────────────────────────── */
#define THEME_ACCENT        (g_glyph_theme.accent)
#define THEME_ACCENT_HOVER  (g_glyph_theme.accent_hover)
#define THEME_ACCENT_ACTIVE (g_glyph_theme.accent_active)
#define THEME_SELECTION     (g_glyph_theme.accent)  /* selected row / tab / item */

/* Runtime theme API (implemented in theme.c). */
int         glyph_theme_load(void);            /* apply config; 1 if any applied */
int         glyph_theme_save(void);            /* persist current accent (per-user) */
void        glyph_theme_set_accent(int index); /* apply preset by index (live) */
int         glyph_theme_accent_count(void);
const char *glyph_theme_accent_name(int index);
uint32_t    glyph_theme_accent_color(int index);  /* 0x00RRGGBB */
int         glyph_theme_current_accent(void);     /* current preset index, or -1 */

/* Runtime preferences (persisted alongside the accent in theme.conf).
 * Setters change the live value only; call glyph_theme_save() to persist. */
int         glyph_theme_animations(void);          /* 1 = window animations on */
void        glyph_theme_set_animations(int on);
int         glyph_theme_clock24(void);             /* 1 = 24-hour clock        */
void        glyph_theme_set_clock24(int on);
int         glyph_theme_natural_scroll(void);      /* 1 = inverted wheel       */
void        glyph_theme_set_natural_scroll(int on);
int         glyph_theme_tz_offset(void);           /* UTC offset, minutes      */
void        glyph_theme_set_tz_offset(int minutes);
int         glyph_theme_light(void);               /* 1 = light theme          */
void        glyph_theme_set_light(int on);         /* applies the palette live */
int         glyph_theme_pointer_speed(void);       /* percent (100/150/250)    */
void        glyph_theme_set_pointer_speed(int pct);
int         glyph_theme_wallpaper(void);           /* preset index             */
void        glyph_theme_set_wallpaper(int idx);
int         glyph_theme_wallpaper_count(void);
const char *glyph_theme_wallpaper_name(int idx);
int         glyph_theme_night_light(void);         /* 1 = warm cast            */
void        glyph_theme_set_night_light(int on);
int         glyph_theme_ntp_auto(void);            /* 1 = NTP sync allowed     */
void        glyph_theme_set_ntp_auto(int on);

/* Re-read the runtime prefs (not accent) from config + reapply the palette.
 * Lets a running compositor pick up Settings changes without a restart. */
void        glyph_theme_reload_prefs(void);

/* ── Status ──────────────────────────────────────────────────────────────── */
#define THEME_OK            0x0030C85A  /* success                              */
#define THEME_WARN          0x00E8B23E  /* warning                              */
#define THEME_ERROR         0x00FF5F57  /* error (matches traffic-light red)     */

/* ── Window traffic lights (macOS-canonical; keep exact) ─────────────────── */
#define THEME_TL_RED        0x00FF5F57
#define THEME_TL_YELLOW     0x00FEBC2E
#define THEME_TL_GREEN      0x0028C840

/* ── Compositor color-keys — CONTRACT VALUES, never re-tune ──────────────── */
#define THEME_KEY_FROST     0x000A0A14  /* frosted-window client key (== C_TERM_BG) */
#define THEME_KEY_SHADOW    0x00080810  /* chrome transparency key   (== C_SHADOW)  */

/* ── Spacing scale (4px grid) ────────────────────────────────────────────── */
#define SP_1   4
#define SP_2   8
#define SP_3   12
#define SP_4   16
#define SP_5   24
#define SP_6   32

/* ── Corner radius scale ─────────────────────────────────────────────────── */
#define R_SM   6   /* buttons, inputs, chips, checkboxes       */
#define R_MD   10  /* cards, menus, modals, window corners     */
#define R_LG   14  /* large surfaces                            */

/* ── Type scale (TTF point sizes; font auto-bakes any size) ──────────────── */
#define TYPE_CAPTION  12  /* captions, secondary labels  */
#define TYPE_BODY     14  /* default UI body text        */
#define TYPE_TITLE    16  /* section / window titles     */
#define TYPE_DISPLAY  20  /* prominent headings          */

#endif /* GLYPH_THEME_H */
