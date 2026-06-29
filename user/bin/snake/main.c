/* user/bin/snake/main.c — Snake for Aegis (external Lumen client)
 *
 * The classic grid Snake game, speaking the Lumen external window protocol
 * (same pattern as 2048 / calculator / settings). Pure userspace: a grid the
 * snake crawls around, food to eat, a growing tail, and a Glyph-rendered
 * board. Distributed as an /apps bundle — installs into /apps/snake and
 * appears in the launcher.
 *
 * Controls: arrow keys or WASD to steer, P to pause, R for a new game,
 * Esc/close to quit. Integer-only (no floating point — Aegis FPU is fragile).
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
#define WIN_W   480
#define WIN_H   520
#define MARGIN  16
#define HDR_H   56
#define GRID    20                                  /* GRID x GRID cells */
#define BOARD_W (WIN_W - 2 * MARGIN)                /* 448 */
#define CELL    (BOARD_W / GRID)                    /* 22  */
#define BOARD_PX (CELL * GRID)                      /* 440 */
#define BOARD_X ((WIN_W - BOARD_PX) / 2)            /* 20  */
#define BOARD_Y (HDR_H + MARGIN)                    /* 72  */
#define CELL_X(c) (BOARD_X + (c) * CELL)
#define CELL_Y(r) (BOARD_Y + (r) * CELL)

/* Timing: faster ticks as score climbs, clamped to a floor. */
#define TICK_START 150     /* ms per step at score 0 */
#define TICK_MIN   70      /* fastest the snake will go */
#define TICK_STEP  4       /* ms shaved per food eaten */

/* Synthetic arrow keycodes Lumen delivers to proxy windows. */
#define KEY_UP    ((char)0xF1)
#define KEY_DOWN  ((char)0xF2)
#define KEY_RIGHT ((char)0xF3)
#define KEY_LEFT  ((char)0xF4)
#define KEY_ESC   '\x1b'

/* ── Colors (XRGB) ────────────────────────────────────────────────────── */
#define C_BG      0x00141821   /* window background (dark slate) */
#define C_BOARD   0x001D2430   /* play field */
#define C_GRID    0x00222C3A   /* subtle grid checker */
#define C_SNAKE   0x0046D17A   /* body green */
#define C_HEAD    0x006FE89C   /* brighter head */
#define C_FOOD    0x00E5533B   /* food red */
#define C_TEXT    0x00E8E8F0
#define C_DIM     0x008A93A6
#define C_GOLD    0x00FEBC2E
#define C_OVERLAY 0x000B0E14

/* ── Directions ───────────────────────────────────────────────────────── */
enum { DIR_UP, DIR_DOWN, DIR_LEFT, DIR_RIGHT };

/* ── State ────────────────────────────────────────────────────────────── */
typedef struct { int x, y; } cell_t;

