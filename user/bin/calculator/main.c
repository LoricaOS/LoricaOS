/* user/bin/calculator/main.c — Aegis Calculator (external Lumen client)
 *
 * A standalone four-function calculator speaking the Lumen external
 * window protocol (same pattern as settings / terminal / gui-installer).
 * Pure userspace logic: a button grid, a display, and a tiny two-operand
 * state machine over doubles. No new kernel support, no file I/O.
 *
 * Input works two ways: click the on-screen buttons, or type on the
 * keyboard (digits, + - * /, Enter/=, Backspace, Esc, 'c' to clear).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <stdint.h>

#include <glyph.h>
#include <lumen_client.h>
#include "font.h"

/* ── Layout (window is WIN_W x WIN_H) ─────────────────────────────────── */

#define WIN_W 260
#define WIN_H 360

#define PAD        12
#define DISP_X     PAD
#define DISP_Y     PAD
#define DISP_W     (WIN_W - 2 * PAD)
#define DISP_H     64

#define GRID_X     PAD
#define GRID_Y     (DISP_Y + DISP_H + PAD)
#define GRID_COLS  4
#define GRID_ROWS  5
#define CELL_GAP   8
#define CELL_W     ((WIN_W - 2 * PAD - (GRID_COLS - 1) * CELL_GAP) / GRID_COLS)
#define CELL_H     ((WIN_H - GRID_Y - PAD - (GRID_ROWS - 1) * CELL_GAP) / GRID_ROWS)

/* Synthetic arrow codes (unused here but kept for symmetry). */
#define KEY_ESC '\x1b'

/* ── Button table ─────────────────────────────────────────────────────── */

/* Each button carries a label and the single character it injects into
 * the same handler the keyboard uses, so mouse and keys share one path. */
typedef struct {
    const char *label;
    char        key;
    unsigned char kind;   /* color role — resolved to a (runtime-themed) color */
} calc_btn_t;

/* Color roles (compile-time constants so the button table stays static; the
 * actual colors — including the runtime-themed accent — are resolved by
 * kind_color() at draw time). */
#define COL_DIGIT  0
#define COL_OP     1
#define COL_FUNC   2
#define COL_EQ     3

static uint32_t kind_color(unsigned char k)
{
    switch (k) {
    case COL_OP:   return THEME_ACCENT;
    case COL_FUNC: return THEME_HOVER;
    case COL_EQ:   return THEME_ACCENT;
    default:       return THEME_SURFACE_2;   /* COL_DIGIT */
    }
}

static const calc_btn_t s_buttons[GRID_ROWS][GRID_COLS] = {
    { {"C", 'c', COL_FUNC}, {"<-", '\b', COL_FUNC}, {"%", '%', COL_FUNC}, {"/", '/', COL_OP} },
    { {"7", '7', COL_DIGIT}, {"8", '8', COL_DIGIT}, {"9", '9', COL_DIGIT}, {"*", '*', COL_OP} },
    { {"4", '4', COL_DIGIT}, {"5", '5', COL_DIGIT}, {"6", '6', COL_DIGIT}, {"-", '-', COL_OP} },
    { {"1", '1', COL_DIGIT}, {"2", '2', COL_DIGIT}, {"3", '3', COL_DIGIT}, {"+", '+', COL_OP} },
    { {"+/-", '~', COL_FUNC}, {"0", '0', COL_DIGIT}, {".", '.', COL_DIGIT}, {"=", '=', COL_EQ} },
};

/* ── State ────────────────────────────────────────────────────────────── */

typedef struct {
    int             lfd;
    lumen_window_t *lwin;
    surface_t       surf;
    int             fb_w, fb_h;
    int             dirty;
    int             done;

    double acc;          /* accumulator (left operand) */
    char   op;           /* pending operator, 0 = none */
    char   ent[32];      /* current entry being typed */
    int    fresh;        /* 1 = next digit starts a new entry */
    int    error;        /* 1 = show "Error" until cleared */

    int    pressed_r, pressed_c;  /* button visual feedback, -1 = none */
} calc_state_t;

static calc_state_t g_st;
static volatile sig_atomic_t s_term;
static void sigterm_handler(int s) { (void)s; s_term = 1; }

/* ── Drawing helpers (settings idioms) ────────────────────────────────── */

static void draw_text_sz(int sz, int x, int y, const char *s, uint32_t color)
{
    if (g_font_ui)
        font_draw_text(&g_st.surf, g_font_ui, sz, x, y, s, color);
    else
        draw_text_t(&g_st.surf, x, y, s, color);
}

