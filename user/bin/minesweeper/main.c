/* user/bin/minesweeper/main.c — Minesweeper for Aegis (external Lumen client)
 *
 * The classic mine-hunting game, speaking the Lumen external window protocol
 * (same pattern as 2048 / calculator). Pure userspace, integer-only.
 *
 * This is THE mouse showcase:
 *   - LEFT-click a covered cell  : reveal it (flood-fill on zero-neighbour)
 *   - RIGHT-click a covered cell : toggle a flag
 *   - click the smiley header     : restart
 *   - 'R'                         : restart
 *   - Esc / close                 : quit
 *
 * First-click safety: the mine field is generated lazily on the first
 * reveal, guaranteeing the first cell (and its neighbours) are clear.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <stdint.h>
#include <time.h>

#include <glyph.h>
#include <lumen_client.h>
#include "font.h"

/* ── Grid config (Beginner): 9x9 with 10 mines ───────────────────────── */
#define COLS    9
#define ROWS    9
#define MINES   10
#define NCELLS  (COLS * ROWS)

/* ── Layout ──────────────────────────────────────────────────────────── */
#define CELL    36                                  /* px per cell        */
#define MARGIN  14
#define HDR_H   60
#define GRID_W  (COLS * CELL)
#define GRID_H  (ROWS * CELL)
#define WIN_W   (GRID_W + 2 * MARGIN)               /* 9*36 + 28 = 352    */
#define WIN_H   (HDR_H + GRID_H + 2 * MARGIN)       /* 60 + 324 + 28 = 412 */
#define GRID_X  MARGIN
#define GRID_Y  (HDR_H + MARGIN)
#define CELL_X(c) (GRID_X + (c) * CELL)
#define CELL_Y(r) (GRID_Y + (r) * CELL)

/* Smiley restart button (centered in the header). */
#define FACE_SZ 40
#define FACE_X  ((WIN_W - FACE_SZ) / 2)
#define FACE_Y  ((HDR_H - FACE_SZ) / 2)

#define KEY_ESC '\x1b'

/* ── Colours (XRGB) ──────────────────────────────────────────────────── */
#define C_BG       0x00C0C0C0   /* classic minesweeper grey            */
#define C_HDR      0x00BEBEBE
#define C_COVERED  0x00BDBDBD
#define C_COV_HI   0x00FFFFFF    /* covered bevel highlight (top/left)  */
#define C_COV_LO   0x00808080    /* covered bevel shadow (bottom/right) */
#define C_OPEN     0x00D8D8D8    /* revealed cell face                  */
#define C_GRID     0x00909090
#define C_FACE_BG  0x00FFD83D    /* smiley yellow                       */
#define C_FACE_LN  0x00202020
#define C_MINE     0x00000000
#define C_MINE_BG  0x00FF4040    /* the mine you stepped on             */
#define C_FLAG     0x00D81212
#define C_FLAGPOLE 0x00202020
#define C_TEXT     0x00202020
#define C_PANEL    0x00303030    /* mine/flag counter LCD background    */
#define C_LCD      0x00FF3030

/* Classic per-number colours, indexed 1..8. */
static const uint32_t s_num_color[9] = {
    0,
    0x000000FF, /* 1 blue   */
    0x00008000, /* 2 green  */
    0x00FF0000, /* 3 red    */
    0x00000080, /* 4 navy   */
    0x00800000, /* 5 maroon */
    0x00008080, /* 6 teal   */
    0x00000000, /* 7 black  */
    0x00808080, /* 8 grey   */
};

/* ── Cell flags ──────────────────────────────────────────────────────── */
#define F_MINE     0x01
#define F_REVEALED 0x02
#define F_FLAGGED  0x04
#define F_BOOM     0x08   /* the specific mine that ended the game */

/* ── State ───────────────────────────────────────────────────────────── */
typedef struct {
    int             lfd;
    lumen_window_t *lwin;
    surface_t       surf;
    int             fb_w, fb_h;
    int             dirty, done;

    uint8_t cell[ROWS][COLS];   /* F_* bitmask                          */
    uint8_t adj[ROWS][COLS];    /* adjacent mine count                  */
    int     placed;             /* mines laid yet? (lazy, first-click)  */
    int     revealed_count;     /* non-mine cells revealed              */
    int     flags;              /* flags currently placed               */
    int     over;               /* hit a mine                           */
    int     won;                /* all safe cells revealed              */
} game_t;

