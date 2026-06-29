/* user/bin/2048/main.c — 2048 for Aegis (external Lumen client)
 *
 * The classic sliding-tile game, speaking the Lumen external window protocol
 * (same pattern as calculator / settings). Pure userspace: a 4x4 board of
 * powers of two, slide+merge logic, and a Glyph-rendered board. Distributed as
 * a herald package — installs into /apps/2048 and appears in the launcher.
 *
 * Controls: arrow keys or WASD to slide, R for a new game, Esc/close to quit.
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

/* ── Layout ───────────────────────────────────────────────────────────── */
#define WIN_W   360
#define WIN_H   474
#define MARGIN  15
#define HDR_H   72
#define GAP     10
#define CELL    70
#define BOARD_W (4 * CELL + 5 * GAP)              /* 330 */
#define BOARD_X ((WIN_W - BOARD_W) / 2)           /* 15  */
#define BOARD_Y (HDR_H + MARGIN)                  /* 87  */
#define TILE_X(c) (BOARD_X + GAP + (c) * (CELL + GAP))
#define TILE_Y(r) (BOARD_Y + GAP + (r) * (CELL + GAP))

/* Synthetic arrow keycodes Lumen delivers to proxy windows. */
#define KEY_UP    ((char)0xF1)
#define KEY_DOWN  ((char)0xF2)
#define KEY_RIGHT ((char)0xF3)
#define KEY_LEFT  ((char)0xF4)
#define KEY_ESC   '\x1b'

/* ── Colors (classic 2048 palette, XRGB) ──────────────────────────────── */
#define C_BG     0x00FAF8EF
#define C_BOARD  0x00BBADA0
#define C_EMPTY  0x00CDC1B4
#define C_GOLD   0x00EDC22E
#define C_DARK   0x00776E65
#define C_LIGHT  0x00F9F6F2

/* ── State ────────────────────────────────────────────────────────────── */
typedef struct {
    int             lfd;
    lumen_window_t *lwin;
    surface_t       surf;
    int             fb_w, fb_h;
    int             dirty, done;

    int  board[4][4];
    long score, best;
    int  over;          /* no moves left */
    int  win, win_seen; /* reached 2048; banner shown until next move */
} game_t;

static game_t g;
static volatile sig_atomic_t s_term;
static void sigterm_handler(int s) { (void)s; s_term = 1; }

/* ── PRNG (xorshift32, seeded per-launch so each game differs) ─────────── */
static uint32_t s_rng;
static void rng_seed(void)
{
    s_rng  = (uint32_t)getpid() * 2654435761u;
    s_rng ^= (uint32_t)time(NULL) * 2246822519u;   /* harmless if time()==0 */
    s_rng ^= 0x9E3779B9u;
    if (!s_rng) s_rng = 0x12345678u;
}
static uint32_t rng_next(void)
{
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5;
    return s_rng;
}
static int rng_below(int n) { return (int)(rng_next() % (uint32_t)n); }

/* ── Drawing helpers ──────────────────────────────────────────────────── */
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

static uint32_t tile_bg(int v)
{
    switch (v) {
    case 0:    return C_EMPTY;
    case 2:    return 0x00EEE4DA;
    case 4:    return 0x00EDE0C8;
    case 8:    return 0x00F2B179;
    case 16:   return 0x00F59563;
    case 32:   return 0x00F67C5F;
    case 64:   return 0x00F65E3B;
    case 128:  return 0x00EDCF72;
    case 256:  return 0x00EDCC61;
    case 512:  return 0x00EDC850;
    case 1024: return 0x00EDC53F;
    case 2048: return 0x00EDC22E;
    default:   return 0x003C3A32;   /* beyond 2048 */
    }
}

/* ── Game logic ───────────────────────────────────────────────────────── */
static void new_game(void);

static int spawn_tile(void)
{
    int empty[16], n = 0;
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            if (!g.board[r][c]) empty[n++] = r * 4 + c;
    if (!n) return 0;
    int p = empty[rng_below(n)];
    g.board[p / 4][p % 4] = (rng_below(10) == 0) ? 4 : 2;
    return 1;
}

