/* citadel-dock — standalone dock binary that talks to Lumen via the
 * external window protocol (LUMEN_OP_CREATE_PANEL + LUMEN_OP_INVOKE).
 *
 * Splits the dock out of libcitadel (which used to be linked into Lumen
 * itself and rendered as an overlay callback). The protocol can't read
 * underlying compositor pixels, so the frosted-glass blur is dropped:
 * we render an opaque dark background instead. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <stdint.h>

#include <glyph.h>
#include <font.h>
#include <lumen_client.h>
#include "icons.h"

/* Dock geometry — must match the old dock.h numbers so the existing
 * dock_click_test geometry remains valid. */
#define DOCK_ICON_SIZE   48
#define DOCK_ICON_GAP    12
#define DOCK_PADDING_X   24
#define DOCK_PADDING_Y   16
#define DOCK_HEIGHT      (DOCK_ICON_SIZE + DOCK_PADDING_Y * 2)
#define DOCK_BG          THEME_SURFACE_2
#define DOCK_HOVER_BG    0x00FFFFFF
#define DOCK_HOVER_ALPHA 40

#define DOCK_ITEM_APPS      0   /* Applications launcher — always leftmost */
#define DOCK_ITEM_SETTINGS  1
#define DOCK_ITEM_FILES     2
#define DOCK_ITEM_TERMINAL  3
#define DOCK_ITEM_CALC      4
#define DOCK_ITEM_EDITOR    5
#define DOCK_ITEM_INSTALLER 6   /* live-boot only — appended at runtime */
#define DOCK_MAX_ITEMS      7

static const char *s_item_keys[DOCK_MAX_ITEMS] = {
    [DOCK_ITEM_APPS]      = "applications",
    [DOCK_ITEM_SETTINGS]  = "settings",
    [DOCK_ITEM_FILES]     = "files",
    [DOCK_ITEM_TERMINAL]  = "terminal",
    [DOCK_ITEM_CALC]      = "calculator",
    [DOCK_ITEM_EDITOR]    = "editor",
    [DOCK_ITEM_INSTALLER] = "installer",
};

/* invoke name sent to Lumen per item (differs from the [DOCK] debug key).
 * These match glyph_icon_draw's known ids exactly, so they double as the
 * icon ids in render_dock. */
static const char *s_item_invoke[DOCK_MAX_ITEMS] = {
    [DOCK_ITEM_APPS]      = "applications",
    [DOCK_ITEM_SETTINGS]  = "settings",
    [DOCK_ITEM_FILES]     = "filemanager",
    [DOCK_ITEM_TERMINAL]  = "terminal",
    [DOCK_ITEM_CALC]      = "calculator",
    [DOCK_ITEM_EDITOR]    = "editor",
    [DOCK_ITEM_INSTALLER] = "gui-installer",
};

static int s_nitems = 6;        /* + installer when on live media */
static int s_dock_w;
static int s_hover = -1;

/* The installer icon only appears on live boots: gen-limine-conf.sh marks
 * every live-media cmdline with aegis_live=1, and vigil removes the
 * installer binaries from installed disks on first boot. Check both the
 * marker and the binary so a half-cleaned disk never shows a dead icon. */
static int
show_installer_item(void)
{
    char cmdline[256] = "";
    int fd = open("/proc/cmdline", O_RDONLY);
    if (fd < 0) return 0;
    ssize_t n = read(fd, cmdline, sizeof(cmdline) - 1);
    close(fd);
    if (n <= 0) return 0;
    cmdline[n] = '\0';
    if (!strstr(cmdline, "aegis_live")) return 0;
    return access("/apps/gui-installer/gui-installer", F_OK) == 0;
}

static void
log_console(const char *msg)
{
    write(2, msg, strlen(msg));
    int cfd = open("/dev/console", O_WRONLY);
    if (cfd >= 0) { write(cfd, msg, strlen(msg)); close(cfd); }
}

/* Bridge surface_t expected by glyph draw_* over the lumen window backbuf. */
static surface_t
backbuf_surface(lumen_window_t *win)
{
    surface_t s;
    s.buf   = (uint32_t *)win->backbuf;
    s.w     = win->w;
    s.h     = win->h;
    s.pitch = win->stride;
    return s;
}

/* Local-coord rect of icon i within the dock surface. */
static void
item_rect(int i, int *ix, int *iy)
{
    *ix = DOCK_PADDING_X + i * (DOCK_ICON_SIZE + DOCK_ICON_GAP);
    *iy = DOCK_PADDING_Y;
}