static game_t g;
static volatile sig_atomic_t s_term;
static void sigterm_handler(int s) { (void)s; s_term = 1; }

/* ── PRNG (xorshift32) ───────────────────────────────────────────────── */
static uint32_t s_rng;
static void rng_seed(void)
{
    s_rng  = (uint32_t)getpid() * 2654435761u;
    s_rng ^= (uint32_t)time(NULL) * 2246822519u;
    s_rng ^= 0x9E3779B9u;
    if (!s_rng) s_rng = 0x12345678u;
}
static uint32_t rng_next(void)
{
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5;
    return s_rng;
}
static int rng_below(int n) { return (int)(rng_next() % (uint32_t)n); }

/* ── Text helpers (TTF if available, bitmap fallback) ────────────────── */
static void text_sz(int sz, int x, int y, const char *s, uint32_t color)
{
    if (g_font_ui) font_draw_text(&g.surf, g_font_ui, sz, x, y, s, color);
    else           draw_text_t(&g.surf, x, y, s, color);
}
static int text_w(int sz, const char *s)
{
    if (g_font_ui) return font_text_width(g_font_ui, sz, s);
    return (int)strlen(s) * FONT_W;
}

/* ── Board logic ─────────────────────────────────────────────────────── */
static int in_bounds(int r, int c)
{
    return r >= 0 && r < ROWS && c >= 0 && c < COLS;
}

/* Lay MINES mines, avoiding the safe cell and its 8 neighbours so the
 * first click always opens a region. Then compute adjacency counts. */
static void place_mines(int safe_r, int safe_c)
{
    int placed = 0;
    while (placed < MINES) {
        int r = rng_below(ROWS), c = rng_below(COLS);
        if (g.cell[r][c] & F_MINE) continue;
        /* Keep the first-click cell and its neighbourhood mine-free. */
        int dr = r - safe_r, dc = c - safe_c;
        if (dr >= -1 && dr <= 1 && dc >= -1 && dc <= 1) continue;
        g.cell[r][c] |= F_MINE;
        placed++;
    }
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            int n = 0;
            for (int yy = -1; yy <= 1; yy++)
                for (int xx = -1; xx <= 1; xx++) {
                    if (!yy && !xx) continue;
                    if (in_bounds(r + yy, c + xx) &&
                        (g.cell[r + yy][c + xx] & F_MINE))
                        n++;
                }
            g.adj[r][c] = (uint8_t)n;
        }
    }
    g.placed = 1;
}

static void new_game(void)
{
    memset(g.cell, 0, sizeof(g.cell));
    memset(g.adj, 0, sizeof(g.adj));
    g.placed = 0;
    g.revealed_count = 0;
    g.flags = 0;
    g.over = 0;
    g.won = 0;
    g.dirty = 1;
}

/* Iterative flood-fill reveal (explicit stack — no recursion, NCELLS max). */
static void flood_reveal(int sr, int sc)
{
    int stack[NCELLS][2];
    int sp = 0;
    stack[sp][0] = sr; stack[sp][1] = sc; sp++;

    while (sp > 0) {
        sp--;
        int r = stack[sp][0], c = stack[sp][1];
        if (!in_bounds(r, c)) continue;
        if (g.cell[r][c] & (F_REVEALED | F_FLAGGED | F_MINE)) continue;

        g.cell[r][c] |= F_REVEALED;
        g.revealed_count++;

        if (g.adj[r][c] == 0) {
            for (int yy = -1; yy <= 1; yy++)
                for (int xx = -1; xx <= 1; xx++) {
                    if (!yy && !xx) continue;
                    int nr = r + yy, nc = c + xx;
                    if (in_bounds(nr, nc) &&
                        !(g.cell[nr][nc] & (F_REVEALED | F_FLAGGED | F_MINE)))
                        { stack[sp][0] = nr; stack[sp][1] = nc; sp++; }
                }
        }
    }
}

