/* glyph_term.c -- terminal emulator core for the Glyph toolkit
 *
 * Extracted verbatim from user/bin/lumen/terminal.c (Phase 47b subsystem
 * peeling) so the Lumen dropdown terminal and the standalone
 * /bin/terminal client share one implementation. See glyph_term.h.
 *
 * The shell is spawned with sys_spawn (syscall 514), not fork: fork from
 * a large process (lumen is ~3000 pages) takes 30+ seconds on bare metal
 * due to eager page copy + vmm_window_lock contention. sys_spawn creates
 * a fresh PML4 with the ELF loaded directly — no page copy.
 */

#include "glyph_term.h"
#include "font.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <termios.h>

/* sys_spawn: create process from ELF path without fork.
 * syscall(514, path, argv, envp, stdio_fd, cap_mask) */
#define SYS_SPAWN 514

/* Short aliases for the moved code */
#define SCROLLBACK_LINES   GLYPH_TERM_SCROLLBACK
#define SB_WIDTH           GLYPH_TERM_SB_WIDTH
#define TERM_DEFAULT_COLOR GLYPH_TERM_DEFAULT_COLOR
#define ATTR_BOLD          GLYPH_TERM_ATTR_BOLD
#define ATTR_UNDERLINE     GLYPH_TERM_ATTR_UNDERLINE
#define ATTR_REVERSE       GLYPH_TERM_ATTR_REVERSE

#define SEL_HIGHLIGHT 0x004488CC  /* selection highlight color */
#define SEL_ALPHA     80          /* highlight opacity (0-255) */

#define SB_THUMB_COLOR  0x00606878
#define SB_THUMB_HI     0x00808898

/* ANSI 8-color palette (standard + bright) */
static const uint32_t ansi_colors[16] = {
    0x000000, 0xCC0000, 0x00CC00, 0xCCAA00,  /* black, red, green, yellow */
    0x4488CC, 0xCC00CC, 0x00CCCC, 0xCCCCCC,  /* blue, magenta, cyan, white */
    0x555555, 0xFF5555, 0x55FF55, 0xFFFF55,  /* bright variants */
    0x5599FF, 0xFF55FF, 0x55FFFF, 0xFFFFFF,
};

/* xterm-256color palette: 0-15 standard, 16-231 6x6x6 cube, 232-255 gray */
static uint32_t ansi_256[256];
static int ansi_256_inited;

static void init_ansi_256(void)
{
    for (int i = 0; i < 16; i++)
        ansi_256[i] = ansi_colors[i];
    for (int i = 0; i < 216; i++) {
        int r = (i / 36) * 51;
        int g = ((i / 6) % 6) * 51;
        int b = (i % 6) * 51;
        ansi_256[16 + i] = (uint32_t)((r << 16) | (g << 8) | b);
    }
    for (int i = 0; i < 24; i++) {
        int v = 8 + i * 10;
        ansi_256[232 + i] = (uint32_t)((v << 16) | (v << 8) | v);
    }
}

/* Built-in color schemes. Scheme 0 ("Lorica") uses GLYPH_TERM_DYN for its
 * defaults so it follows the live desktop theme and keeps the frosted-glass
 * background key; the rest are fixed opaque palettes. */
static const glyph_term_theme_t s_term_themes[] = {
  { "Lorica",
    { 0x000000,0xCC0000,0x00CC00,0xCCAA00,0x4488CC,0xCC00CC,0x00CCCC,0xCCCCCC,
      0x555555,0xFF5555,0x55FF55,0xFFFF55,0x5599FF,0xFF55FF,0x55FFFF,0xFFFFFF },
    GLYPH_TERM_DYN, GLYPH_TERM_DYN, GLYPH_TERM_DYN, 0x004488CC },
  { "Solarized Dark",
    { 0x073642,0xdc322f,0x859900,0xb58900,0x268bd2,0xd33682,0x2aa198,0xeee8d5,
      0x002b36,0xcb4b16,0x586e75,0x657b83,0x839496,0x6c71c4,0x93a1a1,0xfdf6e3 },
    0x839496, 0x002b36, 0x839496, 0x073642 },
  { "Solarized Light",
    { 0x073642,0xdc322f,0x859900,0xb58900,0x268bd2,0xd33682,0x2aa198,0xeee8d5,
      0x002b36,0xcb4b16,0x586e75,0x657b83,0x839496,0x6c71c4,0x93a1a1,0xfdf6e3 },
    0x657b83, 0xfdf6e3, 0x657b83, 0xeee8d5 },
  { "Gruvbox Dark",
    { 0x282828,0xcc241d,0x98971a,0xd79921,0x458588,0xb16286,0x689d6a,0xa89984,
      0x928374,0xfb4934,0xb8bb26,0xfabd2f,0x83a598,0xd3869b,0x8ec07c,0xebdbb2 },
    0xebdbb2, 0x282828, 0xebdbb2, 0x504945 },
  { "Nord",
    { 0x3b4252,0xbf616a,0xa3be8c,0xebcb8b,0x81a1c1,0xb48ead,0x88c0d0,0xe5e9f0,
      0x4c566a,0xbf616a,0xa3be8c,0xebcb8b,0x81a1c1,0xb48ead,0x8fbcbb,0xeceff4 },
    0xd8dee9, 0x2e3440, 0xd8dee9, 0x434c5e },
  { "Dracula",
    { 0x21222c,0xff5555,0x50fa7b,0xf1fa8c,0xbd93f9,0xff79c6,0x8be9fd,0xf8f8f2,
      0x6272a4,0xff6e6e,0x69ff94,0xffffa5,0xd6acff,0xff92df,0xa4ffff,0xffffff },
    0xf8f8f2, 0x282a36, 0xf8f8f2, 0x44475a },
};
#define TERM_NTHEMES ((int)(sizeof(s_term_themes)/sizeof(s_term_themes[0])))

