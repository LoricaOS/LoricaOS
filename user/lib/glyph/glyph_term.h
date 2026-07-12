/* glyph_term.h -- terminal emulator core for the Glyph toolkit
 *
 * Extracted from user/bin/lumen/terminal.c (Phase 47b subsystem peeling)
 * so the in-process Lumen dropdown terminal and the standalone
 * /bin/terminal external-protocol client share one emulator.
 *
 * The core is window-system agnostic: it owns the cell grid, the
 * ANSI/CSI escape parser, the scrollback ring, selection state and the
 * scrollbar, and renders into a caller-supplied surface_t at a
 * (pad_x, pad_y) offset. Callers own dirty-marking and presentation.
 * Mouse/scroll helpers return 1 when the visual state changed so the
 * caller knows to repaint.
 */
#ifndef GLYPH_TERM_H
#define GLYPH_TERM_H

#include "draw.h"
#include <stdint.h>

#define GLYPH_TERM_SCROLLBACK 500  /* lines of history above visible area */
#define GLYPH_TERM_SB_WIDTH   8    /* scrollbar width in pixels */

#define GLYPH_TERM_DEFAULT_COLOR 0xFFFFFFFFU

#define GLYPH_TERM_ATTR_BOLD      0x01
#define GLYPH_TERM_ATTR_UNDERLINE 0x04
#define GLYPH_TERM_ATTR_REVERSE   0x08

/* Cursor shapes (glyph_term_set_cursor_style). */
#define GLYPH_TERM_CURSOR_BLOCK     0
#define GLYPH_TERM_CURSOR_BEAM      1
#define GLYPH_TERM_CURSOR_UNDERLINE 2

/* Color scheme: 16 ANSI colors + defaults. A field == GLYPH_TERM_DYN means
 * "follow the live desktop theme" (used by the default scheme so it stays
 * frosted/light-dark aware); anything else is a fixed 0x00RRGGBB. */
#define GLYPH_TERM_DYN 0xFFFFFFFEu
typedef struct {
    const char *name;
    uint32_t ansi[16];
    uint32_t fg, bg, cursor, selection;
} glyph_term_theme_t;

typedef struct {
    char ch;
    uint32_t fg;    /* 0xRRGGBB or GLYPH_TERM_DEFAULT_COLOR */
    uint32_t bg;    /* 0xRRGGBB or GLYPH_TERM_DEFAULT_COLOR */
    uint8_t  attrs; /* GLYPH_TERM_ATTR_* */
} glyph_term_cell_t;

typedef struct {
    int master_fd;  /* PTY master, -1 if PTY failed (caller sets) */
    int cols, rows;
    int cx, cy;     /* cursor position (relative to visible area) */
    int cell_w, cell_h; /* character cell size (TTF or bitmap fallback) */
    glyph_term_cell_t *grid;  /* ring buffer: total_rows * cols */
    uint32_t cur_fg, cur_bg;  /* current SGR colors */
    uint8_t cur_attrs;        /* current GLYPH_TERM_ATTR_* flags */
    int total_rows; /* rows + GLYPH_TERM_SCROLLBACK */
    int scroll_top; /* index of first visible line in ring buffer */
    int scroll_offset; /* how far user has scrolled back (0 = bottom) */
    /* ANSI escape parser state */
    int esc_state;      /* 0=normal, 1=got ESC, 2=got CSI (ESC[) */
    int esc_params[8];
    int esc_nparam;
    int esc_private;    /* 1 if '?' seen after CSI */
    /* Cursor visibility (DEC ?25) */
    int cursor_visible;
    /* Alternate screen buffer (DEC ?1049) */
    int saved_cx, saved_cy;
    /* Selection state */
    int sel_active;     /* 1 if selection in progress */
    int sel_start_col, sel_start_row;  /* start (visible grid coords) */
    int sel_end_col, sel_end_row;      /* end (visible grid coords) */
    int sel_done;       /* 1 if selection completed (mouse released) */
    /* Padding offsets (grid origin within the surface) */
    int pad_x, pad_y;
    /* Scrollbar drag state */
    int sb_dragging;
    /* OSC (ESC ]) string parser buffer + length (collected until BEL/ST) */
    char osc_buf[64];
    int  osc_len;
    /* Admin-session state, set by the private OSC `]777;admin;<n>`. The
     * consumer (terminal app) polls glyph_term_admin_take_change() and
     * reports the flag to Lumen so it tints the window titlebar red. */
    int  admin;
    int  admin_changed;
    /* Runtime-configurable settings (defaults set by glyph_term_create). */
    const glyph_term_theme_t *theme;  /* active color scheme */
    int  cursor_style;   /* GLYPH_TERM_CURSOR_* */
    int  cursor_blink;   /* 1 = blink, 0 = steady */
    int  font_px;        /* glyph size used to render cells */
    int  scrollback;     /* history lines above the visible area */
} glyph_term_t;

