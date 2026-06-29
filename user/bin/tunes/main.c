/* user/bin/tunes/main.c — Tunes, a minimal WAV/MP3 player (external Lumen client)
 *
 * Opens the audio file given as argv[1] (the file manager spawns it on a
 * .wav/.mp3) and plays it through /dev/audio via libaudio, which streams with
 * backpressure so a track of any length plays. Playback runs in a forked child
 * (audio_play_file_async) so the window stays responsive; Stop kills it. A
 * SIGCHLD tells us when the track finishes on its own.
 *
 * Keys: Space/Enter play, S/Esc stop, Q/close quit. Or click the buttons.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <stdint.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/syscall.h>

#include <glyph.h>
#include <lumen_client.h>
#include "font.h"
#include "audio.h"

#define SYS_AUDIO_STOP 504

#define WIN_W   420
#define WIN_H   214

#define PROG_X  30
#define PROG_Y  54
#define PROG_W  (WIN_W - 2 * PROG_X)
#define PROG_H  6

#define BTN_W   150
#define BTN_H   48
#define PLAY_X  40
#define STOP_X  (PLAY_X + BTN_W + 40)
#define BTN_Y   100

enum { ST_READY, ST_PLAYING, ST_STOPPED, ST_DONE, ST_NOFILE };

typedef struct {
    int             lfd;
    lumen_window_t *lwin;
    surface_t       surf;
    int             w, h;

    char            path[512];
    char            name[160];
    int             has_file;

    pid_t           pid;            /* >0 while a playback child runs */
    int             status;
    int             dur_ms;         /* track length, 0 if unknown */
    long            start_ms;       /* monotonic ms at playback start */
    int             dirty;
} tunes_t;

/* Monotonic milliseconds. */
static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void fmt_time(int ms, char *out, int n)
{
    if (ms < 0) ms = 0;
    int s = ms / 1000;
    snprintf(out, n, "%d:%02d", s / 60, s % 60);
}

static tunes_t g_t;
static volatile sig_atomic_t s_term;
static volatile sig_atomic_t s_child;       /* a child exited (reap in main) */
static volatile sig_atomic_t s_stopping;    /* the exit was a user Stop */

static void on_term(int s) { (void)s; s_term = 1; }
static void on_chld(int s) { (void)s; s_child = 1; }

static void basename_of(const char *p, char *out, int n)
{
    const char *b = p;
    for (const char *s = p; *s; s++) if (*s == '/') b = s + 1;
    snprintf(out, n, "%s", b);
}

/* ── Rendering ────────────────────────────────────────────────────────────── */

static void button(int x, int y, const char *label, uint32_t bg, int enabled)
{
    surface_t *s = &g_t.surf;
    draw_rounded_rect(s, x, y, BTN_W, BTN_H, 8, enabled ? bg : THEME_SURFACE_2);
    int tw = glyph_text_width(label);
    draw_text_ui(s, x + (BTN_W - tw) / 2, y + (BTN_H - glyph_text_height()) / 2,
                 label, enabled ? THEME_TEXT : THEME_TEXT_DIM);
}

static void render(void)
{
    if (!g_t.dirty) return;
    g_t.dirty = 0;
    surface_t *s = &g_t.surf;

    draw_fill_rect(s, 0, 0, g_t.w, g_t.h, THEME_SURFACE);

    const char *title = g_t.has_file ? g_t.name : "No file — open a .wav or .mp3";
    int tw = glyph_text_width(title);
    draw_text_ui(s, (g_t.w - tw) / 2, 22, title,
                 g_t.has_file ? THEME_TEXT : THEME_TEXT_DIM);

    int playing = (g_t.status == ST_PLAYING);

    /* Progress bar + elapsed/total time (when a file is loaded). */
    if (g_t.has_file) {
        int elapsed = playing ? (int)(now_ms() - g_t.start_ms) : 0;
        if (g_t.status == ST_DONE) elapsed = g_t.dur_ms;
        if (g_t.dur_ms > 0 && elapsed > g_t.dur_ms) elapsed = g_t.dur_ms;

        draw_rounded_rect(s, PROG_X, PROG_Y, PROG_W, PROG_H, PROG_H / 2, THEME_SURFACE_2);
        if (g_t.dur_ms > 0) {
            int fillw = (int)((long long)PROG_W * elapsed / g_t.dur_ms);
            if (fillw > 0)
                draw_rounded_rect(s, PROG_X, PROG_Y, fillw, PROG_H, PROG_H / 2, THEME_ACCENT);
        }
        char te[12], tt[12], line[32];
        fmt_time(elapsed, te, sizeof(te));
        fmt_time(g_t.dur_ms, tt, sizeof(tt));
        snprintf(line, sizeof(line), g_t.dur_ms > 0 ? "%s / %s" : "%s", te, tt);
        draw_text_ui(s, PROG_X, PROG_Y + PROG_H + 6, line, THEME_TEXT_DIM);
    }

    button(PLAY_X, BTN_Y, playing ? "Playing" : "Play",
           THEME_ACCENT, g_t.has_file && !playing);
    button(STOP_X, BTN_Y, "Stop", THEME_SURFACE_2, playing);

    const char *st =
        g_t.status == ST_PLAYING ? "Playing…" :
        g_t.status == ST_STOPPED ? "Stopped" :
        g_t.status == ST_DONE    ? "Finished" :
        g_t.status == ST_NOFILE  ? "Nothing to play" : "Ready";
    int sw = glyph_text_width(st);
    draw_text_ui(s, (g_t.w - sw) / 2, BTN_Y + BTN_H + 26, st, THEME_TEXT_DIM);

    lumen_window_present(g_t.lwin);
}