/* Palette-indexed color marker: cells store TERM_PAL(idx) for ANSI colors so a
 * theme switch recolors existing text; 0x00RRGGBB = truecolor; DEFAULT = theme. */
#define TERM_PAL(idx) (0x01000000u | (uint32_t)(idx))

static uint32_t term_fg(glyph_term_t *tp)
{ uint32_t f = tp->theme->fg; return f == GLYPH_TERM_DYN ? C_TERM_FG : f; }
static uint32_t term_bg(glyph_term_t *tp)
{ uint32_t b = tp->theme->bg; return b == GLYPH_TERM_DYN ? C_TERM_BG : b; }
static uint32_t term_cursor(glyph_term_t *tp)
{ uint32_t c = tp->theme->cursor; return c == GLYPH_TERM_DYN ? C_TERM_FG : c; }

/* Resolve a stored cell color to concrete RGB through the active theme. */
static uint32_t term_resolve(glyph_term_t *tp, uint32_t v, int is_bg)
{
    if (v == TERM_DEFAULT_COLOR)
        return is_bg ? term_bg(tp) : term_fg(tp);
    if ((v & 0xFF000000u) == 0x01000000u) {
        int idx = (int)(v & 0xFFu);
        return idx < 16 ? tp->theme->ansi[idx] : ansi_256[idx];
    }
    return v & 0x00FFFFFFu;
}

/* Access a cell in the ring buffer.
 * vis_row is 0..rows-1 (top of visible area = 0).
 * scroll_offset shifts the view up into history. */
static inline glyph_term_cell_t *
grid_cell(glyph_term_t *tp, int vis_row, int col)
{
    int ring_row = (tp->scroll_top - tp->scroll_offset + vis_row) % tp->total_rows;
    if (ring_row < 0)
        ring_row += tp->total_rows;
    return &tp->grid[ring_row * tp->cols + col];
}

/* Access a cell at the cursor's absolute position (ignoring scroll_offset).
 * Used by the output path which always writes at the bottom. */
static inline glyph_term_cell_t *
grid_cell_abs(glyph_term_t *tp, int vis_row, int col)
{
    int ring_row = (tp->scroll_top + vis_row) % tp->total_rows;
    if (ring_row < 0)
        ring_row += tp->total_rows;
    return &tp->grid[ring_row * tp->cols + col];
}

static void term_scroll(glyph_term_t *tp)
{
    tp->scroll_top = (tp->scroll_top + 1) % tp->total_rows;
    int new_bottom = (tp->scroll_top + tp->rows - 1) % tp->total_rows;
    glyph_term_cell_t *row = &tp->grid[new_bottom * tp->cols];
    for (int i = 0; i < tp->cols; i++)
        row[i] = (glyph_term_cell_t){' ', TERM_DEFAULT_COLOR, TERM_DEFAULT_COLOR, 0};
    tp->cy = tp->rows - 1;
}

/* Normalize selection so start is before end (row-major) */
static void
sel_normalize(glyph_term_t *tp, int *sr, int *sc, int *er, int *ec)
{
    int s_r = tp->sel_start_row, s_c = tp->sel_start_col;
    int e_r = tp->sel_end_row, e_c = tp->sel_end_col;

    if (s_r > e_r || (s_r == e_r && s_c > e_c)) {
        *sr = e_r; *sc = e_c;
        *er = s_r; *ec = s_c;
    } else {
        *sr = s_r; *sc = s_c;
        *er = e_r; *ec = e_c;
    }
}

/* Check if a cell (r, c) is within a pre-normalized selection range */
static inline int
cell_in_sel(int r, int c, int sr, int sc, int er, int ec)
{
    if (r < sr || r > er)
        return 0;
    if (r == sr && r == er)
        return c >= sc && c <= ec;
    if (r == sr)
        return c >= sc;
    if (r == er)
        return c <= ec;
    return 1; /* middle row, fully selected */
}