static void lose(int br, int bc)
{
    g.over = 1;
    g.cell[br][bc] |= F_BOOM;
    /* Reveal every mine. */
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++)
            if (g.cell[r][c] & F_MINE)
                g.cell[r][c] |= F_REVEALED;
}

static void check_win(void)
{
    /* Win when every non-mine cell is revealed. */
    if (g.revealed_count >= NCELLS - MINES) {
        g.won = 1;
        /* Auto-flag remaining mines for a tidy finish. */
        for (int r = 0; r < ROWS; r++)
            for (int c = 0; c < COLS; c++)
                if ((g.cell[r][c] & F_MINE) && !(g.cell[r][c] & F_FLAGGED)) {
                    g.cell[r][c] |= F_FLAGGED;
                    g.flags++;
                }
    }
}

static void reveal_cell(int r, int c)
{
    if (g.over || g.won) return;
    if (!in_bounds(r, c)) return;
    if (g.cell[r][c] & (F_REVEALED | F_FLAGGED)) return;

    if (!g.placed) place_mines(r, c);   /* lazy gen → first-click safety */

    if (g.cell[r][c] & F_MINE) { lose(r, c); g.dirty = 1; return; }

    flood_reveal(r, c);
    check_win();
    g.dirty = 1;
}

static void toggle_flag(int r, int c)
{
    if (g.over || g.won) return;
    if (!in_bounds(r, c)) return;
    if (g.cell[r][c] & F_REVEALED) return;

    if (g.cell[r][c] & F_FLAGGED) { g.cell[r][c] &= ~F_FLAGGED; g.flags--; }
    else                          { g.cell[r][c] |=  F_FLAGGED; g.flags++; }
    g.dirty = 1;
}

/* ── Hit testing ─────────────────────────────────────────────────────── */
static int hit_cell(int px, int py, int *r, int *c)
{
    if (px < GRID_X || px >= GRID_X + GRID_W) return 0;
    if (py < GRID_Y || py >= GRID_Y + GRID_H) return 0;
    *c = (px - GRID_X) / CELL;
    *r = (py - GRID_Y) / CELL;
    return in_bounds(*r, *c);
}
static int hit_face(int px, int py)
{
    return px >= FACE_X && px < FACE_X + FACE_SZ &&
           py >= FACE_Y && py < FACE_Y + FACE_SZ;
}

/* ── Rendering ───────────────────────────────────────────────────────── */

/* A raised 3D bevel like the classic covered tile. */
static void bevel(int x, int y, int w, int h, uint32_t face,
                  uint32_t hi, uint32_t lo)
{
    surface_t *s = &g.surf;
    draw_fill_rect(s, x, y, w, h, face);
    draw_fill_rect(s, x, y, w, 2, hi);          /* top    */
    draw_fill_rect(s, x, y, 2, h, hi);          /* left   */
    draw_fill_rect(s, x, y + h - 2, w, 2, lo);  /* bottom */
    draw_fill_rect(s, x + w - 2, y, 2, h, lo);  /* right  */
}

/* Draw a small mine glyph centered in a cell rect. */
static void draw_mine(int x, int y)
{
    surface_t *s = &g.surf;
    int cx = x + CELL / 2, cy = y + CELL / 2;
    int rad = CELL / 5;
    draw_circle_filled(s, cx, cy, rad, C_MINE);
    /* Spikes (horizontal, vertical, diagonals). */
    draw_line(s, cx - rad - 3, cy, cx + rad + 3, cy, C_MINE);
    draw_line(s, cx, cy - rad - 3, cx, cy + rad + 3, C_MINE);
    draw_line(s, cx - rad - 2, cy - rad - 2, cx + rad + 2, cy + rad + 2, C_MINE);
    draw_line(s, cx - rad - 2, cy + rad + 2, cx + rad + 2, cy - rad - 2, C_MINE);
    /* Little shine. */
    draw_fill_rect(s, cx - rad / 2, cy - rad / 2, 2, 2, 0x00FFFFFF);
}