static void
render_dock(lumen_window_t *win)
{
    surface_t s = backbuf_surface(win);

    /* Frosted glass dock: fill with the compositor's frost key (C_TERM_BG) so
     * Lumen blurs + tints the desktop behind the bar (the panel is created
     * frosted). Icons drawn on top are opaque (non-key) and show through. */
    draw_fill_rect(&s, 0, 0, s.w, s.h, C_TERM_BG);
    /* Subtle 1px top highlight for a glassy edge. */
    draw_blend_rect(&s, 0, 0, s.w, 1, 0x00FFFFFF, 28);

    for (int i = 0; i < s_nitems; i++) {
        int ix, iy;
        item_rect(i, &ix, &iy);
        if (i == s_hover) {
            draw_blend_rounded_rect(&s, ix - 4, iy - 4,
                                    DOCK_ICON_SIZE + 8, DOCK_ICON_SIZE + 8,
                                    R_SM + 2, DOCK_HOVER_BG, DOCK_HOVER_ALPHA);
        }
        /* Shared libglyph icons; the invoke name is the icon id
         * (s_item_keys[] holds the [DOCK] debug keys, not all of which
         * glyph_icon_draw knows — e.g. "installer"). */
        glyph_icon_draw(&s, s_item_invoke[i], NULL, ix, iy, DOCK_ICON_SIZE);
    }
}

static int
hit_test(int lx, int ly)
{
    if (lx < 0 || lx >= s_dock_w || ly < 0 || ly >= DOCK_HEIGHT)
        return -1;
    for (int i = 0; i < s_nitems; i++) {
        int ix, iy;
        item_rect(i, &ix, &iy);
        if (lx >= ix && lx < ix + DOCK_ICON_SIZE &&
            ly >= iy && ly < iy + DOCK_ICON_SIZE)
            return i;
    }
    return -1;
}

/* Emit the same [DOCK] debug lines the old in-process dock did so that
 * dock_click_test (which parses serial for icon centers) keeps working. */
static void
emit_debug_lines(int dock_screen_x, int dock_screen_y)
{
    char buf[160];
    for (int i = 0; i < s_nitems; i++) {
        int ix, iy;
        item_rect(i, &ix, &iy);
        int cx = dock_screen_x + ix + DOCK_ICON_SIZE / 2;
        int cy = dock_screen_y + iy + DOCK_ICON_SIZE / 2;
        int hw = DOCK_ICON_SIZE / 2;
        int hh = DOCK_ICON_SIZE / 2;
        int n = snprintf(buf, sizeof(buf),
                         "[DOCK] item=%s idx=%d cx=%d cy=%d hw=%d hh=%d\n",
                         s_item_keys[i], i, cx, cy, hw, hh);
        if (n > 0) log_console(buf);
    }
    log_console("[DOCK] ready\n");
}

int main(void)
{
    /* Wait forever for Lumen. Vigil starts citadel-dock at boot under
     * graphical mode, but Lumen doesn't bind /run/lumen.sock until
     * Bastion authenticates the user — which can be any number of
     * minutes later. Exiting on timeout would burn vigil restart credits
     * for no reason. Loop indefinitely instead. */
    int fd = -1;
    for (;;) {
        fd = lumen_connect();
        if (fd >= 0) break;
        if (fd != -111) {  /* ECONNREFUSED is expected; anything else is fatal */
            char buf[96];
            int n = snprintf(buf, sizeof(buf),
                "[DOCK] lumen_connect=%d (giving up)\n", fd);
            if (n > 0) log_console(buf);
            return 1;
        }
        struct timespec ts = { 0, 200 * 1000 * 1000 };  /* 200ms */
        nanosleep(&ts, NULL);
    }

    /* Initialize TTF font renderer so terminal icon ">_" looks right. */
    font_init();

    if (show_installer_item())
        s_nitems = DOCK_ITEM_INSTALLER + 1;

    s_dock_w = DOCK_PADDING_X * 2 +
               s_nitems * DOCK_ICON_SIZE +
               (s_nitems - 1) * DOCK_ICON_GAP;

    lumen_window_t *win = lumen_panel_create(fd, s_dock_w, DOCK_HEIGHT);
    if (!win) {
        log_console("[DOCK] FAIL: panel_create returned NULL\n");
        close(fd);
        return 1;
    }

    /* dock_click_test parses [DOCK] lines for icon SCREEN-centers.
     * lumen_window_created_t reply now carries the panel's screen
     * position (Lumen places panels at bottom-center). */
    emit_debug_lines(win->x, win->y);

    render_dock(win);
    lumen_window_present(win);

    /* Event loop */
    for (;;) {
        lumen_event_t ev;
        int r = lumen_wait_event(fd, &ev, -1);  /* block forever */
        if (r < 0) {
            log_console("[DOCK] connection lost\n");
            break;
        }
        if (r == 0) continue;

        switch (ev.type) {
        case LUMEN_EV_MOUSE: {
            int item = hit_test(ev.mouse.x, ev.mouse.y);
            if (ev.mouse.evtype == LUMEN_MOUSE_MOVE) {
                if (item != s_hover) {
                    s_hover = item;
                    render_dock(win);
                    lumen_window_present(win);
                }
            } else if (ev.mouse.evtype == LUMEN_MOUSE_DOWN &&
                       (ev.mouse.buttons & 1)) {
                if (item >= 0 && item < s_nitems)
                    lumen_invoke(fd, s_item_invoke[item]);
            }
            break;
        }
        case LUMEN_EV_CLOSE_REQUEST:
            /* Ignore — dock is unkillable from the UI. */
            break;
        default:
            break;
        }
    }

    lumen_window_destroy(win);
    close(fd);
    return 0;
}