void
glyph_term_render(glyph_term_t *tp, surface_t *s,
                  int clear_x, int clear_y, int clear_w, int clear_h)
{
    int ox = tp->pad_x;
    int oy = tp->pad_y;
    int cw = tp->cell_w;
    int ch = tp->cell_h;

    /* Clear the caller-specified background rect */
    uint32_t base_bg = term_bg(tp);
    draw_fill_rect(s, clear_x, clear_y, clear_w, clear_h, base_bg);

    /* Pre-normalize selection once (not per-cell) */
    int has_sel = tp->sel_active;
    int sr = 0, sc = 0, er = 0, ec = 0;
    if (has_sel)
        sel_normalize(tp, &sr, &sc, &er, &ec);

    for (int r = 0; r < tp->rows; r++) {
        /* Quick check: skip entirely blank, unselected rows */
        glyph_term_cell_t *row_start = grid_cell(tp, r, 0);
        int row_has_content = 0;
        int row_has_sel = has_sel && r >= sr && r <= er;

        if (!row_has_sel) {
            for (int c = 0; c < tp->cols; c++) {
                if (row_start[c].ch != ' ' || row_start[c].bg != TERM_DEFAULT_COLOR) {
                    row_has_content = 1;
                    break;
                }
            }
            if (!row_has_content)
                continue;
        }

        for (int c = 0; c < tp->cols; c++) {
            glyph_term_cell_t *cell = grid_cell(tp, r, c);
            int px = ox + c * cw;
            int py = oy + r * ch;

            /* Resolve colors through the active theme */
            uint32_t fg = term_resolve(tp, cell->fg, 0);
            uint32_t bg = term_resolve(tp, cell->bg, 1);

            /* Apply reverse video */
            if (cell->attrs & ATTR_REVERSE) {
                uint32_t tmp = fg; fg = bg; bg = tmp;
            }

            /* Draw cell background if it differs from the base fill */
            if (bg != base_bg)
                draw_fill_rect(s, px, py, cw, ch, bg);

            if (row_has_sel && cell_in_sel(r, c, sr, sc, er, ec))
                draw_blend_rect(s, px, py, cw, ch, tp->theme->selection, SEL_ALPHA);

            if (cell->ch != ' ' || (cell->attrs & ATTR_UNDERLINE)) {
                if (cell->ch != ' ') {
                    if (g_font_mono)
                        font_draw_char(s, g_font_mono, tp->font_px, px, py, cell->ch, fg);
                    else
                        draw_char(s, px, py, cell->ch, fg, base_bg);
                }
                /* Underline: 1px line at bottom of cell */
                if (cell->attrs & ATTR_UNDERLINE)
                    draw_fill_rect(s, px, py + ch - 1, cw, 1, fg);
            }
        }
    }

    /* Cursor (only when viewing the bottom). Shape per cursor_style; blink uses
     * the wall clock so it toggles even when idle (steady if blink is off). */
    if (tp->master_fd >= 0 && tp->scroll_offset == 0 && tp->cursor_visible) {
        int on = 1;
        if (tp->cursor_blink) {
            struct timespec now;
            clock_gettime(CLOCK_REALTIME, &now);
            on = !((int)(now.tv_nsec / 500000000L) & 1); /* 500ms toggle */
        }
        if (on) {
            int px = ox + tp->cx * cw, py = oy + tp->cy * ch;
            uint32_t cc = term_cursor(tp);
            if (tp->cursor_style == GLYPH_TERM_CURSOR_BEAM)
                draw_fill_rect(s, px, py, 2, ch, cc);
            else if (tp->cursor_style == GLYPH_TERM_CURSOR_UNDERLINE)
                draw_fill_rect(s, px, py + ch - 2, cw, 2, cc);
            else
                draw_fill_rect(s, px, py, cw, ch, cc);
        }
    }

    /* Scrollbar — only visible when scrolled back or dragging */
    if (tp->scroll_offset > 0 || tp->sb_dragging) {
        int track_h = tp->rows * ch;
        int track_x = ox + tp->cols * cw;  /* right edge of text area */
        int track_y = oy;

        /* Subtle track background */
        draw_blend_rect(s, track_x, track_y, SB_WIDTH, track_h, 0x00000000, 30);

        /* Thumb: size proportional to visible/total, position from offset */
        int total = tp->scrollback + tp->rows;
        int thumb_h = tp->rows * track_h / total;
        if (thumb_h < 16) thumb_h = 16;
        /* scroll_offset=0 → thumb at bottom; scroll_offset=max → top */
        int usable = track_h - thumb_h;
        int thumb_y = track_y + usable - (usable * tp->scroll_offset /
                                          (tp->scrollback ? tp->scrollback : 1));

        uint32_t tc = tp->sb_dragging ? SB_THUMB_HI : SB_THUMB_COLOR;
        draw_fill_rect(s, track_x + 1, thumb_y, SB_WIDTH - 2, thumb_h, tc);
    }
}

void
glyph_term_puts(glyph_term_t *tp, const char *msg)
{
    while (*msg) {
        unsigned char ch = (unsigned char)*msg++;
        if (ch == '\n') {
            tp->cy++;
            tp->cx = 0;
            if (tp->cy >= tp->rows)
                term_scroll(tp);
        } else if (ch >= 32 && ch <= 126) {
            glyph_term_cell_t *cell = grid_cell_abs(tp, tp->cy, tp->cx);
            cell->ch = (char)ch;
            cell->fg = TERM_DEFAULT_COLOR;
            cell->bg = TERM_DEFAULT_COLOR;
            cell->attrs = 0;
            tp->cx++;
            if (tp->cx >= tp->cols) {
                tp->cx = 0;
                tp->cy++;
                if (tp->cy >= tp->rows)
                    term_scroll(tp);
            }
        }
    }
}