/* Draw a flag glyph centered in a cell rect. */
static void draw_flag(int x, int y)
{
    surface_t *s = &g.surf;
    int px = x + CELL / 2 - 5;        /* pole left edge */
    int top = y + 7;
    int bot = y + CELL - 8;
    draw_fill_rect(s, px, top, 2, bot - top, C_FLAGPOLE);          /* pole */
    draw_fill_rect(s, px - 5, bot, 12, 3, C_FLAGPOLE);             /* base */
    /* Triangular flag, drawn as shrinking horizontal runs. */
    for (int i = 0; i < 8; i++) {
        int wseg = 10 - (i < 4 ? i : (7 - i)) * 2;
        if (wseg < 1) wseg = 1;
        draw_fill_rect(s, px - wseg, top + i, wseg, 1, C_FLAG);
    }
}

/* Smiley restart face: changes with game state. */
static void draw_face(void)
{
    surface_t *s = &g.surf;
    int cx = FACE_X + FACE_SZ / 2, cy = FACE_Y + FACE_SZ / 2;
    int rad = FACE_SZ / 2 - 1;

    draw_circle_filled(s, cx, cy, rad, C_FACE_BG);
    draw_circle(s, cx, cy, rad, C_FACE_LN);

    int ex = 5, ey = 4;
    if (g.over) {
        /* X eyes. */
        for (int d = -2; d <= 2; d++) {
            draw_px(s, cx - ex + d, cy - ey + d, C_FACE_LN);
            draw_px(s, cx - ex - d, cy - ey + d, C_FACE_LN);
            draw_px(s, cx + ex + d, cy - ey + d, C_FACE_LN);
            draw_px(s, cx + ex - d, cy - ey + d, C_FACE_LN);
        }
        /* Frown. */
        for (int dx = -5; dx <= 5; dx++) {
            int dy = (dx * dx) / 12;
            draw_px(s, cx + dx, cy + 8 - dy, C_FACE_LN);
        }
    } else {
        /* Round eyes. */
        draw_circle_filled(s, cx - ex, cy - ey, 2, C_FACE_LN);
        draw_circle_filled(s, cx + ex, cy - ey, 2, C_FACE_LN);
        if (g.won) {
            /* Sunglasses-ish smile: a wide grin. */
            for (int dx = -6; dx <= 6; dx++) {
                int dy = (dx * dx) / 10;
                draw_px(s, cx + dx, cy + 3 + dy, C_FACE_LN);
                draw_px(s, cx + dx, cy + 4 + dy, C_FACE_LN);
            }
        } else {
            /* Smile. */
            for (int dx = -5; dx <= 5; dx++) {
                int dy = (dx * dx) / 12;
                draw_px(s, cx + dx, cy + 4 + dy, C_FACE_LN);
            }
        }
    }
}

/* 7-seg-style 3-digit counter (just a coloured box with the number). */
static void draw_counter(int x, int val, uint32_t numcolor)
{
    surface_t *s = &g.surf;
    int w = 52, h = 30, y = (HDR_H - h) / 2;
    if (val < 0) val = 0;
    if (val > 999) val = 999;
    draw_fill_rect(s, x, y, w, h, C_PANEL);
    draw_rect(s, x, y, w, h, 0x00101010);
    char buf[8];
    snprintf(buf, sizeof(buf), "%03d", val);
    int tw = text_w(20, buf);
    text_sz(20, x + (w - tw) / 2, y + (h - 20) / 2, buf, numcolor);
}