/* Slide+merge one 4-cell line toward index 0. Returns 1 if it changed. */
static int slide_line(int line[4], long *gain)
{
    int tmp[4] = {0}, n = 0;
    for (int i = 0; i < 4; i++) if (line[i]) tmp[n++] = line[i];

    int out[4] = {0}, o = 0, i = 0;
    while (i < 4 && tmp[i]) {
        if (i + 1 < 4 && tmp[i + 1] == tmp[i]) {
            int m = tmp[i] * 2; out[o++] = m; *gain += m; i += 2;
        } else {
            out[o++] = tmp[i]; i++;
        }
    }
    int changed = 0;
    for (int k = 0; k < 4; k++) { if (out[k] != line[k]) changed = 1; line[k] = out[k]; }
    return changed;
}

/* dir: 0=up 1=down 2=left 3=right */
static void cell_of(int dir, int k, int i, int *r, int *c)
{
    switch (dir) {
    case 0: *r = i;     *c = k;     break;  /* up:    front = row 0 */
    case 1: *r = 3 - i; *c = k;     break;  /* down */
    case 2: *r = k;     *c = i;     break;  /* left:  front = col 0 */
    default:*r = k;     *c = 3 - i; break;  /* right */
    }
}

static int do_move(int dir)
{
    int moved = 0; long gain = 0;
    for (int k = 0; k < 4; k++) {
        int line[4], r, c;
        for (int i = 0; i < 4; i++) { cell_of(dir, k, i, &r, &c); line[i] = g.board[r][c]; }
        if (slide_line(line, &gain)) moved = 1;
        for (int i = 0; i < 4; i++) { cell_of(dir, k, i, &r, &c); g.board[r][c] = line[i]; }
    }
    if (moved) {
        g.score += gain;
        if (g.score > g.best) g.best = g.score;
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++)
                if (g.board[r][c] >= 2048 && !g.win_seen) g.win = 1;
        spawn_tile();
    }
    return moved;
}

static int has_moves(void)
{
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            if (!g.board[r][c]) return 1;
            if (c + 1 < 4 && g.board[r][c] == g.board[r][c + 1]) return 1;
            if (r + 1 < 4 && g.board[r][c] == g.board[r + 1][c]) return 1;
        }
    return 0;
}

static void new_game(void)
{
    memset(g.board, 0, sizeof(g.board));
    g.score = 0; g.over = 0; g.win = 0; g.win_seen = 0;
    spawn_tile();
    spawn_tile();
}

static void handle_move(int dir)
{
    if (g.over) return;
    if (g.win) { g.win = 0; g.win_seen = 1; }  /* a move dismisses the win banner */
    if (do_move(dir)) {
        if (!has_moves()) g.over = 1;
        g.dirty = 1;
    }
}

/* ── Render ───────────────────────────────────────────────────────────── */
static void draw_score_box(int x, int w, const char *label, long val)
{
    draw_rounded_rect(&g.surf, x, 18, w, 46, 6, C_BOARD);
    int lw = text_w(13, label);
    text_sz(13, x + (w - lw) / 2, 22, label, 0x00EEE4DA);
    char num[24]; snprintf(num, sizeof(num), "%ld", val);
    int nw = text_w(20, num);
    text_sz(20, x + (w - nw) / 2, 38, num, C_LIGHT);
}

static void draw_banner(const char *msg, const char *sub)
{
    int bw = 250, bh = 96;
    int bx = BOARD_X + (BOARD_W - bw) / 2;
    int by = BOARD_Y + (BOARD_W - bh) / 2;
    draw_rounded_rect(&g.surf, bx, by, bw, bh, 10, 0x00EEE4DA);
    draw_rect(&g.surf, bx, by, bw, bh, C_BOARD);
    int mw = text_w(30, msg);
    text_sz(30, bx + (bw - mw) / 2, by + 18, msg, C_DARK);
    int sw = text_w(16, sub);
    text_sz(16, bx + (bw - sw) / 2, by + 58, sub, C_DARK);
}