/* Handle a completed CSI sequence: ESC [ params... final_char */
static void term_csi(glyph_term_t *tp, int final)
{
    int n = (tp->esc_nparam > 0) ? tp->esc_params[0] : 0;

    switch (final) {
    case 'A': /* CUU -- cursor up */
        if (n < 1) n = 1;
        tp->cy -= n;
        if (tp->cy < 0) tp->cy = 0;
        break;
    case 'B': /* CUD -- cursor down */
        if (n < 1) n = 1;
        tp->cy += n;
        if (tp->cy >= tp->rows) tp->cy = tp->rows - 1;
        break;
    case 'C': /* CUF -- cursor forward */
        if (n < 1) n = 1;
        tp->cx += n;
        if (tp->cx >= tp->cols) tp->cx = tp->cols - 1;
        break;
    case 'D': /* CUB -- cursor back */
        if (n < 1) n = 1;
        tp->cx -= n;
        if (tp->cx < 0) tp->cx = 0;
        break;
    case 'H': /* CUP -- cursor position */
    case 'f': {
        int row = (tp->esc_nparam > 0) ? tp->esc_params[0] : 1;
        int col = (tp->esc_nparam > 1) ? tp->esc_params[1] : 1;
        if (row < 1) row = 1;
        if (col < 1) col = 1;
        tp->cy = row - 1;
        tp->cx = col - 1;
        if (tp->cy >= tp->rows) tp->cy = tp->rows - 1;
        if (tp->cx >= tp->cols) tp->cx = tp->cols - 1;
        break;
    }
    case 'J': /* ED -- erase in display */
        {
            glyph_term_cell_t blank = {' ', TERM_DEFAULT_COLOR, TERM_DEFAULT_COLOR, 0};
            if (n == 2) {
                for (int r = 0; r < tp->rows; r++)
                    for (int c2 = 0; c2 < tp->cols; c2++)
                        *grid_cell_abs(tp, r, c2) = blank;
                tp->cx = 0;
                tp->cy = 0;
            } else if (n == 0) {
                for (int c2 = tp->cx; c2 < tp->cols; c2++)
                    *grid_cell_abs(tp, tp->cy, c2) = blank;
                for (int r = tp->cy + 1; r < tp->rows; r++)
                    for (int c2 = 0; c2 < tp->cols; c2++)
                        *grid_cell_abs(tp, r, c2) = blank;
            }
        }
        break;
    case 'K': /* EL -- erase in line */
        {
            glyph_term_cell_t blank = {' ', TERM_DEFAULT_COLOR, TERM_DEFAULT_COLOR, 0};
            if (n == 0) {
                for (int c2 = tp->cx; c2 < tp->cols; c2++)
                    *grid_cell_abs(tp, tp->cy, c2) = blank;
            } else if (n == 2) {
                for (int c2 = 0; c2 < tp->cols; c2++)
                    *grid_cell_abs(tp, tp->cy, c2) = blank;
            }
        }
        break;
    case 'm': /* SGR -- set graphic rendition (colors) */
        if (tp->esc_nparam == 0) {
            tp->cur_fg = TERM_DEFAULT_COLOR;
            tp->cur_bg = TERM_DEFAULT_COLOR;
            tp->cur_attrs = 0;
        }
        for (int pi = 0; pi < tp->esc_nparam; pi++) {
            int p = tp->esc_params[pi];
            if (p == 0) {
                tp->cur_fg = TERM_DEFAULT_COLOR;
                tp->cur_bg = TERM_DEFAULT_COLOR;
                tp->cur_attrs = 0;
            }
            else if (p == 1) tp->cur_attrs |= ATTR_BOLD;
            else if (p == 4) tp->cur_attrs |= ATTR_UNDERLINE;
            else if (p == 7) tp->cur_attrs |= ATTR_REVERSE;
            else if (p == 22) tp->cur_attrs &= (uint8_t)~ATTR_BOLD;
            else if (p == 24) tp->cur_attrs &= (uint8_t)~ATTR_UNDERLINE;
            else if (p == 27) tp->cur_attrs &= (uint8_t)~ATTR_REVERSE;
            else if (p >= 30 && p <= 37) {
                int idx = p - 30;
                if (tp->cur_attrs & ATTR_BOLD) idx += 8;
                tp->cur_fg = TERM_PAL(idx);
            }
            else if (p == 39) tp->cur_fg = TERM_DEFAULT_COLOR;
            else if (p >= 40 && p <= 47) tp->cur_bg = TERM_PAL(p - 40);
            else if (p == 49) tp->cur_bg = TERM_DEFAULT_COLOR;
            else if (p >= 90 && p <= 97) tp->cur_fg = TERM_PAL(p - 90 + 8);
            else if (p >= 100 && p <= 107) tp->cur_bg = TERM_PAL(p - 100 + 8);
            /* 256-color: 38;5;N (fg) / 48;5;N (bg) */
            else if (p == 38 && pi + 2 < tp->esc_nparam
                     && tp->esc_params[pi + 1] == 5) {
                int idx = tp->esc_params[pi + 2];
                if (idx >= 0 && idx < 256)
                    tp->cur_fg = TERM_PAL(idx);
                pi += 2;
            }
            else if (p == 48 && pi + 2 < tp->esc_nparam
                     && tp->esc_params[pi + 1] == 5) {
                int idx = tp->esc_params[pi + 2];
                if (idx >= 0 && idx < 256)
                    tp->cur_bg = TERM_PAL(idx);
                pi += 2;
            }
            /* Truecolor: 38;2;R;G;B (fg) / 48;2;R;G;B (bg) */
            else if (p == 38 && pi + 4 < tp->esc_nparam
                     && tp->esc_params[pi + 1] == 2) {
                int r = tp->esc_params[pi + 2] & 0xFF;
                int g = tp->esc_params[pi + 3] & 0xFF;
                int b = tp->esc_params[pi + 4] & 0xFF;
                tp->cur_fg = (uint32_t)((r << 16) | (g << 8) | b);
                pi += 4;
            }
            else if (p == 48 && pi + 4 < tp->esc_nparam
                     && tp->esc_params[pi + 1] == 2) {
                int r = tp->esc_params[pi + 2] & 0xFF;
                int g = tp->esc_params[pi + 3] & 0xFF;
                int b = tp->esc_params[pi + 4] & 0xFF;
                tp->cur_bg = (uint32_t)((r << 16) | (g << 8) | b);
                pi += 4;
            }
        }
        break;
    case 'h': /* SM -- set mode */
        if (tp->esc_private) {
            if (n == 25) tp->cursor_visible = 1;
            else if (n == 1049) {
                /* Save cursor + clear screen (alternate screen) */
                tp->saved_cx = tp->cx;
                tp->saved_cy = tp->cy;
                glyph_term_cell_t blank = {' ', TERM_DEFAULT_COLOR, TERM_DEFAULT_COLOR, 0};
                for (int r = 0; r < tp->rows; r++)
                    for (int c2 = 0; c2 < tp->cols; c2++)
                        *grid_cell_abs(tp, r, c2) = blank;
                tp->cx = 0;
                tp->cy = 0;
            }
        }
        break;
    case 'l': /* RM -- reset mode */
        if (tp->esc_private) {
            if (n == 25) tp->cursor_visible = 0;
            else if (n == 1049) {
                /* Restore cursor + clear screen (leave alternate screen) */
                glyph_term_cell_t blank = {' ', TERM_DEFAULT_COLOR, TERM_DEFAULT_COLOR, 0};
                for (int r = 0; r < tp->rows; r++)
                    for (int c2 = 0; c2 < tp->cols; c2++)
                        *grid_cell_abs(tp, r, c2) = blank;
                tp->cx = tp->saved_cx;
                tp->cy = tp->saved_cy;
            }
        }
        break;
    default:
        break;
    }
}

