/* glyph.h -- Glyph window + geometry public API.
 *
 * Immediate-mode: clients draw into their window surface via draw.h
 * primitives. (The old retained-mode widget tree was removed — unused.)
 */
#ifndef GLYPH_H
#define GLYPH_H

#include "draw.h"
#include <stdint.h>

/* ---- Geometry ---- */

typedef struct {
    int x, y, w, h;
} glyph_rect_t;

glyph_rect_t glyph_rect_union(glyph_rect_t a, glyph_rect_t b);
int glyph_rect_empty(glyph_rect_t r);
int glyph_rect_intersects(glyph_rect_t a, glyph_rect_t b);

typedef struct glyph_window glyph_window_t;

/* ---- Window ---- */

#define GLYPH_TITLEBAR_HEIGHT 30
#define GLYPH_BORDER_WIDTH    1
#define GLYPH_SHADOW_OFFSET   0  /* no drop shadow — surface == window body */

struct glyph_window {
    /* Position on screen (top-left of border) */
    int x, y;

    /* Client area dimensions */
    int client_w, client_h;

    /* Total surface including chrome */
    int surf_w, surf_h;
    surface_t surface;

    /* Compositor blit source override. When non-NULL, the compositor reads
     * pixels from here instead of surface.buf — lets a chromeless proxy
     * window composite directly from its client's shared memfd buffer
     * (no second full-size copy, no per-frame memcpy). NULL = use surface.buf. */
    uint32_t *blit_src;

    /* 0 = don't composite yet (created but client hasn't drawn its first
     * frame). Proxy windows start unpresented and flip to 1 on first DAMAGE,
     * so a freshly-opened window never flashes its uninitialized buffer.
     * In-process windows are born presented (glyph_window_create sets 1). */
    int presented;

    /* Title */
    char title[64];

    /* Dirty tracking */
    glyph_rect_t dirty_rect;
    int has_dirty;


    /* Flags */
    int visible;
    int closeable;
    int focused_window; /* 1 if this is the compositor's focused window */
    int frosted;        /* 1 = frosted glass compositing (blur+tint, keyed blit) */
    int chromeless;     /* 1 = no titlebar/border — surface IS the client area */
    int admin;          /* 1 = elevated admin session → red titlebar (danger) */

    /* Callbacks for compositor integration */
    void (*on_key)(glyph_window_t *self, char key);
    void (*on_close)(glyph_window_t *self);
    void (*on_render)(glyph_window_t *self);  /* custom client area render (after chrome) */
    void (*on_mouse_down)(glyph_window_t *self, int x, int y);
    void (*on_mouse_move)(glyph_window_t *self, int x, int y);
    void (*on_mouse_up)(glyph_window_t *self, int x, int y);
    /* Right-button press. Only windows that set this receive right
     * clicks (proxy windows forward them as LUMEN_EV_MOUSE buttons=2). */
    void (*on_mouse_rdown)(glyph_window_t *self, int x, int y);
    void (*on_scroll)(glyph_window_t *self, int direction); /* +1 up, -1 down */
    /* Wheel with cursor position + signed delta (+up/−down). Proxy windows
     * forward as LUMEN_EV_MOUSE evtype=WHEEL; lets clients zoom/scroll about
     * the pointer. Distinct from on_scroll, which has no position. */
    void (*on_mouse_wheel)(glyph_window_t *self, int x, int y, int scroll);
    void *priv;
    int tag;  /* generic integer tag — lumen uses for PTY master fd */

    /* Open animation (driven by the compositor): 0 = none, 1 = opening.
     * anim_t0_ms is the start time in CLOCK_MONOTONIC milliseconds. */
    unsigned char anim;
    unsigned long long anim_t0_ms;

    /* Frosted-glass backdrop cache (Lumen-compositor private). Caches the
     * blurred backdrop under this frosted window so a frame that changes only
     * the window's OWN content reuses the blur instead of recomputing the box
     * blur over the whole footprint. The compositor clears blur_cache_valid
     * whenever the backdrop behind the window changed (see
     * comp_update_blur_validity). Allocated lazily on first blur; freed in
     * glyph_window_destroy — co-located with window teardown so it never
     * outlives the window and there is exactly one free site. */
    uint32_t *blur_cache;
    int blur_cache_x, blur_cache_y;   /* clamped footprint origin at fill time */
    int blur_cache_w, blur_cache_h;   /* clamped footprint dims at fill time */
    int blur_cache_valid;             /* 1 = backdrop unchanged → reuse */
};

glyph_window_t *glyph_window_create(const char *title, int client_w, int client_h);
void glyph_window_destroy(glyph_window_t *win);
void glyph_window_render(glyph_window_t *win);
void glyph_window_dispatch_mouse(glyph_window_t *win, int btn, int x, int y);
int glyph_window_get_dirty_rect(glyph_window_t *win, glyph_rect_t *out);
void glyph_window_mark_dirty_rect(glyph_window_t *win, glyph_rect_t r);
void glyph_window_mark_all_dirty(glyph_window_t *win);

#endif /* GLYPH_H */
