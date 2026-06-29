/* user/bin/run/main.c — Aegis "Run…" launcher (external Lumen client)
 *
 * A small chromed window with a single text field. Type an application
 * name; the field live-matches it against the /apps bundle registry
 * (case-insensitive prefix on id or display name) and Enter asks Lumen to
 * spawn the match via LUMEN_OP_INVOKE, then the dialog exits. Esc or the
 * close button dismisses.
 *
 * ponytail: launches GUI app bundles only — that's what Lumen's invoke
 * mechanism resolves. CLI commands need a terminal (open /apps/terminal),
 * so there's deliberately no arbitrary-command path here.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <stdint.h>

#include <glyph.h>
#include <lumen_client.h>
#include "font.h"
#include "apps.h"

#define WIN_W   460
#define WIN_H   120

#define FIELD_X 16
#define FIELD_Y 42
#define FIELD_H 34
#define PAD     12

#define KEY_ESC 0x1B
#define KEY_BS1 0x08
#define KEY_BS2 0x7F

typedef struct {
    int             lfd;
    lumen_window_t *lwin;
    surface_t       surf;
    int             w, h;

    glyph_app_t     apps[GLYPH_APPS_MAX];
    int             count;

    char            text[64];
    int             len;
    int             match;        /* index into apps[], -1 = none */
    int             dirty;
} run_state_t;

static run_state_t g_st;
static volatile sig_atomic_t s_term;
static void sigterm_handler(int s) { (void)s; s_term = 1; }

static char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

/* Case-insensitive: does s start with prefix? Empty prefix never matches. */
static int ci_starts_with(const char *s, const char *prefix)
{
    if (!*prefix) return 0;
    for (; *prefix; s++, prefix++)
        if (!*s || lc(*s) != lc(*prefix)) return 0;
    return 1;
}

/* First app whose id or display name prefix-matches the typed text, or -1. */
static int find_match(void)
{
    for (int i = 0; i < g_st.count; i++)
        if (ci_starts_with(g_st.apps[i].id, g_st.text) ||
            ci_starts_with(g_st.apps[i].name, g_st.text))
            return i;
    return -1;
}

static void render(void)
{
    if (!g_st.dirty) return;
    g_st.dirty = 0;
    surface_t *s = &g_st.surf;

    draw_fill_rect(s, 0, 0, g_st.w, g_st.h, THEME_SURFACE);
    draw_text_ui(s, FIELD_X, 14, "Run application", C_SUBTLE);

    /* Input field. */
    int fw = g_st.w - 2 * FIELD_X;
    draw_rounded_rect(s, FIELD_X, FIELD_Y, fw, FIELD_H, 6, THEME_INPUT_BG);
    draw_rounded_outline(s, FIELD_X, FIELD_Y, fw, FIELD_H, 6, 1, THEME_BORDER);

    int ty = FIELD_Y + (FIELD_H - glyph_text_height()) / 2;
    int tx = FIELD_X + PAD;
    if (g_st.len)
        draw_text_ui(s, tx, ty, g_st.text, THEME_TEXT);
    /* Caret after the text. */
    int cx = tx + glyph_text_width(g_st.text);
    draw_fill_rect(s, cx + 1, ty, 2, glyph_text_height(), THEME_ACCENT);

    /* Hint / match line. */
    int hy = FIELD_Y + FIELD_H + 12;
    if (g_st.len == 0)
        draw_text_ui(s, FIELD_X, hy, "Type a name, press Enter to launch", C_SUBTLE);
    else if (g_st.match >= 0) {
        char line[96];
        snprintf(line, sizeof(line), "\xE2\x86\xB5 %s", g_st.apps[g_st.match].name);
        draw_text_ui(s, FIELD_X, hy, line, THEME_ACCENT);
    } else
        draw_text_ui(s, FIELD_X, hy, "No matching application", THEME_TEXT_DIM);

    lumen_window_present(g_st.lwin);
}

/* Returns 0 to exit (launched or dismissed), 1 to keep running. */
static int handle_key(uint8_t k)
{
    if (k == KEY_ESC) return 0;
    if (k == '\r' || k == '\n') {
        if (g_st.match >= 0) {
            lumen_invoke(g_st.lfd, g_st.apps[g_st.match].id);
            dprintf(2, "[RUN] launch %s\n", g_st.apps[g_st.match].id);
            return 0;
        }
        return 1;                       /* no match: ignore Enter */
    }
    if (k == KEY_BS1 || k == KEY_BS2) {
        if (g_st.len) g_st.text[--g_st.len] = '\0';
    } else if (k >= 0x20 && k < 0x7F && g_st.len < (int)sizeof(g_st.text) - 1) {
        g_st.text[g_st.len++] = (char)k;
        g_st.text[g_st.len] = '\0';
    } else {
        return 1;                       /* arrows / other: no-op, no redraw */
    }
    g_st.match = find_match();
    g_st.dirty = 1;
    return 1;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    g_st.lfd = lumen_connect_retry();
    if (g_st.lfd < 0) {
        dprintf(2, "[RUN] lumen_connect failed (%d)\n", g_st.lfd);
        return 1;
    }

    g_st.lwin = lumen_window_create(g_st.lfd, "Run", WIN_W, WIN_H);
    if (!g_st.lwin) {
        dprintf(2, "[RUN] lumen_window_create failed\n");
        close(g_st.lfd);
        return 1;
    }
    g_st.w = g_st.lwin->w;
    g_st.h = g_st.lwin->h;
    g_st.surf = (surface_t){
        .buf = (uint32_t *)g_st.lwin->backbuf,
        .w = g_st.w, .h = g_st.h, .pitch = g_st.lwin->stride,
    };

    g_st.count = glyph_apps_scan(g_st.apps, GLYPH_APPS_MAX);
    if (g_st.count < 0) g_st.count = 0;
    g_st.match = -1;

    font_init();
    dprintf(2, "[RUN] connected %dx%d apps=%d\n", g_st.w, g_st.h, g_st.count);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigterm_handler;
    sigaction(SIGTERM, &sa, NULL);

    g_st.dirty = 1;
    render();

    while (!s_term) {
        lumen_event_t ev;
        int r = lumen_wait_event(g_st.lfd, &ev, 500);
        if (r < 0) break;
        if (r == 1) {
            if (ev.type == LUMEN_EV_CLOSE_REQUEST)
                break;
            if (ev.type == LUMEN_EV_KEY && ev.key.pressed &&
                !handle_key((uint8_t)ev.key.keycode))
                break;
        }
        render();
    }

    lumen_window_destroy(g_st.lwin);
    close(g_st.lfd);
    dprintf(2, "[RUN] exit\n");
    return 0;
}
