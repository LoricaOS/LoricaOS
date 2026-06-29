/* user/bin/imageviewer/main.c — Aegis Image Viewer (external Lumen client)
 *
 * A standalone image viewer speaking the Lumen external window protocol
 * (same pattern as calculator / settings / terminal / filemanager). It
 * decodes one image via libglyph's stb_image wrapper and renders it into
 * the window with fit/zoom/pan controls.
 *
 *   argv[1] = absolute path to an image file (filemanager passes it on
 *             open / drag). If absent, a "no file" message is shown and
 *             Esc quits.
 *
 * View model
 *   The view is a zoom factor plus a pan offset expressed in *image*
 *   pixels (ox, oy = the image-space coordinate drawn at the top-left of
 *   the image's on-screen rectangle). The default view is FIT: the whole
 *   image scaled to fit the client area, centered.
 *
 * Input
 *   Wheel        zoom about the cursor (image point under cursor stays put)
 *   + / =        zoom in about center
 *   -            zoom out about center
 *   f / 0        fit
 *   1            100% (1:1)
 *   arrows       nudge the pan (raw VT or synthetic 0xF1-0xF4)
 *   Esc / close  quit
 *
 *   Mouse drag is split unambiguously by WHERE the press lands:
 *     - press+drag on the IMAGE area  → pan the image
 *     - press+drag on the STATUS chip (bottom-left filename area) →
 *       lumen_drag_start(COPY): drag the file OUT to another window.
 *   This keeps panning and DnD from fighting over the same gesture.
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
#include <image_load.h>
#include "font.h"   /* font_init() — not pulled in by glyph.h */

/* ── Window sizing ────────────────────────────────────────────────────── */

#define DEF_WIN_W 900
#define DEF_WIN_H 650
#define MIN_WIN_W 320
#define MIN_WIN_H 240

#define BG_COLOR  0x00141418   /* off-key near-black (NOT C_TERM_BG) */

#define STATUS_H  24           /* bottom status / drag-chip strip */
#define DRAG_SLOP 6            /* px before a press becomes a drag */

#define ZOOM_MIN  0.02f
#define ZOOM_MAX  64.0f
#define ZOOM_STEP 1.25f        /* per wheel notch / key press */

/* Synthetic arrow byte codes (Lumen folds CSI arrows to these for proxy
 * windows; raw VT sequences may also arrive as ESC '[' 'A'.. ). */
#define KEY_UP    ((char)0xF1)
#define KEY_DOWN  ((char)0xF2)
#define KEY_RIGHT ((char)0xF3)
#define KEY_LEFT  ((char)0xF4)
#define KEY_ESC   '\x1b'

/* ── State ────────────────────────────────────────────────────────────── */

typedef struct {
    int             lfd;
    lumen_window_t *lwin;
    surface_t       surf;
    int             win_w, win_h;   /* client-area size */
    int             dirty;
    int             done;

    glyph_pixbuf_t   img;            /* px==NULL when load failed / no file */
    int             have_img;
    char            path[256];
    const char     *base;           /* basename pointer into path */
    int             load_errno;     /* 0 = ok, else -errno from loader */

    float           zoom;           /* on-screen px per image px */
    float           ox, oy;         /* image-space coord at image rect's TL */
    int             fit;            /* 1 = currently in fit mode */

    /* Mouse gesture tracking. */
    int             panning;        /* left button held, started on image */
    int             press_x, press_y;
    float           press_ox, press_oy;
    int             chip_press;     /* press started on the status chip */
    int             chip_dragged;   /* drag already handed to Lumen */
} viewer_t;

static viewer_t g_v;
static volatile sig_atomic_t s_term;
static void sigterm_handler(int s) { (void)s; s_term = 1; }

/* ── Small helpers ────────────────────────────────────────────────────── */

static const char *path_basename(const char *p)
{
    const char *b = p;
    for (const char *q = p; *q; q++) {
        if (*q == '/' && q[1])
            b = q + 1;
    }
    return b;
}

static int img_area_h(void)
{
    int h = g_v.win_h - STATUS_H;
    return h > 0 ? h : g_v.win_h;
}

/* Scale that makes the whole image fit the image area, centered. */
static float fit_zoom(void)
{
    if (!g_v.have_img || g_v.img.w <= 0 || g_v.img.h <= 0)
        return 1.0f;
    float zw = (float)g_v.win_w / (float)g_v.img.w;
    float zh = (float)img_area_h() / (float)g_v.img.h;
    float z = zw < zh ? zw : zh;
    if (z <= 0.0f) z = ZOOM_MIN;
    return z;
}

