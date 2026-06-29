/* user/bin/sysmon/main.c — Aegis System Monitor (external Lumen client)
 *
 * A standalone task/resource monitor speaking the Lumen external window
 * protocol (same pattern as calculator / settings / terminal). It reads
 * /proc directly — no new kernel support:
 *
 *   - /proc/meminfo            MemTotal / MemFree / MemAvailable  (a bar)
 *   - CLOCK_MONOTONIC          uptime "up Xh Ym Zs"
 *   - readdir("/proc")         the all-digit entries are pids
 *   - /proc/<pid>/status       Name / State / VmSize  (the table)
 *
 * NOTE on the size column: Aegis procfs exposes VmSize (sum of VMA lengths
 * in kB) but NOT VmRSS — there is no resident-set accounting yet. ps reads
 * VmSize, so this tool mirrors that and labels the column MEM(KB). The list
 * is sorted by VmSize descending (PID ascending on ties / no size).
 *
 * Refresh is ~1 Hz: lumen_wait_event with a 1000 ms timeout re-scans /proc
 * on timeout; any input also forces a re-render. Selection moves with the
 * arrow keys (raw or Lumen's synthetic 0xF1/0xF2), a left click selects a
 * row, the wheel and scrollbar scroll the list, and 'k' sends SIGTERM to
 * the selected pid (pid 0/1 are guarded).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <stdint.h>
#include <ctype.h>
#include <dirent.h>
#include <time.h>

#include <glyph.h>
#include <lumen_client.h>
#include "font.h"

/* ── Layout ───────────────────────────────────────────────────────────── */

#define WIN_W 720
#define WIN_H 560

#define PAD        14
#define BG_COLOR   0x00141418   /* off-key near-black (NOT C_TERM_BG) */

/* Memory bar */
#define BAR_X      PAD
#define BAR_Y      40
#define BAR_W      (WIN_W - 2 * PAD)
#define BAR_H      28

/* Process table */
#define TBL_X      PAD
#define TBL_Y      150
#define TBL_W      (WIN_W - 2 * PAD)
#define ROW_H      22
#define HDR_H      24
#define SCROLLBAR_W 10

/* Column x-offsets within the table (pixels from TBL_X) */
#define COL_PID    0
#define COL_NAME   70
#define COL_STATE  340
#define COL_MEM    430

/* Synthetic / raw key codes */
#define KEY_ESC      '\x1b'
#define KEY_UP_SYN   0xF1
#define KEY_DOWN_SYN 0xF2
#define KEY_UP_RAW   0x11   /* fallback raw arrow codes some paths use */
#define KEY_DOWN_RAW 0x12

#define MAX_PROCS 256

/* ── Process record ───────────────────────────────────────────────────── */

typedef struct {
    int  pid;
    char state;
    long vsz_kb;
    char name[48];
} proc_row_t;

/* ── State ────────────────────────────────────────────────────────────── */

typedef struct {
    int             lfd;
    lumen_window_t *lwin;
    surface_t       surf;
    int             fb_w, fb_h;
    int             dirty;
    int             done;

    proc_row_t      procs[MAX_PROCS];
    int             nprocs;
    int             truncated;

    long            mem_total_kb;
    long            mem_used_kb;

    int             sel;        /* selected row index, -1 = none */
    int             scroll;     /* first visible row index */
    int             rows_vis;   /* rows that fit in the table viewport */
} sysmon_state_t;

static sysmon_state_t g_st;
static volatile sig_atomic_t s_term;
static void sigterm_handler(int s) { (void)s; s_term = 1; }

/* ── /proc parsing (mirrors ps/free) ──────────────────────────────────── */

/* Find a "Key:\tvalue" field in an already-read status buffer. */
static const char *status_field(const char *buf, const char *key)
{
    const char *p = buf;
    size_t klen = strlen(key);
    while (p && *p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == ':') {
            p += klen + 1;
            while (*p == ' ' || *p == '\t') p++;
            return p;
        }
        p = strchr(p, '\n');
        if (p) p++;
    }
    return NULL;
}

static int read_proc(int pid, proc_row_t *out)
{
    char path[64], buf[1024];
    snprintf(path, sizeof(path), "/proc/%d/status", pid);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    memset(out, 0, sizeof(*out));
    out->pid = pid;
    out->state = '?';

    const char *v;
    if ((v = status_field(buf, "Name"))) {
        size_t i = 0;
        while (v[i] && v[i] != '\n' && i < sizeof(out->name) - 1) {
            out->name[i] = v[i];
            i++;
        }
        out->name[i] = '\0';
    }
    if (!out->name[0])
        snprintf(out->name, sizeof(out->name), "?");
    if ((v = status_field(buf, "State")))  out->state  = *v;
    if ((v = status_field(buf, "VmSize"))) out->vsz_kb = atol(v);
    return 0;
}