static int text_w(int sz, const char *s)
{
    if (g_font_ui) return font_text_width(g_font_ui, sz, s);
    return (int)strlen(s) * FONT_W;
}

/* Build the display string: the live entry, or the accumulator. */
static void display_string(char *out, size_t cap)
{
    if (g_st.error) { snprintf(out, cap, "Error"); return; }
    if (g_st.ent[0]) { snprintf(out, cap, "%s", g_st.ent); return; }
    /* Format the accumulator without trailing-zero noise. */
    double v = g_st.acc;
    if (v == 0) { snprintf(out, cap, "0"); return; }
    snprintf(out, cap, "%.10g", v);
}

static void render(void)
{
    if (!g_st.dirty) return;
    g_st.dirty = 0;
    surface_t *s = &g_st.surf;

    draw_fill_rect(s, 0, 0, g_st.fb_w, g_st.fb_h, THEME_SURFACE);

    /* Display panel — right-aligned text. */
    draw_rounded_rect(s, DISP_X, DISP_Y, DISP_W, DISP_H, R_SM, THEME_INPUT_BG);
    draw_rounded_outline(s, DISP_X, DISP_Y, DISP_W, DISP_H, R_SM, 1, THEME_BORDER);
    char disp[48];
    display_string(disp, sizeof(disp));
    int tw = text_w(30, disp);
    if (tw > DISP_W - 16) {            /* fall back to smaller size if long */
        int dw = text_w(20, disp);
        draw_text_sz(20, DISP_X + DISP_W - 8 - dw, DISP_Y + (DISP_H - 22) / 2,
                     disp, THEME_TEXT);
    } else {
        draw_text_sz(30, DISP_X + DISP_W - 8 - tw, DISP_Y + (DISP_H - 32) / 2,
                     disp, THEME_TEXT);
    }

    /* Button grid. */
    for (int r = 0; r < GRID_ROWS; r++) {
        for (int c = 0; c < GRID_COLS; c++) {
            int x = GRID_X + c * (CELL_W + CELL_GAP);
            int y = GRID_Y + r * (CELL_H + CELL_GAP);
            const calc_btn_t *b = &s_buttons[r][c];
            uint32_t bg = kind_color(b->kind);
            if (r == g_st.pressed_r && c == g_st.pressed_c)
                bg = THEME_ACCENT;         /* press highlight */
            draw_rounded_rect(s, x, y, CELL_W, CELL_H, 6, bg);
            int lw = text_w(20, b->label);
            draw_text_sz(20, x + (CELL_W - lw) / 2, y + (CELL_H - 22) / 2,
                         b->label, 0x00FFFFFF);
        }
    }

    lumen_window_present(g_st.lwin);
}

/* ── Calculator logic ─────────────────────────────────────────────────── */

static double entry_value(void)
{
    return g_st.ent[0] ? atof(g_st.ent) : g_st.acc;
}

static void apply_pending(void)
{
    double rhs = entry_value();
    switch (g_st.op) {
    case '+': g_st.acc += rhs; break;
    case '-': g_st.acc -= rhs; break;
    case '*': g_st.acc *= rhs; break;
    case '/':
        if (rhs == 0) { g_st.error = 1; g_st.op = 0; g_st.ent[0] = '\0'; return; }
        g_st.acc /= rhs;
        break;
    default:  g_st.acc = rhs; break;   /* no pending op: adopt entry */
    }
    g_st.op = 0;
    g_st.ent[0] = '\0';
}

static void input_digit(char d)
{
    if (g_st.error) { g_st.error = 0; g_st.acc = 0; g_st.ent[0] = '\0'; }
    if (g_st.fresh) { g_st.ent[0] = '\0'; g_st.fresh = 0; }
    int len = (int)strlen(g_st.ent);
    if (d == '.') {
        if (strchr(g_st.ent, '.')) return;           /* one dot only */
        if (len == 0) { strcpy(g_st.ent, "0"); len = 1; }
    }
    if (len < (int)sizeof(g_st.ent) - 1) {
        g_st.ent[len] = d;
        g_st.ent[len + 1] = '\0';
    }
}

static void input_op(char o)
{
    if (g_st.error) return;
    /* Chain: if an op is already pending and the user typed a fresh
     * entry, fold it before starting the next operation. */
    if (g_st.op && !g_st.fresh)
        apply_pending();
    else if (!g_st.op)
        g_st.acc = entry_value();
    if (g_st.error) return;
    g_st.op = o;
    g_st.ent[0] = '\0';
    g_st.fresh = 1;
}

static void input_equals(void)
{
    if (g_st.error || !g_st.op) return;
    apply_pending();
    g_st.fresh = 1;
}