/* ── Actions ──────────────────────────────────────────────────────────────── */

static void do_play(void)
{
    if (!g_t.has_file || g_t.status == ST_PLAYING) return;
    g_t.dur_ms = audio_duration_ms(g_t.path);
    pid_t pid = audio_play_file_async(g_t.path);
    if (pid < 0) return;
    g_t.pid = pid;
    g_t.start_ms = now_ms();
    g_t.status = ST_PLAYING;
    g_t.dirty = 1;
    dprintf(2, "[TUNES] play %s pid=%d dur=%dms\n", g_t.path, (int)pid, g_t.dur_ms);
}

static void do_stop(void)
{
    if (g_t.status != ST_PLAYING || g_t.pid <= 0) return;
    s_stopping = 1;
    kill(g_t.pid, SIGKILL);          /* stop the producer */
    syscall(SYS_AUDIO_STOP);         /* halt the DMA now (no drain tail) */
    dprintf(2, "[TUNES] stop pid=%d\n", (int)g_t.pid);
}

static int in_btn(int mx, int my, int bx)
{
    return mx >= bx && mx < bx + BTN_W && my >= BTN_Y && my < BTN_Y + BTN_H;
}

/* Returns 0 to quit, 1 to keep running. */
static int handle_key(uint8_t k)
{
    switch (k) {
    case 'q': case 'Q':            return 0;
    case 0x1B:                     do_stop(); return 1;   /* Esc */
    case ' ': case '\r': case '\n': do_play(); return 1;
    case 's': case 'S':            do_stop(); return 1;
    default:                       return 1;
    }
}

int main(int argc, char **argv)
{
    if (argc > 1 && argv[1][0]) {
        snprintf(g_t.path, sizeof(g_t.path), "%s", argv[1]);
        basename_of(g_t.path, g_t.name, sizeof(g_t.name));
        g_t.has_file = 1;
        g_t.status = ST_READY;
    } else {
        g_t.status = ST_NOFILE;
    }

    g_t.lfd = lumen_connect_retry();
    if (g_t.lfd < 0) { dprintf(2, "[TUNES] lumen_connect failed\n"); return 1; }

    g_t.lwin = lumen_window_create(g_t.lfd, "Tunes", WIN_W, WIN_H);
    if (!g_t.lwin) { dprintf(2, "[TUNES] window_create failed\n"); close(g_t.lfd); return 1; }
    g_t.w = g_t.lwin->w; g_t.h = g_t.lwin->h;
    g_t.surf = (surface_t){ .buf = (uint32_t *)g_t.lwin->backbuf,
                            .w = g_t.w, .h = g_t.h, .pitch = g_t.lwin->stride };

    font_init();
    dprintf(2, "[TUNES] connected %dx%d file=%s\n",
            g_t.w, g_t.h, g_t.has_file ? g_t.name : "(none)");

    struct sigaction sa = {0};
    sa.sa_handler = on_term; sigaction(SIGTERM, &sa, NULL);
    sa.sa_handler = on_chld; sigaction(SIGCHLD, &sa, NULL);

    /* Auto-play when launched on a file (the file-manager open path). */
    if (g_t.has_file) do_play();

    g_t.dirty = 1;
    render();

    while (!s_term) {
        lumen_event_t ev;
        int r = lumen_wait_event(g_t.lfd, &ev, 300);

        if (s_child) {                 /* playback child exited */
            s_child = 0;
            while (waitpid(-1, NULL, WNOHANG) > 0) { }
            if (g_t.status == ST_PLAYING) {
                g_t.status = s_stopping ? ST_STOPPED : ST_DONE;
                g_t.pid = 0;
                s_stopping = 0;
                g_t.dirty = 1;
            }
        }

        if (r < 0) break;
        if (r == 1) {
            if (ev.type == LUMEN_EV_CLOSE_REQUEST) break;
            else if (ev.type == LUMEN_EV_KEY && ev.key.pressed) {
                if (!handle_key((uint8_t)ev.key.keycode)) break;
            } else if (ev.type == LUMEN_EV_MOUSE &&
                       ev.mouse.evtype == LUMEN_MOUSE_DOWN && (ev.mouse.buttons & 1)) {
                if (in_btn(ev.mouse.x, ev.mouse.y, PLAY_X)) do_play();
                else if (in_btn(ev.mouse.x, ev.mouse.y, STOP_X)) do_stop();
            }
        }
        if (g_t.status == ST_PLAYING)
            g_t.dirty = 1;           /* advance the progress bar each tick */
        render();
    }

    do_stop();                         /* don't leave audio playing after quit */
    lumen_window_destroy(g_t.lwin);
    close(g_t.lfd);
    dprintf(2, "[TUNES] exit\n");
    return 0;
}