/* Sort by VmSize descending; PID ascending on ties (or when both zero). */
static int cmp_proc(const void *a, const void *b)
{
    const proc_row_t *pa = (const proc_row_t *)a;
    const proc_row_t *pb = (const proc_row_t *)b;
    if (pa->vsz_kb != pb->vsz_kb)
        return (pb->vsz_kb > pa->vsz_kb) ? 1 : -1;
    return pa->pid - pb->pid;
}

static void scan_procs(void)
{
    int sel_pid = (g_st.sel >= 0 && g_st.sel < g_st.nprocs)
                  ? g_st.procs[g_st.sel].pid : -1;

    g_st.nprocs = 0;
    g_st.truncated = 0;

    DIR *d = opendir("/proc");
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (!isdigit((unsigned char)de->d_name[0]))
                continue;
            /* all-digit check */
            int ok = 1;
            for (const char *c = de->d_name; *c; c++) {
                if (!isdigit((unsigned char)*c)) { ok = 0; break; }
            }
            if (!ok) continue;
            int pid = atoi(de->d_name);
            if (pid <= 0) continue;
            if (g_st.nprocs >= MAX_PROCS) { g_st.truncated = 1; break; }
            if (read_proc(pid, &g_st.procs[g_st.nprocs]) == 0)
                g_st.nprocs++;
        }
        closedir(d);
    }
    if (g_st.truncated)
        dprintf(2, "[SYSMON] process table truncated at %d\n", MAX_PROCS);

    qsort(g_st.procs, (size_t)g_st.nprocs, sizeof(proc_row_t), cmp_proc);

    /* Keep the same process selected across a re-sort, if it survives. */
    g_st.sel = -1;
    if (sel_pid >= 0) {
        for (int i = 0; i < g_st.nprocs; i++) {
            if (g_st.procs[i].pid == sel_pid) { g_st.sel = i; break; }
        }
    }
}

static void scan_meminfo(void)
{
    char buf[512];
    g_st.mem_total_kb = 0;
    g_st.mem_used_kb = 0;

    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return;
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    const char *p;
    long total = -1, freek = -1;
    if ((p = status_field(buf, "MemTotal"))) total = atol(p);
    if ((p = status_field(buf, "MemFree")))  freek = atol(p);
    if (total < 0) total = 0;
    if (freek < 0) freek = 0;
    g_st.mem_total_kb = total;
    g_st.mem_used_kb = (total > freek) ? (total - freek) : 0;
}

/* ── Drawing ──────────────────────────────────────────────────────────── */

static int text_w(const char *s)
{
    return glyph_text_width(s);
}

static void clamp_scroll(void)
{
    int max_scroll = g_st.nprocs - g_st.rows_vis;
    if (max_scroll < 0) max_scroll = 0;
    if (g_st.scroll > max_scroll) g_st.scroll = max_scroll;
    if (g_st.scroll < 0) g_st.scroll = 0;
}

/* Keep the selected row visible. */
static void scroll_to_sel(void)
{
    if (g_st.sel < 0) return;
    if (g_st.sel < g_st.scroll)
        g_st.scroll = g_st.sel;
    else if (g_st.sel >= g_st.scroll + g_st.rows_vis)
        g_st.scroll = g_st.sel - g_st.rows_vis + 1;
    clamp_scroll();
}