/* Recompute ox,oy so the image is centered at the current zoom. */
static void center_view(void)
{
    if (!g_v.have_img) return;
    float disp_w = g_v.img.w * g_v.zoom;
    float disp_h = g_v.img.h * g_v.zoom;
    g_v.ox = (disp_w - g_v.win_w) / (2.0f * g_v.zoom);
    g_v.oy = (disp_h - img_area_h()) / (2.0f * g_v.zoom);
}

static void set_fit(void)
{
    g_v.zoom = fit_zoom();
    center_view();
    g_v.fit = 1;
    g_v.dirty = 1;
}

static void set_100(void)
{
    g_v.zoom = 1.0f;
    center_view();
    g_v.fit = 0;
    g_v.dirty = 1;
}

/* Clamp pan so the image can never be dragged entirely off-screen: keep
 * at least a sliver visible on every edge. When the image is smaller than
 * the area on an axis, lock it to the centered position on that axis. */
static void clamp_pan(void)
{
    if (!g_v.have_img) return;
    int areah = img_area_h();
    float disp_w = g_v.img.w * g_v.zoom;
    float disp_h = g_v.img.h * g_v.zoom;

    /* X axis */
    if (disp_w <= g_v.win_w) {
        g_v.ox = (disp_w - g_v.win_w) / (2.0f * g_v.zoom);
    } else {
        /* image rect TL on screen = -ox*zoom; keep it within bounds so the
         * rect overlaps the area by at least a margin. */
        float min_ox = 0.0f;                                  /* TL flush left */
        float max_ox = (disp_w - g_v.win_w) / g_v.zoom;       /* BR flush right */
        if (g_v.ox < min_ox) g_v.ox = min_ox;
        if (g_v.ox > max_ox) g_v.ox = max_ox;
    }

    /* Y axis */
    if (disp_h <= areah) {
        g_v.oy = (disp_h - areah) / (2.0f * g_v.zoom);
    } else {
        float min_oy = 0.0f;
        float max_oy = (disp_h - areah) / g_v.zoom;
        if (g_v.oy < min_oy) g_v.oy = min_oy;
        if (g_v.oy > max_oy) g_v.oy = max_oy;
    }
}

/* Zoom by `factor` keeping the image point under (cx,cy) fixed on screen.
 * Screen→image: img = ox + scr/zoom (for the x axis; y analogous over the
 * image area). After changing zoom we solve for the new ox,oy so the same
 * image coordinate maps back to (cx,cy). */
static void zoom_about(float factor, int cx, int cy)
{
    if (!g_v.have_img) return;
    float old = g_v.zoom;
    float nz = old * factor;
    if (nz < ZOOM_MIN) nz = ZOOM_MIN;
    if (nz > ZOOM_MAX) nz = ZOOM_MAX;
    if (nz == old) return;

    /* image coord currently under the cursor */
    float ix = g_v.ox + (float)cx / old;
    float iy = g_v.oy + (float)cy / old;

    g_v.zoom = nz;
    g_v.ox = ix - (float)cx / nz;
    g_v.oy = iy - (float)cy / nz;

    g_v.fit = 0;
    clamp_pan();
    g_v.dirty = 1;
}

static void zoom_center(float factor)
{
    zoom_about(factor, g_v.win_w / 2, img_area_h() / 2);
}

/* ── Rendering ────────────────────────────────────────────────────────── */

static void draw_centered_msg(const char *msg, uint32_t color)
{
    int tw = glyph_text_width(msg);
    int th = glyph_text_height();
    draw_text_ui(&g_v.surf, (g_v.win_w - tw) / 2,
                 (g_v.win_h - th) / 2, msg, color);
}

