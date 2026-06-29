/* user/bin/calendar/main.c — Calendar, a month view with per-day notes
 * (external Lumen client).
 *
 * Shows the current month as a grid; today is highlighted, days with a saved
 * note are marked. Arrow keys (or clicks) move the selection, ‹/› (or PgUp/Dn)
 * change month, and typing edits the selected day's note — Enter saves it to
 * $HOME/.calendar (one "YYYY-MM-DD note" line per day). A live clock/date sits
 * in the header.
 *
 * Keys: arrows move, PgUp/PgDn (or [ ]) prev/next month, type to edit the note,
 * Enter saves, Esc reverts the edit, Q/close quits.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <stdint.h>
#include <fcntl.h>
#include <time.h>

#include <glyph.h>
#include <lumen_client.h>
#include "font.h"

#define WIN_W   476
#define WIN_H   430

#define GRID_X  14
#define GRID_Y  88
#define CELL_W  ((WIN_W - 2 * GRID_X) / 7)
#define CELL_H  44
#define ROWS    6

#define NOTE_Y  (GRID_Y + ROWS * CELL_H + 10)

#define KEY_UP    0xF1
#define KEY_DOWN  0xF2
#define KEY_RIGHT 0xF3
#define KEY_LEFT  0xF4

#define MAX_EVENTS 512

typedef struct { char date[11]; char note[120]; } event_t;

typedef struct {
    int      lfd;
    lumen_window_t *lwin;
    surface_t surf;
    int      w, h;

    int      year, month;       /* displayed month (month 1..12) */
    int      sel;               /* selected day 1..dim, 0 = none */
    int      ty, tm, td;        /* today */
    char     clockstr[20];

    event_t  ev[MAX_EVENTS];
    int      nev;
    char     edit[120];         /* edit buffer for the selected day */

    int      dirty;
} cal_t;

static cal_t g;
static volatile sig_atomic_t s_term;
static void on_term(int s) { (void)s; s_term = 1; }

/* ── Date math ────────────────────────────────────────────────────────────── */

static int is_leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