static void render(void)
{
    if (!g_st.dirty) return;
    g_st.dirty = 0;
    surface_t *s = &g_st.surf;

    draw_fill_rect(s, 0, 0, g_st.fb_w, g_st.fb_h, BG_COLOR);

    /* ── Title ─────────────────────────────────────────────────────── */
    draw_text_ui(s, PAD, 10, "System Monitor", C_WIN);

    /* ── Memory bar ────────────────────────────────────────────────── */
    char memlbl[96];
    long total_mb = g_st.mem_total_kb / 1024;
    long used_mb = g_st.mem_used_kb / 1024;
    int pct = (g_st.mem_total_kb > 0)
              ? (int)((g_st.mem_used_kb * 100) / g_st.mem_total_kb) : 0;
    snprintf(memlbl, sizeof(memlbl), "Memory: %ld / %ld MB (%d%%)",
             used_mb, total_mb, pct);
    draw_text_ui(s, BAR_X, BAR_Y - 20, memlbl, C_TEXT);

    draw_fill_rect(s, BAR_X, BAR_Y, BAR_W, BAR_H, C_INPUT_BG);
    int fill = (g_st.mem_total_kb > 0)
               ? (int)(((long)BAR_W * g_st.mem_used_kb) / g_st.mem_total_kb)
               : 0;
    if (fill < 0) fill = 0;
    if (fill > BAR_W) fill = BAR_W;
    uint32_t bar_col = (pct >= 90) ? C_RED
                     : (pct >= 70) ? C_YELLOW : C_GREEN;
    if (fill > 0)
        draw_fill_rect(s, BAR_X, BAR_Y, fill, BAR_H, bar_col);
    draw_rect(s, BAR_X, BAR_Y, BAR_W, BAR_H, C_BORDER);

    /* ── Uptime + process count ────────────────────────────────────── */
    struct timespec ts;
    long up = 0;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        up = ts.tv_sec;
    long uh = up / 3600;
    long um = (up % 3600) / 60;
    long us = up % 60;
    char info[128];
    snprintf(info, sizeof(info), "up %ldh %ldm %lds      Processes: %d",
             uh, um, us, g_st.nprocs);
    draw_text_ui(s, PAD, BAR_Y + BAR_H + 14, info, C_SUBTLE);

    /* ── Process table header ──────────────────────────────────────── */
    int hx = TBL_X;
    int hy = TBL_Y;
    draw_fill_rect(s, TBL_X, hy, TBL_W, HDR_H, C_WIN_BG);
    draw_text_ui(s, hx + COL_PID,   hy + 3, "PID",     C_SUBTLE);
    draw_text_ui(s, hx + COL_NAME,  hy + 3, "NAME",    C_SUBTLE);
    draw_text_ui(s, hx + COL_STATE, hy + 3, "STATE",   C_SUBTLE);
    draw_text_ui(s, hx + COL_MEM,   hy + 3, "MEM(KB)", C_SUBTLE);

    /* ── Rows ──────────────────────────────────────────────────────── */
    int list_y = TBL_Y + HDR_H;
    int list_h = g_st.fb_h - list_y - PAD;
    g_st.rows_vis = list_h / ROW_H;
    if (g_st.rows_vis < 1) g_st.rows_vis = 1;
    clamp_scroll();

    int has_bar = (g_st.nprocs > g_st.rows_vis);
    int row_w = TBL_W - (has_bar ? SCROLLBAR_W + 2 : 0);

    for (int i = 0; i < g_st.rows_vis; i++) {
        int idx = g_st.scroll + i;
        if (idx >= g_st.nprocs) break;
        proc_row_t *pr = &g_st.procs[idx];
        int ry = list_y + i * ROW_H;

        uint32_t rowbg = (idx == g_st.sel) ? C_SEL_BG
                       : (idx & 1) ? C_INPUT_BG : BG_COLOR;
        draw_fill_rect(s, TBL_X, ry, row_w, ROW_H, rowbg);

        uint32_t fg = (idx == g_st.sel) ? C_WIN : C_TEXT;

        char cell[64];
        snprintf(cell, sizeof(cell), "%d", pr->pid);
        draw_text_ui(s, TBL_X + COL_PID, ry + 2, cell, fg);

        /* Truncate the name to its column width. */
        char nm[48];
        snprintf(nm, sizeof(nm), "%s", pr->name);
        int maxnw = COL_STATE - COL_NAME - 6;
        while (nm[0] && text_w(nm) > maxnw)
            nm[strlen(nm) - 1] = '\0';
        draw_text_ui(s, TBL_X + COL_NAME, ry + 2, nm, fg);

        snprintf(cell, sizeof(cell), "%c", pr->state);
        draw_text_ui(s, TBL_X + COL_STATE, ry + 2, cell, fg);

        snprintf(cell, sizeof(cell), "%ld", pr->vsz_kb);
        draw_text_ui(s, TBL_X + COL_MEM, ry + 2, cell, fg);
    }

    /* ── Scrollbar ─────────────────────────────────────────────────── */
    if (has_bar) {
        int track_x = TBL_X + TBL_W - SCROLLBAR_W;
        int track_y = list_y;
        int track_h = g_st.rows_vis * ROW_H;
        draw_fill_rect(s, track_x, track_y, SCROLLBAR_W, track_h, C_INPUT_BG);

        int thumb_h = (track_h * g_st.rows_vis) / g_st.nprocs;
        if (thumb_h < 16) thumb_h = 16;
        if (thumb_h > track_h) thumb_h = track_h;
        int max_scroll = g_st.nprocs - g_st.rows_vis;
        int thumb_y = track_y;
        if (max_scroll > 0)
            thumb_y += ((track_h - thumb_h) * g_st.scroll) / max_scroll;
        draw_fill_rect(s, track_x, thumb_y, SCROLLBAR_W, thumb_h, C_ACCENT);
    }

    /* ── Footer hint ───────────────────────────────────────────────── */
    char hint[96];
    snprintf(hint, sizeof(hint),
             "k: kill selected   ^/v: select   wheel: scroll   Esc: quit");
    draw_text_ui(s, PAD, g_st.fb_h - 18, hint, C_SUBTLE);

    lumen_window_present(g_st.lwin);
}