static void render(void)
{
    if (!g_v.dirty) return;
    g_v.dirty = 0;
    surface_t *s = &g_v.surf;

    draw_fill_rect(s, 0, 0, g_v.win_w, g_v.win_h, BG_COLOR);

    if (!g_v.have_img) {
        char msg[300];
        if (g_v.path[0])
            snprintf(msg, sizeof(msg), "Cannot open %s", g_v.base);
        else
            snprintf(msg, sizeof(msg), "No image — press Esc to quit");
        draw_centered_msg(msg, C_SUBTLE);
        lumen_window_present(g_v.lwin);
        return;
    }

    /* Image rect on screen. Top-left = -ox*zoom (may be negative when the
     * image overflows the area). draw_blit_scaled samples the source, so we
     * can hand it the full image scaled to disp_w x disp_h. */
    int disp_w = (int)(g_v.img.w * g_v.zoom + 0.5f);
    int disp_h = (int)(g_v.img.h * g_v.zoom + 0.5f);
    if (disp_w < 1) disp_w = 1;
    if (disp_h < 1) disp_h = 1;
    int dx = (int)(-g_v.ox * g_v.zoom + 0.5f);
    int dy = (int)(-g_v.oy * g_v.zoom + 0.5f);

    if (disp_w == g_v.img.w && disp_h == g_v.img.h)
        draw_blit(s, dx, dy, g_v.img.px, g_v.img.w, g_v.img.h);
    else
        draw_blit_scaled(s, dx, dy, disp_w, disp_h,
                         g_v.img.px, g_v.img.w, g_v.img.h);

    /* Status / drag chip along the bottom. */
    int sy = g_v.win_h - STATUS_H;
    draw_fill_rect(s, 0, sy, g_v.win_w, STATUS_H, C_BAR);
    char st[320];
    snprintf(st, sizeof(st), "%s   %dx%d   %d%%",
             g_v.base, g_v.img.w, g_v.img.h, (int)(g_v.zoom * 100.0f + 0.5f));
    draw_text_ui(s, 8, sy + (STATUS_H - glyph_text_height()) / 2, st, C_SUBTLE);

    lumen_window_present(g_v.lwin);
}

/* ── Geometry queries ─────────────────────────────────────────────────── */

static int in_status_chip(int x, int y)
{
    /* The whole bottom status strip is the drag affordance. */
    return y >= g_v.win_h - STATUS_H && y < g_v.win_h &&
           x >= 0 && x < g_v.win_w;
}

/* ── Input ────────────────────────────────────────────────────────────── */

static void nudge(float dx, float dy)
{
    if (!g_v.have_img) return;
    g_v.ox += dx;
    g_v.oy += dy;
    g_v.fit = 0;
    clamp_pan();
    g_v.dirty = 1;
}

static void feed_key(char k)
{
    switch (k) {
    case '+': case '=': zoom_center(ZOOM_STEP);       break;
    case '-':           zoom_center(1.0f / ZOOM_STEP); break;
    case 'f': case 'F': case '0': set_fit();           break;
    case '1':           set_100();                     break;
    case KEY_UP:        nudge(0, -40.0f / g_v.zoom);   break;
    case KEY_DOWN:      nudge(0,  40.0f / g_v.zoom);   break;
    case KEY_LEFT:      nudge(-40.0f / g_v.zoom, 0);   break;
    case KEY_RIGHT:     nudge( 40.0f / g_v.zoom, 0);   break;
    default: break;
    }
}

static void on_mouse(const lumen_event_t *ev)
{
    int x = ev->mouse.x, y = ev->mouse.y;

    if (ev->mouse.evtype == LUMEN_MOUSE_WHEEL) {
        float f = ev->mouse.scroll > 0 ? ZOOM_STEP : 1.0f / ZOOM_STEP;
        zoom_about(f, x, y);
        return;
    }

    if (ev->mouse.evtype == LUMEN_MOUSE_DOWN && (ev->mouse.buttons & 1)) {
        if (in_status_chip(x, y) && g_v.have_img) {
            /* Begin a potential drag-out from the status chip. */
            g_v.chip_press = 1;
            g_v.chip_dragged = 0;
            g_v.press_x = x; g_v.press_y = y;
        } else if (g_v.have_img) {
            /* Begin a pan over the image area. */
            g_v.panning = 1;
            g_v.press_x = x; g_v.press_y = y;
            g_v.press_ox = g_v.ox; g_v.press_oy = g_v.oy;
        }
        return;
    }

    if (ev->mouse.evtype == LUMEN_MOUSE_MOVE) {
        if (g_v.chip_press && !g_v.chip_dragged) {
            if (abs(x - g_v.press_x) + abs(y - g_v.press_y) > DRAG_SLOP) {
                /* Hand the gesture to the compositor as a file drag-out. */
                lumen_drag_start(g_v.lwin, LUMEN_DND_COPY, g_v.base, g_v.path);
                dprintf(2, "[IMG] drag_start path=%s\n", g_v.path);
                g_v.chip_dragged = 1;
            }
            return;
        }
        if (g_v.panning) {
            /* Convert the on-screen drag delta back to image-space offset.
             * Dragging right should move the image right → ox decreases. */
            g_v.ox = g_v.press_ox - (float)(x - g_v.press_x) / g_v.zoom;
            g_v.oy = g_v.press_oy - (float)(y - g_v.press_y) / g_v.zoom;
            g_v.fit = 0;
            clamp_pan();
            g_v.dirty = 1;
        }
        return;
    }

    if (ev->mouse.evtype == LUMEN_MOUSE_UP) {
        g_v.panning = 0;
        g_v.chip_press = 0;
        g_v.chip_dragged = 0;
        return;
    }
}