static int days_in_month(int y, int m)
{
    static const int d[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (m == 2 && is_leap(y)) return 29;
    return d[m - 1];
}

/* Day of week, 0 = Sunday (Sakamoto's algorithm). */
static int day_of_week(int y, int m, int d)
{
    static const int t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (m < 3) y -= 1;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

static const char *month_name(int m)
{
    static const char *n[] = { "January", "February", "March", "April", "May",
        "June", "July", "August", "September", "October", "November", "December" };
    return (m >= 1 && m <= 12) ? n[m - 1] : "?";
}

/* ── Persistence ($HOME/.calendar) ────────────────────────────────────────── */

static void cal_path(char *out, size_t n)
{
    const char *home = getenv("HOME");
    if (!home || !*home) home = "/root";
    snprintf(out, n, "%s/.calendar", home);
}

static void make_date(int y, int m, int d, char *out)
{
    snprintf(out, 11, "%04d-%02d-%02d", y, m, d);
}

static void events_load(void)
{
    g.nev = 0;
    char path[256]; cal_path(path, sizeof(path));
    int fd = open(path, O_RDONLY);
    if (fd < 0) return;
    static char buf[MAX_EVENTS * 132];
    int len = (int)read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (len <= 0) return;
    buf[len] = '\0';
    for (char *line = strtok(buf, "\n"); line && g.nev < MAX_EVENTS;
         line = strtok(NULL, "\n")) {
        if (strlen(line) < 12 || line[10] != ' ') continue;
        event_t *e = &g.ev[g.nev++];
        memcpy(e->date, line, 10); e->date[10] = '\0';
        snprintf(e->note, sizeof(e->note), "%s", line + 11);
    }
}

static void events_save(void)
{
    char path[256]; cal_path(path, sizeof(path));
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    for (int i = 0; i < g.nev; i++) {
        if (!g.ev[i].note[0]) continue;
        char line[140];
        int n = snprintf(line, sizeof(line), "%s %s\n", g.ev[i].date, g.ev[i].note);
        if (write(fd, line, (size_t)n) < 0) break;
    }
    close(fd);
}

static const char *note_for(int y, int m, int d)
{
    char date[11]; make_date(y, m, d, date);
    for (int i = 0; i < g.nev; i++)
        if (strcmp(g.ev[i].date, date) == 0) return g.ev[i].note;
    return NULL;
}

/* Save `text` for (y,m,d): update, add, or (empty) remove. */
static void note_set(int y, int m, int d, const char *text)
{
    char date[11]; make_date(y, m, d, date);
    int idx = -1;
    for (int i = 0; i < g.nev; i++)
        if (strcmp(g.ev[i].date, date) == 0) { idx = i; break; }
    if (!text[0]) {                         /* delete */
        if (idx >= 0) g.ev[idx] = g.ev[--g.nev];
    } else {
        if (idx < 0) {
            if (g.nev >= MAX_EVENTS) return;
            idx = g.nev++;
            memcpy(g.ev[idx].date, date, 11);
        }
        snprintf(g.ev[idx].note, sizeof(g.ev[idx].note), "%s", text);
    }
    events_save();
}

/* Load the selected day's stored note into the edit buffer. */
static void load_edit(void)
{
    const char *n = (g.sel > 0) ? note_for(g.year, g.month, g.sel) : NULL;
    snprintf(g.edit, sizeof(g.edit), "%s", n ? n : "");
}

/* ── Rendering ────────────────────────────────────────────────────────────── */

static void cell_xy(int day, int *cx, int *cy)
{
    int first = day_of_week(g.year, g.month, 1);
    int idx = first + (day - 1);
    *cx = GRID_X + (idx % 7) * CELL_W;
    *cy = GRID_Y + (idx / 7) * CELL_H;
}

static int day_at(int mx, int my)
{
    if (mx < GRID_X || my < GRID_Y) return 0;
    int c = (mx - GRID_X) / CELL_W;
    int r = (my - GRID_Y) / CELL_H;
    if (c >= 7 || r >= ROWS) return 0;
    int first = day_of_week(g.year, g.month, 1);
    int day = r * 7 + c - first + 1;
    return (day >= 1 && day <= days_in_month(g.year, g.month)) ? day : 0;
}

static void render(void)
{
    if (!g.dirty) return;
    g.dirty = 0;
    surface_t *s = &g.surf;
    draw_fill_rect(s, 0, 0, g.w, g.h, THEME_SURFACE);

    /* Header: month + year (centered), live clock right, nav arrows. */
    char title[32];
    snprintf(title, sizeof(title), "%s %d", month_name(g.month), g.year);
    int tw = glyph_text_width(title);
    draw_text_ui(s, (g.w - tw) / 2, 16, title, THEME_TEXT);
    draw_text_ui(s, 16, 16, "‹", THEME_TEXT_DIM);
    draw_text_ui(s, g.w - 24, 16, "›", THEME_TEXT_DIM);
    if (g.clockstr[0]) {
        int cw = glyph_text_width(g.clockstr);
        draw_text_ui(s, (g.w - cw) / 2, 44, g.clockstr, THEME_TEXT_DIM);
    }

    /* Weekday labels. */
    static const char *wd[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    for (int i = 0; i < 7; i++) {
        int lw = glyph_text_width(wd[i]);
        draw_text_ui(s, GRID_X + i * CELL_W + (CELL_W - lw) / 2, 66,
                     wd[i], i == 0 || i == 6 ? THEME_ACCENT : THEME_TEXT_DIM);
    }

    /* Day cells. */
    int ndays = days_in_month(g.year, g.month);
    for (int d = 1; d <= ndays; d++) {
        int cx, cy; cell_xy(d, &cx, &cy);
        int today = (g.year == g.ty && g.month == g.tm && d == g.td);
        int seld  = (d == g.sel);
        if (seld)
            draw_rounded_rect(s, cx + 2, cy + 2, CELL_W - 4, CELL_H - 4, 6, THEME_ACCENT);
        else if (today)
            draw_blend_rounded_rect(s, cx + 2, cy + 2, CELL_W - 4, CELL_H - 4, 6,
                                    0x00FFFFFF, 28);
        char ds[4]; snprintf(ds, sizeof(ds), "%d", d);
        uint32_t col = seld ? THEME_TEXT_ON_ACCENT : THEME_TEXT;
        draw_text_ui(s, cx + 8, cy + 6, ds, col);
        if (note_for(g.year, g.month, d))   /* note marker dot */
            draw_circle_filled(s, cx + CELL_W - 9, cy + 9, 2,
                               seld ? THEME_TEXT_ON_ACCENT : THEME_ACCENT);
    }

    /* Note editor for the selected day. */
    if (g.sel > 0) {
        char lbl[48];
        snprintf(lbl, sizeof(lbl), "%s %d — note:", month_name(g.month), g.sel);
        draw_text_ui(s, GRID_X, NOTE_Y, lbl, THEME_TEXT_DIM);
        int fy = NOTE_Y + 20, fw = g.w - 2 * GRID_X;
        draw_rounded_rect(s, GRID_X, fy, fw, 30, 6, THEME_INPUT_BG);
        draw_rounded_outline(s, GRID_X, fy, fw, 30, 6, 1, THEME_BORDER);
        int ty = fy + (30 - glyph_text_height()) / 2;
        draw_text_ui(s, GRID_X + 10, ty, g.edit, THEME_TEXT);
        int cxx = GRID_X + 10 + glyph_text_width(g.edit);
        draw_fill_rect(s, cxx + 1, ty, 2, glyph_text_height(), THEME_ACCENT);
        draw_text_ui(s, GRID_X, fy + 36, "Enter saves · Esc reverts", THEME_TEXT_DIM);
    }

    lumen_window_present(g.lwin);
}

/* ── Actions ──────────────────────────────────────────────────────────────── */

static void select_day(int d)
{
    int nd = days_in_month(g.year, g.month);
    if (d < 1) d = 1;
    if (d > nd) d = nd;
    g.sel = d;
    load_edit();
    g.dirty = 1;
}

static void change_month(int delta)
{
    g.month += delta;
    while (g.month > 12) { g.month -= 12; g.year++; }
    while (g.month < 1)  { g.month += 12; g.year--; }
    int nd = days_in_month(g.year, g.month);
    if (g.sel > nd) g.sel = nd;
    if (g.sel < 1) g.sel = 1;
    load_edit();
    g.dirty = 1;
}

/* Returns 0 to quit, 1 to keep running. */
static int handle_key(uint8_t k)
{
    switch (k) {
    case KEY_LEFT:  if (g.sel > 1) select_day(g.sel - 1); else change_month(-1); return 1;
    case KEY_RIGHT: select_day(g.sel + 1); return 1;
    case KEY_UP:    if (g.sel > 7) select_day(g.sel - 7); return 1;
    case KEY_DOWN:  select_day(g.sel + 7); return 1;
    case '[':       change_month(-1); return 1;
    case ']':       change_month(1);  return 1;
    case '\r': case '\n':
        if (g.sel > 0) { note_set(g.year, g.month, g.sel, g.edit); g.dirty = 1; }
        return 1;
    case 0x1B:      load_edit(); return 1;          /* Esc reverts the edit */
    case '\b': case 0x7F: {
        int l = (int)strlen(g.edit);
        if (l > 0) { g.edit[l - 1] = '\0'; g.dirty = 1; }
        return 1;
    }
    default:
        if (k >= 0x20 && k < 0x7F) {
            int l = (int)strlen(g.edit);
            if (l < (int)sizeof(g.edit) - 1) { g.edit[l] = (char)k; g.edit[l + 1] = '\0'; g.dirty = 1; }
        }
        return 1;
    }
}

static void update_clock(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return;
    time_t local = ts.tv_sec + glyph_theme_tz_offset() * 60;
    struct tm *tm = gmtime(&local);
    if (!tm) return;
    g.ty = tm->tm_year + 1900;
    g.tm = tm->tm_mon + 1;
    g.td = tm->tm_mday;
    char ns[20];
    snprintf(ns, sizeof(ns), "%04d-%02d-%02d  %02d:%02d",
             g.ty, g.tm, g.td, tm->tm_hour, tm->tm_min);
    if (strcmp(ns, g.clockstr) != 0) {
        memcpy(g.clockstr, ns, sizeof(ns));
        g.dirty = 1;
    }
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    g.lfd = lumen_connect_retry();
    if (g.lfd < 0) { dprintf(2, "[CAL] lumen_connect failed\n"); return 1; }
    g.lwin = lumen_window_create(g.lfd, "Calendar", WIN_W, WIN_H);
    if (!g.lwin) { dprintf(2, "[CAL] window_create failed\n"); close(g.lfd); return 1; }
    g.w = g.lwin->w; g.h = g.lwin->h;
    g.surf = (surface_t){ .buf = (uint32_t *)g.lwin->backbuf,
                          .w = g.w, .h = g.h, .pitch = g.lwin->stride };

    font_init();
    events_load();
    update_clock();
    g.year = g.ty; g.month = g.tm;
    select_day(g.td);
    dprintf(2, "[CAL] connected %dx%d %s %d, %d notes\n",
            g.w, g.h, month_name(g.month), g.year, g.nev);

    struct sigaction sa = {0};
    sa.sa_handler = on_term; sigaction(SIGTERM, &sa, NULL);

    g.dirty = 1;
    render();

    while (!s_term) {
        lumen_event_t ev;
        int r = lumen_wait_event(g.lfd, &ev, 1000);
        update_clock();
        if (r < 0) break;
        if (r == 1) {
            if (ev.type == LUMEN_EV_CLOSE_REQUEST) break;
            else if (ev.type == LUMEN_EV_KEY && ev.key.pressed) {
                uint8_t k = (uint8_t)ev.key.keycode;
                if (k == 'q' || k == 'Q') break;
                if (!handle_key(k)) break;
            } else if (ev.type == LUMEN_EV_MOUSE &&
                       ev.mouse.evtype == LUMEN_MOUSE_DOWN && (ev.mouse.buttons & 1)) {
                int d = day_at(ev.mouse.x, ev.mouse.y);
                if (d) select_day(d);
                else if (ev.mouse.y < 40 && ev.mouse.x < 40) change_month(-1);
                else if (ev.mouse.y < 40 && ev.mouse.x > g.w - 40) change_month(1);
            }
        }
        render();
    }

    lumen_window_destroy(g.lwin);
    close(g.lfd);
    dprintf(2, "[CAL] exit\n");
    return 0;
}