static void input_clear(void)
{
    g_st.acc = 0;
    g_st.op = 0;
    g_st.ent[0] = '\0';
    g_st.fresh = 0;
    g_st.error = 0;
}

static void input_backspace(void)
{
    if (g_st.error) { input_clear(); return; }
    int len = (int)strlen(g_st.ent);
    if (len > 0) g_st.ent[len - 1] = '\0';
}

static void input_percent(void)
{
    if (g_st.error) return;
    double v = entry_value() / 100.0;
    snprintf(g_st.ent, sizeof(g_st.ent), "%.10g", v);
    g_st.fresh = 0;
}

static void input_negate(void)
{
    if (g_st.error) return;
    double v = -entry_value();
    snprintf(g_st.ent, sizeof(g_st.ent), "%.10g", v);
    g_st.fresh = 0;
}

/* Single dispatch shared by keyboard and on-screen buttons. */
static void feed_key(char k)
{
    if ((k >= '0' && k <= '9') || k == '.')      input_digit(k);
    else if (k == '+' || k == '-' || k == '*' || k == '/') input_op(k);
    else if (k == '=' || k == '\r' || k == '\n') input_equals();
    else if (k == 'c' || k == 'C')               input_clear();
    else if (k == '\b' || k == 127)              input_backspace();
    else if (k == '%')                           input_percent();
    else if (k == '~')                           input_negate();
    else return;
    g_st.dirty = 1;
}

/* ── Mouse ────────────────────────────────────────────────────────────── */

static int hit_button(int x, int y, int *out_r, int *out_c)
{
    for (int r = 0; r < GRID_ROWS; r++) {
        for (int c = 0; c < GRID_COLS; c++) {
            int bx = GRID_X + c * (CELL_W + CELL_GAP);
            int by = GRID_Y + r * (CELL_H + CELL_GAP);
            if (x >= bx && x < bx + CELL_W && y >= by && y < by + CELL_H) {
                *out_r = r; *out_c = c; return 1;
            }
        }
    }
    return 0;
}

/* ── Main ─────────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    g_st.lfd = lumen_connect_retry();
    if (g_st.lfd < 0) {
        dprintf(2, "[CALC] lumen_connect failed (%d)\n", g_st.lfd);
        return 1;
    }

    g_st.lwin = lumen_window_create(g_st.lfd, "Calculator", WIN_W, WIN_H);
    if (!g_st.lwin) {
        dprintf(2, "[CALC] lumen_window_create failed\n");
        close(g_st.lfd);
        return 1;
    }
    g_st.fb_w = g_st.lwin->w;
    g_st.fb_h = g_st.lwin->h;
    g_st.surf = (surface_t){
        .buf = (uint32_t *)g_st.lwin->backbuf,
        .w = g_st.fb_w, .h = g_st.fb_h, .pitch = g_st.lwin->stride,
    };

    font_init();

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigterm_handler;
    sigaction(SIGTERM, &sa, NULL);

    g_st.pressed_r = g_st.pressed_c = -1;
    input_clear();
    g_st.dirty = 1;
    render();

    dprintf(2, "[CALC] connected %dx%d\n", g_st.lwin->w, g_st.lwin->h);

    while (!s_term && !g_st.done) {
        lumen_event_t ev;
        int r = lumen_wait_event(g_st.lfd, &ev, 16);
        if (r < 0) break;

        if (r == 1) {
            if (ev.type == LUMEN_EV_CLOSE_REQUEST) break;
            if (ev.type == LUMEN_EV_KEY && ev.key.pressed) {
                char k = (char)ev.key.keycode;
                if (k == KEY_ESC) break;
                feed_key(k);
            }
            if (ev.type == LUMEN_EV_MOUSE) {
                int rr, cc;
                if (ev.mouse.evtype == LUMEN_MOUSE_DOWN &&
                    (ev.mouse.buttons & 1) &&
                    hit_button(ev.mouse.x, ev.mouse.y, &rr, &cc)) {
                    g_st.pressed_r = rr; g_st.pressed_c = cc;
                    feed_key(s_buttons[rr][cc].key);
                    g_st.dirty = 1;
                } else if (ev.mouse.evtype == LUMEN_MOUSE_UP) {
                    if (g_st.pressed_r >= 0) {
                        g_st.pressed_r = g_st.pressed_c = -1;
                        g_st.dirty = 1;
                    }
                }
            }
        }
        render();
    }

    lumen_window_destroy(g_st.lwin);
    close(g_st.lfd);
    dprintf(2, "[CALC] exit\n");
    return 0;
}