/* ── Main ─────────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    /* Connect (retry only on ECONNREFUSED, like calculator). */
    g_v.lfd = lumen_connect_retry();
    if (g_v.lfd < 0) {
        dprintf(2, "[IMG] connect failed (%d)\n", g_v.lfd);
        return 1;
    }

    /* Window size: default ~900x650 clamped to ~3/4 of the framebuffer. */
    g_v.win_w = DEF_WIN_W;
    g_v.win_h = DEF_WIN_H;
    {
        const char *fw = getenv("LUMEN_FB_W");
        const char *fh = getenv("LUMEN_FB_H");
        int fbw = fw ? atoi(fw) : 0;
        int fbh = fh ? atoi(fh) : 0;
        if (fbw > 0 && g_v.win_w > fbw * 3 / 4) g_v.win_w = fbw * 3 / 4;
        if (fbh > 0 && g_v.win_h > fbh * 3 / 4) g_v.win_h = fbh * 3 / 4;
        if (g_v.win_w < MIN_WIN_W) g_v.win_w = MIN_WIN_W;
        if (g_v.win_h < MIN_WIN_H) g_v.win_h = MIN_WIN_H;
    }

    g_v.lwin = lumen_window_create(g_v.lfd, "Images", g_v.win_w, g_v.win_h);
    if (!g_v.lwin) {
        dprintf(2, "[IMG] window create failed\n");
        close(g_v.lfd);
        return 1;
    }
    g_v.win_w = g_v.lwin->w;
    g_v.win_h = g_v.lwin->h;
    g_v.surf = (surface_t){
        .buf = (uint32_t *)g_v.lwin->backbuf,
        .w = g_v.win_w, .h = g_v.win_h, .pitch = g_v.lwin->stride,
    };

    font_init();

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigterm_handler;
    sigaction(SIGTERM, &sa, NULL);

    /* Load the image (if a path was given). */
    if (argc > 1 && argv[1] && argv[1][0]) {
        snprintf(g_v.path, sizeof(g_v.path), "%s", argv[1]);
        g_v.base = path_basename(g_v.path);
        int rc = glyph_pixbuf_load_file(g_v.path, &g_v.img);
        if (rc == 0 && g_v.img.px && g_v.img.w > 0 && g_v.img.h > 0) {
            g_v.have_img = 1;
        } else {
            g_v.load_errno = rc;
            dprintf(2, "[IMG] load failed: %s (%d)\n", g_v.path, rc);
        }
    } else {
        g_v.base = "";
    }

    if (g_v.have_img)
        set_fit();

    g_v.dirty = 1;
    render();

    /* EXACT prefix below is an E2E hook — do not change "[IMG] connected". */
    dprintf(2, "[IMG] connected %dx%d file=%s\n",
            g_v.win_w, g_v.win_h, g_v.path[0] ? g_v.path : "(none)");

    while (!s_term && !g_v.done) {
        lumen_event_t ev;
        int r = lumen_wait_event(g_v.lfd, &ev, 100);
        if (r < 0) break;

        if (r == 1) {
            if (ev.type == LUMEN_EV_CLOSE_REQUEST) break;
            if (ev.type == LUMEN_EV_KEY && ev.key.pressed) {
                char k = (char)ev.key.keycode;
                if (k == KEY_ESC) break;
                feed_key(k);
            }
            if (ev.type == LUMEN_EV_MOUSE)
                on_mouse(&ev);
        }
        render();
    }

    glyph_pixbuf_free(&g_v.img);
    lumen_window_destroy(g_v.lwin);
    close(g_v.lfd);
    dprintf(2, "[IMG] exit\n");
    return 0;
}