/* Dispatch a completed OSC string (osc_buf[0..osc_len]). We only act on the
 * private `777;admin;<n>` sequence (admin-session indicator); other OSC
 * strings — title-setting etc. — are ignored. */
static void
term_osc(glyph_term_t *tp)
{
    tp->osc_buf[tp->osc_len] = '\0';
    const char *p = tp->osc_buf;
    const char *pfx = "777;admin;";
    int n = 0;
    while (pfx[n]) {
        if (p[n] != pfx[n])
            return;   /* not our sequence */
        n++;
    }
    int admin = (p[n] != '0' && p[n] != '\0') ? 1 : 0;
    if (admin != tp->admin) {
        tp->admin = admin;
        tp->admin_changed = 1;
    }
}

int
glyph_term_admin_take_change(glyph_term_t *t, int *admin_out)
{
    if (!t || !t->admin_changed)
        return 0;
    t->admin_changed = 0;
    if (admin_out)
        *admin_out = t->admin;
    return 1;
}

void
glyph_term_feed(glyph_term_t *tp, const char *data, int len)
{
    /* Snap to bottom on new output */
    if (tp->scroll_offset > 0)
        tp->scroll_offset = 0;

    for (int i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)data[i];

        /* ANSI escape sequence parser */
        if (tp->esc_state == 1) {
            /* Got ESC, expect '[' for CSI or ']' for OSC */
            if (ch == '[') {
                tp->esc_state = 2;
                tp->esc_nparam = 0;
                tp->esc_private = 0;
                tp->esc_params[0] = 0;
                tp->esc_params[1] = 0;
            } else if (ch == ']') {
                tp->esc_state = 3;   /* OSC: collect string until BEL/ST */
                tp->osc_len = 0;
            } else {
                tp->esc_state = 0; /* unknown ESC seq, drop */
            }
            continue;
        }
        if (tp->esc_state == 3) {
            /* OSC string body — terminated by BEL (0x07) or ST (ESC \). */
            if (ch == 0x07) {
                term_osc(tp);
                tp->esc_state = 0;
            } else if (ch == 0x1B) {
                tp->esc_state = 4;   /* maybe ST: expect '\' next */
            } else {
                if (tp->osc_len < (int)sizeof(tp->osc_buf) - 1)
                    tp->osc_buf[tp->osc_len++] = (char)ch;
                /* overflow: keep consuming, drop excess */
            }
            continue;
        }
        if (tp->esc_state == 4) {
            /* In OSC, saw ESC: '\' completes ST (terminate), else abort. */
            if (ch == '\\')
                term_osc(tp);
            tp->esc_state = 0;
            continue;
        }
        if (tp->esc_state == 2) {
            /* Inside CSI -- collect params and final byte */
            if (ch == '?' && tp->esc_nparam == 0) {
                tp->esc_private = 1;
                continue;
            }
            if (ch >= '0' && ch <= '9') {
                if (tp->esc_nparam == 0) tp->esc_nparam = 1;
                tp->esc_params[tp->esc_nparam - 1] =
                    tp->esc_params[tp->esc_nparam - 1] * 10 + (ch - '0');
            } else if (ch == ';') {
                if (tp->esc_nparam < 8) {
                    tp->esc_params[tp->esc_nparam] = 0;
                    tp->esc_nparam++;
                }
            } else if (ch >= 0x40 && ch <= 0x7E) {
                /* Final byte -- execute */
                term_csi(tp, ch);
                tp->esc_state = 0;
                tp->esc_private = 0;
            } else {
                tp->esc_state = 0; /* unexpected byte, abort */
                tp->esc_private = 0;
            }
            continue;
        }

        /* Normal character processing */
        if (ch == 0x1B) {
            tp->esc_state = 1;
            continue;   /* ESC byte must never fall through */
        } else if (ch == '\n') {
            tp->cy++;
            if (tp->cy >= tp->rows)
                term_scroll(tp);
        } else if (ch == '\r') {
            tp->cx = 0;
        } else if (ch == '\b' || ch == 127) {
            if (tp->cx > 0)
                tp->cx--;
        } else if (ch == '\t') {
            tp->cx = (tp->cx + 8) & ~7;
            if (tp->cx >= tp->cols) {
                tp->cx = 0;
                tp->cy++;
                if (tp->cy >= tp->rows)
                    term_scroll(tp);
            }
        } else if (ch >= 32 && ch <= 126) {
            glyph_term_cell_t *cell = grid_cell_abs(tp, tp->cy, tp->cx);
            cell->ch = (char)ch;
            cell->fg = tp->cur_fg;
            cell->bg = tp->cur_bg;
            cell->attrs = tp->cur_attrs;
            tp->cx++;
            if (tp->cx >= tp->cols) {
                tp->cx = 0;
                tp->cy++;
                if (tp->cy >= tp->rows)
                    term_scroll(tp);
            }
        }
    }
}