static void render(void)
{
    if (!g.dirty) return;
    g.dirty = 0;
    surface_t *s = &g.surf;

    draw_fill_rect(s, 0, 0, g.fb_w, g.fb_h, C_BG);

    /* Header: title + score / best boxes. */
    text_sz(42, MARGIN, 14, "2048", C_GOLD);
    draw_score_box(WIN_W - MARGIN - 80,        80, "SCORE", g.score);
    draw_score_box(WIN_W - MARGIN - 80 - 88,   80, "BEST",  g.best);

    /* Board background. */
    draw_rounded_rect(s, BOARD_X, BOARD_Y, BOARD_W, BOARD_W, 8, C_BOARD);

    /* Tiles. */
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            int v = g.board[r][c];
            int x = TILE_X(c), y = TILE_Y(r);
            draw_rounded_rect(s, x, y, CELL, CELL, 5, tile_bg(v));
            if (v) {
                char buf[12]; snprintf(buf, sizeof(buf), "%d", v);
                int len = (int)strlen(buf);
                int sz = len <= 2 ? 34 : len == 3 ? 27 : len == 4 ? 21 : 17;
                uint32_t fg = (v <= 4) ? C_DARK : C_LIGHT;
                int tw = text_w(sz, buf);
                text_sz(sz, x + (CELL - tw) / 2, y + (CELL - sz) / 2 + 1, buf, fg);
            }
        }
    }

    /* Footer hint. */
    const char *hint = "Arrows / WASD to move  \xb7  R: new game";
    text_sz(14, BOARD_X, BOARD_Y + BOARD_W + 12, hint, 0x00998E80);

    if (g.over)      draw_banner("Game Over", "Press R to play again");
    else if (g.win)  draw_banner("You Win!", "R: new game  \xb7  move: keep going");

    lumen_window_present(g.lwin);
}

/* ── Input ────────────────────────────────────────────────────────────── */
static void feed_key(char k)
{
    switch (k) {
    case KEY_UP:    case 'w': case 'W': handle_move(0); break;
    case KEY_DOWN:  case 's': case 'S': handle_move(1); break;
    case KEY_LEFT:  case 'a': case 'A': handle_move(2); break;
    case KEY_RIGHT: case 'd': case 'D': handle_move(3); break;
    case 'r': case 'R': new_game(); g.dirty = 1; break;
    default: break;
    }
}

/* ── Main ─────────────────────────────────────────────────────────────── */
int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    g.lfd = lumen_connect_retry();
    if (g.lfd < 0) { dprintf(2, "[2048] lumen_connect failed (%d)\n", g.lfd); return 1; }

    g.lwin = lumen_window_create(g.lfd, "2048", WIN_W, WIN_H);
    if (!g.lwin) { dprintf(2, "[2048] window_create failed\n"); close(g.lfd); return 1; }

    g.fb_w = g.lwin->w; g.fb_h = g.lwin->h;
    g.surf = (surface_t){ .buf = (uint32_t *)g.lwin->backbuf,
                          .w = g.fb_w, .h = g.fb_h, .pitch = g.lwin->stride };
    font_init();

    struct sigaction sa; memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigterm_handler; sigaction(SIGTERM, &sa, NULL);

    rng_seed();
    new_game();
    g.dirty = 1;
    render();
    dprintf(2, "[2048] connected %dx%d\n", g.lwin->w, g.lwin->h);

    while (!s_term && !g.done) {
        lumen_event_t ev;
        int r = lumen_wait_event(g.lfd, &ev, 100);
        if (r < 0) break;
        if (r == 1) {
            if (ev.type == LUMEN_EV_CLOSE_REQUEST) break;
            if (ev.type == LUMEN_EV_KEY && ev.key.pressed) {
                char k = (char)ev.key.keycode;
                if (k == KEY_ESC) break;
                feed_key(k);
            }
        }
        render();
    }

    lumen_window_destroy(g.lwin);
    close(g.lfd);
    dprintf(2, "[2048] exit\n");
    return 0;
}