/* Lifecycle */
glyph_term_t *glyph_term_create(int cols, int rows, int cell_w, int cell_h,
                                int pad_x, int pad_y);
void glyph_term_destroy(glyph_term_t *t);

/* Resize the grid to new cols×rows with new cell metrics (font-size change or
 * window resize). Reallocs the ring buffer preserving the top-left overlap,
 * clamps the cursor, and pushes the new size to the PTY via TIOCSWINSZ.
 * Returns 0 on success, -1 on alloc failure (grid unchanged). */
int glyph_term_resize(glyph_term_t *t, int cols, int rows,
                      int cell_w, int cell_h, int font_px);

/* Runtime settings. set_theme takes a built-in scheme index (clamped). */
void glyph_term_set_theme(glyph_term_t *t, int scheme);
void glyph_term_set_cursor_style(glyph_term_t *t, int style);
void glyph_term_set_cursor_blink(glyph_term_t *t, int on);
/* Push current cols/rows to the PTY (TIOCSWINSZ). Call once after setting
 * master_fd; resize re-applies it automatically. */
void glyph_term_apply_winsize(glyph_term_t *t);

/* Built-in color scheme registry (for a settings picker). preview returns a
 * representative background color for a swatch. */
int         glyph_term_theme_count(void);
const char *glyph_term_theme_name(int scheme);
uint32_t    glyph_term_theme_preview(int scheme);

/* Output path: feed PTY bytes through the escape parser into the grid.
 * Caller marks dirty / presents afterwards. */
void glyph_term_feed(glyph_term_t *t, const char *data, int len);

/* Write a literal message into the grid (no escape parsing) — used for
 * "PTY unavailable" style diagnostics. */
void glyph_term_puts(glyph_term_t *t, const char *msg);

/* Render the grid (+ cursor + selection + scrollbar) into s. The
 * (clear_x, clear_y, clear_w, clear_h) rect is filled with the terminal
 * background first; the grid is drawn at (pad_x, pad_y). */
void glyph_term_render(glyph_term_t *t, surface_t *s,
                       int clear_x, int clear_y, int clear_w, int clear_h);

/* Mouse in surface-local pixels. Return 1 if a repaint is needed. */
int glyph_term_mouse_down(glyph_term_t *t, int x, int y);
int glyph_term_mouse_move(glyph_term_t *t, int x, int y);
int glyph_term_mouse_up(glyph_term_t *t, int x, int y);
int glyph_term_scroll(glyph_term_t *t, int direction); /* +1 up, -1 down */

/* Selection / scrollback */
int  glyph_term_has_selection(glyph_term_t *t);
int  glyph_term_copy_selection(glyph_term_t *t, char *buf, int max);
void glyph_term_clear_selection(glyph_term_t *t);
void glyph_term_scroll_back(glyph_term_t *t, int lines);

/* Key → PTY byte translation. Lumen folds 3-byte CSI arrow sequences
 * into single synthetic bytes for proxy windows (0xF1=Up 0xF2=Down
 * 0xF3=Right 0xF4=Left — see lumen/main.c). Re-expand them to VT
 * sequences for the PTY; everything else passes through as one byte.
 * out must hold >= 4 bytes; returns the byte count. */
int glyph_term_translate_key(unsigned char key, char *out);

/* Admin-session change poll. Returns 1 if the admin state changed since
 * the last call (and writes the new state to *admin_out), else 0. Lets the
 * terminal app notice an `admin`/`deadmin` in its shell and tell Lumen. */
int glyph_term_admin_take_change(glyph_term_t *t, int *admin_out);

/* Open a PTY pair and spawn the standard shell (/bin/stsh via
 * sys_spawn, stdio on the slave). Returns master_fd (O_NONBLOCK) or -1.
 * fail_reason_out / shell_pid_out may be NULL. */
int glyph_pty_open_and_spawn(const char **fail_reason_out,
                             int *shell_pid_out);

#endif /* GLYPH_TERM_H */