/* ---- Scrollbar helpers ---- */

/* Convert a Y position (in surface-local coords) to scroll_offset */
static void
sb_set_from_y(glyph_term_t *tp, int y)
{
    int track_h = tp->rows * tp->cell_h;
    int total = SCROLLBACK_LINES + tp->rows;
    int thumb_h = tp->rows * track_h / total;
    if (thumb_h < 16) thumb_h = 16;
    int usable = track_h - thumb_h;
    if (usable <= 0) return;

    int rel_y = y - tp->pad_y;
    if (rel_y < 0) rel_y = 0;
    if (rel_y > track_h) rel_y = track_h;

    /* Invert: top of track = max scroll, bottom = 0 */
    int offset = SCROLLBACK_LINES * (usable - rel_y) / usable;
    if (offset < 0) offset = 0;
    if (offset > SCROLLBACK_LINES) offset = SCROLLBACK_LINES;
    tp->scroll_offset = offset;
}

static int
sb_hit(glyph_term_t *tp, int x)
{
    int sb_x = tp->pad_x + tp->cols * tp->cell_w;
    return x >= sb_x && x < sb_x + SB_WIDTH;
}

/* ---- Mouse handlers for text selection + scrollbar ---- */

int
glyph_term_mouse_down(glyph_term_t *tp, int x, int y)
{
    /* Scrollbar click */
    if (sb_hit(tp, x)) {
        tp->sb_dragging = 1;
        sb_set_from_y(tp, y);
        return 1;
    }

    int col = (x - tp->pad_x) / tp->cell_w;
    int row = (y - tp->pad_y) / tp->cell_h;

    /* Clamp to grid */
    if (col < 0) col = 0;
    if (col >= tp->cols) col = tp->cols - 1;
    if (row < 0) row = 0;
    if (row >= tp->rows) row = tp->rows - 1;

    tp->sel_active = 1;
    tp->sel_done = 0;
    tp->sel_start_col = col;
    tp->sel_start_row = row;
    tp->sel_end_col = col;
    tp->sel_end_row = row;

    return 1;
}

int
glyph_term_mouse_move(glyph_term_t *tp, int x, int y)
{
    /* Scrollbar drag */
    if (tp->sb_dragging) {
        sb_set_from_y(tp, y);
        return 1;
    }

    if (!tp->sel_active || tp->sel_done)
        return 0;

    int col = (x - tp->pad_x) / tp->cell_w;
    int row = (y - tp->pad_y) / tp->cell_h;

    if (col < 0) col = 0;
    if (col >= tp->cols) col = tp->cols - 1;
    if (row < 0) row = 0;
    if (row >= tp->rows) row = tp->rows - 1;

    if (col != tp->sel_end_col || row != tp->sel_end_row) {
        tp->sel_end_col = col;
        tp->sel_end_row = row;
        return 1;
    }
    return 0;
}

int
glyph_term_mouse_up(glyph_term_t *tp, int x, int y)
{
    /* End scrollbar drag */
    if (tp->sb_dragging) {
        tp->sb_dragging = 0;
        return 1;
    }

    if (!tp->sel_active)
        return 0;

    int col = (x - tp->pad_x) / tp->cell_w;
    int row = (y - tp->pad_y) / tp->cell_h;

    if (col < 0) col = 0;
    if (col >= tp->cols) col = tp->cols - 1;
    if (row < 0) row = 0;
    if (row >= tp->rows) row = tp->rows - 1;

    tp->sel_end_col = col;
    tp->sel_end_row = row;

    /* If start == end, it was a click not a drag -- clear selection */
    if (tp->sel_start_col == tp->sel_end_col &&
        tp->sel_start_row == tp->sel_end_row) {
        tp->sel_active = 0;
    } else {
        tp->sel_done = 1;
    }

    return 1;
}

int
glyph_term_scroll(glyph_term_t *tp, int direction)
{
    int max_offset = SCROLLBACK_LINES;

    if (direction > 0) {
        /* Scroll up into history */
        tp->scroll_offset += 3; /* 3 lines per scroll step */
        if (tp->scroll_offset > max_offset)
            tp->scroll_offset = max_offset;
    } else {
        /* Scroll down toward present */
        tp->scroll_offset -= 3;
        if (tp->scroll_offset < 0)
            tp->scroll_offset = 0;
    }

    return 1;
}

/* ---- Selection / scrollback API ---- */

int
glyph_term_has_selection(glyph_term_t *tp)
{
    if (!tp)
        return 0;
    return tp->sel_active && tp->sel_done;
}

int
glyph_term_copy_selection(glyph_term_t *tp, char *buf, int max)
{
    if (!tp || max <= 0)
        return 0;
    if (!tp->sel_active)
        return 0;

    int sr, sc, er, ec;
    sel_normalize(tp, &sr, &sc, &er, &ec);

    int pos = 0;
    for (int r = sr; r <= er && pos < max - 1; r++) {
        int c_start = (r == sr) ? sc : 0;
        int c_end = (r == er) ? ec : tp->cols - 1;

        /* Find last non-space in this row segment to strip trailing spaces */
        int last_nonspace = c_start - 1;
        for (int c = c_end; c >= c_start; c--) {
            if (grid_cell(tp, r, c)->ch != ' ') {
                last_nonspace = c;
                break;
            }
        }

        for (int c = c_start; c <= last_nonspace && pos < max - 1; c++) {
            buf[pos++] = grid_cell(tp, r, c)->ch;
        }

        /* Add newline between lines (not after last line) */
        if (r < er && pos < max - 1)
            buf[pos++] = '\n';
    }

    buf[pos] = '\0';
    return pos;
}