static void render(void)
{
    if (!g.dirty) return;
    g.dirty = 0;
    surface_t *s = &g.surf;

    /* Background + header strip. */
    draw_fill_rect(s, 0, 0, g.fb_w, g.fb_h, C_BG);
    draw_fill_rect(s, 0, 0, g.fb_w, HDR_H, C_HDR);

    /* Mines-remaining (mines − flags) on the left, restart face center. */
    draw_counter(MARGIN, MINES - g.flags, C_LCD);
    draw_face();

    /* Revealed-safe-cells progress on the right. */
    draw_counter(WIN_W - MARGIN - 52, g.revealed_count, 0x0030FF60);

    /* Grid. */
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            int x = CELL_X(c), y = CELL_Y(r);
            uint8_t cv = g.cell[r][c];

            if (cv & F_REVEALED) {
                uint32_t face = (cv & F_BOOM) ? C_MINE_BG : C_OPEN;
                draw_fill_rect(s, x, y, CELL, CELL, face);
                draw_rect(s, x, y, CELL, CELL, C_GRID);
                if (cv & F_MINE) {
                    draw_mine(x, y);
                } else if (g.adj[r][c] > 0) {
                    char buf[2] = { (char)('0' + g.adj[r][c]), 0 };
                    int tw = text_w(22, buf);
                    text_sz(22, x + (CELL - tw) / 2, y + (CELL - 22) / 2,
                            buf, s_num_color[g.adj[r][c]]);
                }
            } else {
                bevel(x, y, CELL, CELL, C_COVERED, C_COV_HI, C_COV_LO);
                if (cv & F_FLAGGED) draw_flag(x, y);
            }
        }
    }

    /* Win / lose overlay banner across the grid. */
    if (g.over || g.won) {
        int bw = GRID_W - 20, bh = 56;
        int bx = GRID_X + 10;
        int by = GRID_Y + (GRID_H - bh) / 2;
        draw_blend_rect(s, bx, by, bw, bh, 0x00000000, 150);
        draw_rect(s, bx, by, bw, bh, 0x00FFFFFF);
        const char *msg = g.won ? "You win!" : "Game Over";
        uint32_t col = g.won ? 0x0040FF60 : 0x00FF5050;
        int mw = text_w(28, msg);
        text_sz(28, bx + (bw - mw) / 2, by + 6, msg, col);
        const char *sub = "Click the face or press R";
        int sw = text_w(13, sub);
        text_sz(13, bx + (bw - sw) / 2, by + 36, sub, 0x00E0E0E0);
    }

    lumen_window_present(g.lwin);
}

/* ── Main ────────────────────────────────────────────────────────────── */
int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    g.lfd = lumen_connect_retry();
    if (g.lfd < 0) {
        dprintf(2, "[minesweeper] lumen_connect failed (%d)\n", g.lfd);
        return 1;
    }

    g.lwin = lumen_window_create(g.lfd, "Minesweeper", WIN_W, WIN_H);
    if (!g.lwin) {
        dprintf(2, "[minesweeper] window_create failed\n");
        close(g.lfd);
        return 1;
    }

    g.fb_w = g.lwin->w; g.fb_h = g.lwin->h;
    g.surf = (surface_t){ .buf = (uint32_t *)g.lwin->backbuf,
                          .w = g.fb_w, .h = g.fb_h, .pitch = g.lwin->stride };
    font_init();

    struct sigaction sa; memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigterm_handler; sigaction(SIGTERM, &sa, NULL);

    rng_seed();
    new_game();
    render();
    dprintf(2, "[minesweeper] connected %dx%d\n", g.lwin->w, g.lwin->h);

    while (!s_term && !g.done) {
        lumen_event_t ev;
        int r = lumen_wait_event(g.lfd, &ev, 100);
        if (r < 0) break;
        if (r == 1) {
            if (ev.type == LUMEN_EV_CLOSE_REQUEST) break;

            if (ev.type == LUMEN_EV_KEY && ev.key.pressed) {
                char k = (char)ev.key.keycode;
                if (k == KEY_ESC) break;
                if (k == 'r' || k == 'R') { new_game(); }
            }

            if (ev.type == LUMEN_EV_MOUSE &&
                ev.mouse.evtype == LUMEN_MOUSE_DOWN) {
                int cx = ev.mouse.x, cy = ev.mouse.y;
                if (hit_face(cx, cy)) {
                    new_game();
                } else {
                    int rr, cc;
                    if (hit_cell(cx, cy, &rr, &cc)) {
                        if (ev.mouse.buttons & 2)      toggle_flag(rr, cc);
                        else if (ev.mouse.buttons & 1) reveal_cell(rr, cc);
                    }
                }
            }
        }
        render();
    }

    lumen_window_destroy(g.lwin);
    close(g.lfd);
    dprintf(2, "[minesweeper] exit\n");
    return 0;
}