/* ── Input handling ───────────────────────────────────────────────────── */

static void select_delta(int d)
{
    if (g_st.nprocs == 0) { g_st.sel = -1; return; }
    if (g_st.sel < 0)
        g_st.sel = (d > 0) ? 0 : g_st.nprocs - 1;
    else
        g_st.sel += d;
    if (g_st.sel < 0) g_st.sel = 0;
    if (g_st.sel >= g_st.nprocs) g_st.sel = g_st.nprocs - 1;
    scroll_to_sel();
    g_st.dirty = 1;
}

static void kill_selected(void)
{
    if (g_st.sel < 0 || g_st.sel >= g_st.nprocs) return;
    int pid = g_st.procs[g_st.sel].pid;
    if (pid <= 1) {               /* never signal pid 0 or init */
        dprintf(2, "[SYSMON] refusing to kill pid %d\n", pid);
        return;
    }
    if (kill(pid, SIGTERM) == 0)
        dprintf(2, "[SYSMON] sent SIGTERM to pid %d\n", pid);
    else
        dprintf(2, "[SYSMON] kill pid %d failed\n", pid);
    /* Re-scan immediately so the row reflects the change. */
    scan_procs();
    g_st.dirty = 1;
}

static void handle_key(unsigned k)
{
    switch (k) {
    case KEY_ESC:
        g_st.done = 1;
        break;
    case KEY_UP_SYN:
    case KEY_UP_RAW:
        select_delta(-1);
        break;
    case KEY_DOWN_SYN:
    case KEY_DOWN_RAW:
        select_delta(1);
        break;
    case 'k':
    case 'K':
        kill_selected();
        break;
    default:
        break;
    }
}

/* Left click: select the row under (x,y); empty space clears selection. */
static void handle_click(int x, int y)
{
    int list_y = TBL_Y + HDR_H;
    if (x < TBL_X || x >= TBL_X + TBL_W) return;
    if (y < list_y) return;
    int row = (y - list_y) / ROW_H;
    int idx = g_st.scroll + row;
    if (row < 0 || row >= g_st.rows_vis || idx >= g_st.nprocs) return;
    g_st.sel = idx;
    g_st.dirty = 1;
}

static void handle_wheel(int delta)
{
    /* scroll up moves toward earlier rows (delta > 0 = up) */
    g_st.scroll -= delta;
    clamp_scroll();
    g_st.dirty = 1;
}

/* ── Main ─────────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    /* Window size clamped to the framebuffer (Lumen exports LUMEN_FB_*). */
    int win_w = WIN_W, win_h = WIN_H;
    const char *efw = getenv("LUMEN_FB_W");
    const char *efh = getenv("LUMEN_FB_H");
    if (efw) { int v = atoi(efw); if (v > 0 && win_w > v) win_w = v; }
    if (efh) { int v = atoi(efh); if (v > 0 && win_h > v) win_h = v; }

    g_st.lfd = lumen_connect_retry();
    if (g_st.lfd < 0) {
        dprintf(2, "[SYSMON] lumen_connect failed\n");
        return 1;
    }

    g_st.lwin = lumen_window_create(g_st.lfd, "System Monitor", win_w, win_h);
    if (!g_st.lwin) {
        dprintf(2, "[SYSMON] window_create failed\n");
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

    g_st.sel = -1;
    g_st.scroll = 0;
    scan_meminfo();
    scan_procs();

    dprintf(2, "[SYSMON] connected procs=%d\n", g_st.nprocs);

    g_st.dirty = 1;
    render();

    while (!s_term && !g_st.done) {
        lumen_event_t ev;
        int r = lumen_wait_event(g_st.lfd, &ev, 1000);
        if (r < 0) break;

        if (r == 0) {
            /* ~1 Hz refresh tick. */
            scan_meminfo();
            scan_procs();
            g_st.dirty = 1;
        } else if (r == 1) {
            if (ev.type == LUMEN_EV_CLOSE_REQUEST)
                break;
            if (ev.type == LUMEN_EV_KEY && ev.key.pressed)
                handle_key(ev.key.keycode);
            if (ev.type == LUMEN_EV_MOUSE) {
                if (ev.mouse.evtype == LUMEN_MOUSE_WHEEL)
                    handle_wheel(ev.mouse.scroll);
                else if (ev.mouse.evtype == LUMEN_MOUSE_DOWN &&
                         (ev.mouse.buttons & 1))
                    handle_click(ev.mouse.x, ev.mouse.y);
            }
        }
        render();
    }

    lumen_window_destroy(g_st.lwin);
    close(g_st.lfd);
    dprintf(2, "[SYSMON] exit\n");
    return 0;
}