void
glyph_term_clear_selection(glyph_term_t *tp)
{
    if (!tp)
        return;
    tp->sel_active = 0;
    tp->sel_done = 0;
}

void
glyph_term_scroll_back(glyph_term_t *tp, int lines)
{
    if (!tp)
        return;
    tp->scroll_offset += lines;
    if (tp->scroll_offset > SCROLLBACK_LINES)
        tp->scroll_offset = SCROLLBACK_LINES;
    if (tp->scroll_offset < 0)
        tp->scroll_offset = 0;
}

/* ---- Key translation ---- */

int
glyph_term_translate_key(unsigned char key, char *out)
{
    /* Re-expand Lumen's synthetic arrow bytes (proxy windows receive one
     * byte per LUMEN_EV_KEY; lumen/main.c folds 3-byte CSI sequences
     * into 0xF1-0xF4) back to VT escape sequences for the PTY. */
    switch (key) {
    case 0xF1: out[0] = '\033'; out[1] = '['; out[2] = 'A'; return 3; /* Up */
    case 0xF2: out[0] = '\033'; out[1] = '['; out[2] = 'B'; return 3; /* Down */
    case 0xF3: out[0] = '\033'; out[1] = '['; out[2] = 'C'; return 3; /* Right */
    case 0xF4: out[0] = '\033'; out[1] = '['; out[2] = 'D'; return 3; /* Left */
    default:
        out[0] = (char)key;
        return 1;
    }
}

/* ---- Lifecycle ---- */

glyph_term_t *
glyph_term_create(int cols, int rows, int cell_w, int cell_h,
                  int pad_x, int pad_y)
{
    if (!ansi_256_inited) {
        init_ansi_256();
        ansi_256_inited = 1;
    }

    glyph_term_t *tp = calloc(1, sizeof(*tp));
    if (!tp)
        return NULL;

    tp->master_fd = -1;
    tp->cols = cols;
    tp->rows = rows;
    tp->cell_w = cell_w;
    tp->cell_h = cell_h;
    tp->pad_x = pad_x;
    tp->pad_y = pad_y;
    /* Runtime-configurable defaults (match the old hard-coded behavior). */
    tp->theme = &s_term_themes[0];
    tp->cursor_style = GLYPH_TERM_CURSOR_BLOCK;
    tp->cursor_blink = 1;
    tp->font_px = 16;
    tp->scrollback = SCROLLBACK_LINES;
    tp->total_rows = rows + tp->scrollback;
    tp->scroll_top = 0;
    tp->scroll_offset = 0;

    tp->cur_fg = TERM_DEFAULT_COLOR;
    tp->cur_bg = TERM_DEFAULT_COLOR;
    tp->cur_attrs = 0;
    tp->cursor_visible = 1;
    tp->grid = calloc((unsigned)(tp->total_rows * cols), sizeof(glyph_term_cell_t));
    if (!tp->grid) {
        free(tp);
        return NULL;
    }
    for (int i = 0; i < tp->total_rows * cols; i++)
        tp->grid[i] = (glyph_term_cell_t){' ', TERM_DEFAULT_COLOR, TERM_DEFAULT_COLOR, 0};

    return tp;
}

void
glyph_term_destroy(glyph_term_t *tp)
{
    if (!tp)
        return;
    free(tp->grid);
    free(tp);
}

/* Push the current grid dimensions to the PTY so programs (vim, less, the
 * shell's line editor) know the real window size. Call once after the caller
 * sets master_fd, and it's re-applied on every resize. */
void glyph_term_apply_winsize(glyph_term_t *tp)
{
    if (tp->master_fd < 0)
        return;
    struct winsize ws;
    ws.ws_row = (unsigned short)tp->rows;
    ws.ws_col = (unsigned short)tp->cols;
    ws.ws_xpixel = (unsigned short)(tp->cols * tp->cell_w);
    ws.ws_ypixel = (unsigned short)(tp->rows * tp->cell_h);
    ioctl(tp->master_fd, TIOCSWINSZ, &ws);
}

int
glyph_term_resize(glyph_term_t *tp, int cols, int rows,
                  int cell_w, int cell_h, int font_px)
{
    if (!tp || cols < 1 || rows < 1)
        return -1;
    if (font_px < 1) font_px = tp->font_px;

    int new_total = rows + tp->scrollback;
    glyph_term_cell_t *ng = calloc((unsigned)(new_total * cols), sizeof(*ng));
    if (!ng)
        return -1;
    for (int i = 0; i < new_total * cols; i++)
        ng[i] = (glyph_term_cell_t){' ', TERM_DEFAULT_COLOR, TERM_DEFAULT_COLOR, 0};

    /* Preserve the visible screen (top-left overlap). Scrollback is reset —
     * ponytail: no reflow/rewrap; keeping the visible grid is the 90% that
     * matters, and re-wrapping a ring buffer across a column change is a lot of
     * code for a rare action. */
    int cr = rows < tp->rows ? rows : tp->rows;
    int cc = cols < tp->cols ? cols : tp->cols;
    for (int r = 0; r < cr; r++) {
        glyph_term_cell_t *src = grid_cell(tp, r, 0);
        for (int c = 0; c < cc; c++)
            ng[r * cols + c] = src[c];
    }

    free(tp->grid);
    tp->grid = ng;
    tp->cols = cols;
    tp->rows = rows;
    tp->cell_w = cell_w;
    tp->cell_h = cell_h;
    tp->font_px = font_px;
    tp->total_rows = new_total;
    tp->scroll_top = 0;
    tp->scroll_offset = 0;
    if (tp->cx >= cols) tp->cx = cols - 1;
    if (tp->cy >= rows) tp->cy = rows - 1;
    tp->sel_active = 0;
    tp->sel_done = 0;

    glyph_term_apply_winsize(tp);
    return 0;
}