typedef struct {
    int             lfd;
    lumen_window_t *lwin;
    surface_t       surf;
    int             fb_w, fb_h;
    int             dirty, done;

    cell_t snake[GRID * GRID];  /* snake[0] = head */
    int    len;
    int    dir;                 /* current heading */
    int    next_dir;            /* heading queued from input this tick */
    cell_t food;
    long   score;
    long   best;
    int    over;
    int    paused;
    int    tick_ms;
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
    s_rng ^= (uint32_t)(uintptr_t)&g * 0x9E3779B9u; /* address entropy */
    s_rng ^= 0x12345678u;
    if (!s_rng) s_rng = 0xC0FFEEu;
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

/* ── Game logic ───────────────────────────────────────────────────────── */
static int cell_on_snake(int x, int y)
{
    for (int i = 0; i < g.len; i++)
        if (g.snake[i].x == x && g.snake[i].y == y) return 1;
    return 0;
}

static void place_food(void)
{
    int free_n = GRID * GRID - g.len;
    if (free_n <= 0) return;                 /* board full — you win, basically */
    /* Pick the k-th empty cell to guarantee an empty spot without looping. */
    int target = rng_below(free_n);
    int seen = 0;
    for (int y = 0; y < GRID; y++) {
        for (int x = 0; x < GRID; x++) {
            if (cell_on_snake(x, y)) continue;
            if (seen == target) { g.food.x = x; g.food.y = y; return; }
            seen++;
        }
    }
}

static void new_game(void)
{
    g.len = 3;
    int cx = GRID / 2, cy = GRID / 2;
    g.snake[0] = (cell_t){ cx,     cy };   /* head */
    g.snake[1] = (cell_t){ cx - 1, cy };
    g.snake[2] = (cell_t){ cx - 2, cy };
    g.dir = DIR_RIGHT;
    g.next_dir = DIR_RIGHT;
    g.score = 0;
    g.over = 0;
    g.paused = 0;
    g.tick_ms = TICK_START;
    place_food();
}

/* Reject an instant 180° reversal. */
static int opposite(int a, int b)
{
    return (a == DIR_UP    && b == DIR_DOWN)  ||
           (a == DIR_DOWN  && b == DIR_UP)    ||
           (a == DIR_LEFT  && b == DIR_RIGHT) ||
           (a == DIR_RIGHT && b == DIR_LEFT);
}

static void set_dir(int d)
{
    if (g.over || g.paused) return;
    if (opposite(g.dir, d)) return;   /* no suicide reversal */
    g.next_dir = d;
}

static void step(void)
{
    if (g.over || g.paused) return;

    g.dir = g.next_dir;
    int nx = g.snake[0].x, ny = g.snake[0].y;
    switch (g.dir) {
    case DIR_UP:    ny--; break;
    case DIR_DOWN:  ny++; break;
    case DIR_LEFT:  nx--; break;
    case DIR_RIGHT: nx++; break;
    }

    /* Wall collision. */
    if (nx < 0 || nx >= GRID || ny < 0 || ny >= GRID) { g.over = 1; g.dirty = 1; return; }

    int eating = (nx == g.food.x && ny == g.food.y);

    /* Self collision. The tail tip moves away unless we're growing, so it is
     * legal to step onto the current tail cell when not eating. */
    for (int i = 0; i < g.len; i++) {
        if (i == g.len - 1 && !eating) break;   /* tail will vacate */
        if (g.snake[i].x == nx && g.snake[i].y == ny) { g.over = 1; g.dirty = 1; return; }
    }

    /* Advance the body: shift cells back, then place the new head. */
    if (eating) {
        if (g.len < GRID * GRID) g.len++;
    }
    for (int i = g.len - 1; i > 0; i--) g.snake[i] = g.snake[i - 1];
    g.snake[0].x = nx;
    g.snake[0].y = ny;

    if (eating) {
        g.score += 10;
        if (g.score > g.best) g.best = g.score;
        if (g.tick_ms > TICK_MIN) {
            g.tick_ms -= TICK_STEP;
            if (g.tick_ms < TICK_MIN) g.tick_ms = TICK_MIN;
        }
        if (g.len >= GRID * GRID) g.over = 1;   /* board filled — perfect game */
        else place_food();
    }
    g.dirty = 1;
}

/* ── Render ───────────────────────────────────────────────────────────── */
static void draw_overlay_text(const char *msg, const char *sub)
{
    /* Dim the board, then center two lines of text over it. */
    draw_blend_rect(&g.surf, BOARD_X, BOARD_Y, BOARD_PX, BOARD_PX, C_OVERLAY, 180);

    int mw = text_w(34, msg);
    text_sz(34, BOARD_X + (BOARD_PX - mw) / 2, BOARD_Y + BOARD_PX / 2 - 36, msg, C_GOLD);
    int sw = text_w(16, sub);
    text_sz(16, BOARD_X + (BOARD_PX - sw) / 2, BOARD_Y + BOARD_PX / 2 + 8, sub, C_TEXT);
}

static void render(void)
{
    if (!g.dirty) return;
    g.dirty = 0;
    surface_t *s = &g.surf;

    /* Background. */
    draw_fill_rect(s, 0, 0, g.fb_w, g.fb_h, C_BG);

    /* Header: title + score / best. */
    text_sz(30, MARGIN, 12, "Snake", C_SNAKE);

    char buf[48];
    snprintf(buf, sizeof(buf), "Score %ld", g.score);
    int sw = text_w(18, buf);
    text_sz(18, WIN_W - MARGIN - sw, 12, buf, C_TEXT);
    snprintf(buf, sizeof(buf), "Best %ld", g.best);
    int bw = text_w(13, buf);
    text_sz(13, WIN_W - MARGIN - bw, 36, buf, C_DIM);

    /* Board background + faint checkerboard for depth. */
    draw_rounded_rect(s, BOARD_X - 2, BOARD_Y - 2, BOARD_PX + 4, BOARD_PX + 4, 6, C_BOARD);
    for (int y = 0; y < GRID; y++)
        for (int x = 0; x < GRID; x++)
            if (((x + y) & 1) == 0)
                draw_fill_rect(s, CELL_X(x), CELL_Y(y), CELL, CELL, C_GRID);

    /* Food (rounded). */
    draw_rounded_rect(s, CELL_X(g.food.x) + 2, CELL_Y(g.food.y) + 2,
                      CELL - 4, CELL - 4, 5, C_FOOD);

    /* Snake: body then head on top. */
    for (int i = g.len - 1; i >= 0; i--) {
        uint32_t col = (i == 0) ? C_HEAD : C_SNAKE;
        int x = CELL_X(g.snake[i].x), y = CELL_Y(g.snake[i].y);
        draw_rounded_rect(s, x + 1, y + 1, CELL - 2, CELL - 2, 4, col);
    }

    /* Footer hint. */
    const char *hint = "Arrows / WASD: move  \xb7  P: pause  \xb7  R: restart";
    int hw = text_w(13, hint);
    text_sz(13, BOARD_X + (BOARD_PX - hw) / 2, BOARD_Y + BOARD_PX + 10, hint, C_DIM);

    if (g.over) {
        snprintf(buf, sizeof(buf), "Score %ld  \xb7  press R to restart", g.score);
        draw_overlay_text("Game Over", buf);
    } else if (g.paused) {
        draw_overlay_text("Paused", "press P to resume");
    }

    lumen_window_present(g.lwin);
}

/* ── Input ────────────────────────────────────────────────────────────── */
static void feed_key(char k)
{
    switch (k) {
    case KEY_UP:    case 'w': case 'W': set_dir(DIR_UP);    break;
    case KEY_DOWN:  case 's': case 'S': set_dir(DIR_DOWN);  break;
    case KEY_LEFT:  case 'a': case 'A': set_dir(DIR_LEFT);  break;
    case KEY_RIGHT: case 'd': case 'D': set_dir(DIR_RIGHT); break;
    case 'p': case 'P':
        if (!g.over) { g.paused = !g.paused; g.dirty = 1; }
        break;
    case 'r': case 'R':
        new_game(); g.dirty = 1; break;
    default: break;
    }
}

/* ── Main ─────────────────────────────────────────────────────────────── */
int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    g.lfd = lumen_connect_retry();
    if (g.lfd < 0) { dprintf(2, "[snake] lumen_connect failed (%d)\n", g.lfd); return 1; }

    g.lwin = lumen_window_create(g.lfd, "Snake", WIN_W, WIN_H);
    if (!g.lwin) { dprintf(2, "[snake] window_create failed\n"); close(g.lfd); return 1; }

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
    dprintf(2, "[snake] connected %dx%d\n", g.lwin->w, g.lwin->h);

    while (!s_term && !g.done) {
        /* The wait timeout is the game tick; when paused/over we idle on
         * a long timeout so the snake doesn't move but input stays live. */
        int timeout = (g.over || g.paused) ? 200 : g.tick_ms;
        lumen_event_t ev;
        int r = lumen_wait_event(g.lfd, &ev, timeout);
        if (r < 0) break;
        if (r == 1) {
            if (ev.type == LUMEN_EV_CLOSE_REQUEST) break;
            if (ev.type == LUMEN_EV_KEY && ev.key.pressed) {
                char k = (char)ev.key.keycode;
                if (k == KEY_ESC) break;
                feed_key(k);
            }
            /* r == 1 means an event arrived, not a timeout — don't step yet. */
        } else {
            /* r == 0: timeout elapsed → advance one game tick. */
            step();
        }
        render();
    }

    lumen_window_destroy(g.lwin);
    close(g.lfd);
    dprintf(2, "[snake] exit\n");
    return 0;
}