void glyph_term_set_theme(glyph_term_t *tp, int scheme)
{
    if (!tp) return;
    if (scheme < 0) scheme = 0;
    if (scheme >= TERM_NTHEMES) scheme = TERM_NTHEMES - 1;
    tp->theme = &s_term_themes[scheme];
}

void glyph_term_set_cursor_style(glyph_term_t *tp, int style)
{
    if (tp) tp->cursor_style = style;
}

void glyph_term_set_cursor_blink(glyph_term_t *tp, int on)
{
    if (tp) tp->cursor_blink = on ? 1 : 0;
}

int glyph_term_theme_count(void) { return TERM_NTHEMES; }

const char *glyph_term_theme_name(int scheme)
{
    if (scheme < 0 || scheme >= TERM_NTHEMES) return "";
    return s_term_themes[scheme].name;
}

uint32_t glyph_term_theme_preview(int scheme)
{
    if (scheme < 0 || scheme >= TERM_NTHEMES) return 0;
    uint32_t bg = s_term_themes[scheme].bg;
    return bg == GLYPH_TERM_DYN ? 0x00141A24u : bg;  /* frost → a dark chip */
}

/* ---- PTY + shell spawning ---- */

/* Spawn the shell process for a terminal. The slave fd becomes the
 * child's stdio via sys_spawn's stdio_fd argument; sys_spawn puts the
 * child in a new session (sid = pid) and the kernel's PTY layer
 * handles controlling-terminal acquisition + foreground pgrp (the
 * shell adjusts fg_pgrp per command via sys_setfg).
 *
 * Note: the old terminal.c also issued two sys_cap_grant_exec(361)
 * calls here. That syscall was retired in the Phase 46c cap-policy
 * redesign (caps now come from /etc/aegis/caps.d/ at exec time), so
 * the dead calls are dropped. */
static int
spawn_shell(int slave_fd)
{
    char *argv[] = {"-stsh", NULL};  /* leading '-' = login shell */

    /* Inherit the session's identity rather than hardcoding root: USER/HOME
     * come from the environment the desktop was logged into. LoricaOS has no
     * "root" user — uid 0 just carries the name the person chose at install. */
    const char *user = getenv("USER");
    const char *home = getenv("HOME");
    char user_env[96] = "USER=";
    char home_env[288] = "HOME=";
    strncat(user_env, (user && user[0]) ? user : "user", sizeof(user_env) - 6);
    strncat(home_env, (home && home[0]) ? home : "/",    sizeof(home_env) - 6);
    char *envp[] = {"PATH=/bin", home_env, "TERM=xterm-256color", user_env, NULL};

    long pid = syscall(SYS_SPAWN, "/bin/stsh", argv, envp, slave_fd, 0);
    return (int)pid;
}

/* Open a PTY pair and spawn a shell. Returns master_fd or -1 on failure.
 * If fail_reason_out is non-NULL, sets it to a description.
 * If shell_pid_out is non-NULL, receives the shell's PID on success. */
int
glyph_pty_open_and_spawn(const char **fail_reason_out, int *shell_pid_out)
{
    int master_fd = -1, pts_num = -1, slave_fd = -1;
    char slave_path[32];
    const char *fail_reason = NULL;

    master_fd = open("/dev/ptmx", O_RDWR);
    if (master_fd < 0) {
        fail_reason = "open /dev/ptmx failed";
        goto fail;
    }

    if (ioctl(master_fd, 0x80045430 /* TIOCGPTN */, &pts_num) < 0) {
        fail_reason = "TIOCGPTN failed";
        goto fail;
    }

    {
        int unlock = 0;
        ioctl(master_fd, 0x40045431 /* TIOCSPTLCK */, &unlock);
    }

    {
        const char *prefix = "/dev/pts/";
        int j = 0;
        while (*prefix)
            slave_path[j++] = *prefix++;
        if (pts_num == 0) {
            slave_path[j++] = '0';
        } else {
            char digits[10];
            int nd = 0, n = pts_num;
            while (n > 0) {
                digits[nd++] = '0' + (n % 10);
                n /= 10;
            }
            for (int k = nd - 1; k >= 0; k--)
                slave_path[j++] = digits[k];
        }
        slave_path[j] = '\0';
    }

    /* O_NOCTTY: the emulator opens the slave only to pass it to the child
     * shell as stdio.  The shell — spawned as a new session leader — must
     * own the controlling terminal and the foreground pgrp, not us.  Without
     * O_NOCTTY a session-leader emulator (/bin/terminal) steals fg_pgrp and
     * the shell's first read stops on SIGTTIN. */
    slave_fd = open(slave_path, O_RDWR | O_NOCTTY);
    if (slave_fd < 0) {
        fail_reason = "open slave failed";
        goto fail;
    }

    fcntl(master_fd, F_SETFL, O_NONBLOCK);

    {
        long pid = spawn_shell(slave_fd);
        if (pid < 0) {
            fail_reason = "sys_spawn failed";
            goto fail;
        }
        if (shell_pid_out)
            *shell_pid_out = (int)pid;
    }

    close(slave_fd);
    return master_fd;

fail:
    if (slave_fd >= 0)
        close(slave_fd);
    if (master_fd >= 0)
        close(master_fd);
    if (fail_reason_out)
        *fail_reason_out = fail_reason;
    return -1;
}
