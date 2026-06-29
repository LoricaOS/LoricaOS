/* compositor.c -- Lumen window management and dirty-rect compositing */
#include "compositor.h"
#include "cursor.h"
#include "lumen_server.h"
#include <glyph.h>
#include <font.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Defined with the rest of the DnD machinery below comp_composite. */
static void draw_dnd_ghost(compositor_t *c);

/* Window open animation duration (ms). */
#define ANIM_OPEN_MS 150ULL

/* Monotonic milliseconds for animation timing. */
static unsigned long long
now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ULL +
           (unsigned long long)ts.tv_nsec / 1000000ULL;
}


/* ---- Helpers ---- */

/* Soft drop-shadow margin around chromed windows. win_screen_rect inflates a
 * shadowed window's footprint by this so dirty-rect tracking repaints the
 * shadow on move/close (no smear). Must comfortably exceed the shadow spread
 * + downward offset used in draw_window_shadow. */
#define SHADOW_PAD 30

/* Total window bounds on screen (including chrome + soft shadow). */
static glyph_rect_t
win_screen_rect(glyph_window_t *win)
{
    glyph_rect_t r;
    r.x = win->x;
    r.y = win->y;
    r.w = win->surf_w;
    r.h = win->surf_h;
    /* Chromed (non-chromeless) windows cast a soft shadow that extends past
     * their surface; reserve that area so damage tracking covers it. */
    if (!win->chromeless) {
        r.x -= SHADOW_PAD;
        r.y -= SHADOW_PAD;
        r.w += 2 * SHADOW_PAD;
        r.h += 2 * SHADOW_PAD;
    }
    return r;
}

/* Vertical desktop gradient color at row y (used when no wallpaper is set).
 * Both the full-redraw and dirty-rect background fills use this so a repainted
 * sub-region is bit-identical to the original paint (no seam). */
/* Vertical interpolation a→b over [0,h). */
static uint32_t
lerp_col(uint32_t a, uint32_t b, int y, int h)
{
    if (h <= 1) return a;
    int ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
    int br = (b >> 16) & 0xFF, bg = (b >> 8) & 0xFF, bb = b & 0xFF;
    int rr = ar + (br - ar) * y / (h - 1);
    int rg = ag + (bg - ag) * y / (h - 1);
    int rb = ab + (bb - ab) * y / (h - 1);
    return ((uint32_t)rr << 16) | ((uint32_t)rg << 8) | (uint32_t)rb;
}

/* Blend a*wa + b*(256-wa), wa in [0,256]. */
static uint32_t
blend_col(uint32_t a, uint32_t b, int wa)
{
    int ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
    int br = (b >> 16) & 0xFF, bg = (b >> 8) & 0xFF, bb = b & 0xFF;
    int rr = (ar * wa + br * (256 - wa)) >> 8;
    int rg = (ag * wa + bg * (256 - wa)) >> 8;
    int rb = (ab * wa + bb * (256 - wa)) >> 8;
    return ((uint32_t)rr << 16) | ((uint32_t)rg << 8) | (uint32_t)rb;
}

/* Desktop background color for row y, per the selected wallpaper preset.
 * Both the full-redraw and dirty-rect fills use this so a repainted sub-region
 * is bit-identical to the original paint (no seam). */
static uint32_t
desktop_bg_at(int y, int h)
{
    switch (glyph_theme_wallpaper()) {
    case 1:  /* Midnight — flat deep chrome */
        return THEME_BG;
    case 2:  /* Slate — raised-surface to deep gradient */
        return lerp_col(THEME_SURFACE_2, THEME_BG, y, h);
    case 3:  /* Accent — accent-tinted gradient */
        return lerp_col(blend_col(THEME_ACCENT, THEME_DESKTOP_TOP, 96),
                        THEME_DESKTOP_BOT, y, h);
    default: /* Aegis — the default top→bottom gradient */
        return lerp_col(THEME_DESKTOP_TOP, THEME_DESKTOP_BOT, y, h);
    }
}

/* Check if screen point is inside a window's total area */
static int
point_in_window(glyph_window_t *win, int px, int py)
{
    return px >= win->x && px < win->x + win->surf_w &&
           py >= win->y && py < win->y + win->surf_h;
}

/* ---- Init ---- */

void
comp_init(compositor_t *c, uint32_t *fb, uint32_t *backbuf,
          int w, int h, int pitch)
{
    memset(c, 0, sizeof(*c));
    c->fb.buf = fb;
    c->fb.w = w;
    c->fb.h = h;
    c->fb.pitch = pitch;
    c->back.buf = backbuf;
    c->back.w = w;
    c->back.h = h;
    c->back.pitch = pitch;
    c->cursor_x = w / 2;
    c->cursor_y = h / 2;
    c->full_redraw = 1;
}

/* ---- Open animation ---- */

/* Begin a window's open (scale+fade) animation. Chromeless windows (dropdown,
 * launcher, dock) are not animated. */
void
comp_start_open_anim(glyph_window_t *win)
{
    if (!win || win->chromeless)
        return;
    /* Re-read prefs at trigger time (cheap, one small file) so toggling
     * animations in Settings takes effect for the running compositor without
     * a restart. No accent side effects — only the runtime prefs are reread. */
    glyph_theme_reload_prefs();
    if (!glyph_theme_animations())
        return;
    win->anim = 1;
    win->anim_t0_ms = now_ms();
}

/* True while any window is mid-open-animation; clears finished ones. */
int
comp_has_anim(compositor_t *c)
{
    unsigned long long now = now_ms();
    int any = 0;
    for (int i = 0; i < c->nwindows; i++) {
        glyph_window_t *w = c->windows[i];
        if (!w->anim)
            continue;
        if (now - w->anim_t0_ms >= ANIM_OPEN_MS)
            w->anim = 0;
        else
            any = 1;
    }
    return any;
}

/* ---- Window management ---- */

void
comp_add_window(compositor_t *c, glyph_window_t *win)
{
    if (c->nwindows >= MAX_WINDOWS)
        return;
    c->windows[c->nwindows++] = win;
    c->focused = win;
    win->focused_window = 1;
    c->full_redraw = 1;
    /* In-process windows are presented immediately → animate their open here.
     * Proxy windows present on first DAMAGE; lumen_server starts their anim. */
    if (win->presented && !win->chromeless)
        comp_start_open_anim(win);
}

void
comp_remove_window(compositor_t *c, glyph_window_t *win)
{
    int idx = -1;
    for (int i = 0; i < c->nwindows; i++) {
        if (c->windows[i] == win) {
            idx = i;
            break;
        }
    }
    if (idx < 0)
        return;

    /* Mark the window's screen area as dirty before removal */
    comp_add_dirty(c, win_screen_rect(win));

    for (int i = idx; i < c->nwindows - 1; i++)
        c->windows[i] = c->windows[i + 1];
    c->nwindows--;

    if (c->focused == win)
        c->focused = c->nwindows > 0 ? c->windows[c->nwindows - 1] : NULL;
    if (c->drag_win == win) {
        c->drag_win = NULL;
        c->dragging = 0;
    }
    if (c->content_drag_win == win)
        c->content_drag_win = NULL;
    if (c->dnd_source == win) {
        /* source died mid-drag — cancel the whole gesture */
        c->dnd_active = 0;
        c->dnd_source = NULL;
        c->dnd_over = NULL;
    }
    if (c->dnd_over == win)
        c->dnd_over = NULL;

    /* Update focused_window flags */
    for (int i = 0; i < c->nwindows; i++)
        c->windows[i]->focused_window = (c->windows[i] == c->focused) ? 1 : 0;

    glyph_window_destroy(win);
    c->full_redraw = 1;
}

void
comp_raise_window(compositor_t *c, glyph_window_t *win)
{
    int idx = -1;
    for (int i = 0; i < c->nwindows; i++) {
        if (c->windows[i] == win) {
            idx = i;
            break;
        }
    }
    if (idx < 0 || idx == c->nwindows - 1)
        return;

    for (int i = idx; i < c->nwindows - 1; i++)
        c->windows[i] = c->windows[i + 1];
    c->windows[c->nwindows - 1] = win;
    c->full_redraw = 1;
}

glyph_window_t *
comp_window_at(compositor_t *c, int x, int y)
{
    for (int i = c->nwindows - 1; i >= 0; i--) {
        glyph_window_t *win = c->windows[i];
        if (!win->visible)
            continue;
        if (point_in_window(win, x, y))
            return win;
    }
    return NULL;
}

/* ---- Dirty rect management ---- */

void
comp_add_dirty(compositor_t *c, glyph_rect_t r)
{
    if (glyph_rect_empty(r))
        return;

    /* Clamp to screen */
    if (r.x < 0) { r.w += r.x; r.x = 0; }
    if (r.y < 0) { r.h += r.y; r.y = 0; }
    if (r.x + r.w > c->fb.w) r.w = c->fb.w - r.x;
    if (r.y + r.h > c->fb.h) r.h = c->fb.h - r.y;

    if (r.w <= 0 || r.h <= 0)
        return;

    if (c->ndirty < MAX_DIRTY_RECTS) {
        c->dirty_rects[c->ndirty++] = r;
    } else {
        /* Overflow: union into last rect */
        c->dirty_rects[MAX_DIRTY_RECTS - 1] =
            glyph_rect_union(c->dirty_rects[MAX_DIRTY_RECTS - 1], r);
    }
}

/* ---- Desktop selection box ---- */

static void
draw_selection_box(surface_t *s, compositor_t *c)
{
    if (!c->selecting) return;
    int x0 = c->sel_x0 < c->sel_x1 ? c->sel_x0 : c->sel_x1;
    int y0 = c->sel_y0 < c->sel_y1 ? c->sel_y0 : c->sel_y1;
    int w = abs(c->sel_x1 - c->sel_x0);
    int h = abs(c->sel_y1 - c->sel_y0);
    if (w < 2 || h < 2) return;
    /* Translucent accent fill */
    draw_blend_rect(s, x0, y0, w, h, THEME_ACCENT, 40);
    /* Border — slightly more opaque */
    draw_blend_rect(s, x0, y0, w, 1, THEME_ACCENT, 150);
    draw_blend_rect(s, x0, y0 + h - 1, w, 1, THEME_ACCENT, 150);
    draw_blend_rect(s, x0, y0, 1, h, THEME_ACCENT, 150);
    draw_blend_rect(s, x0 + w - 1, y0, 1, h, THEME_ACCENT, 150);
}

/* ---- Composite ---- */

/* Blit modes: 0 = full frost (blur+tint+keyed), 1 = fast frost (tint+keyed,
 * no blur — used during drag for non-dragged windows), 2 = opaque (dragged window) */
#define BLIT_FROST      0
#define BLIT_FAST_FROST  1
#define BLIT_OPAQUE      2

/* Soft drop shadow behind a chromed window: a stack of expanding translucent
 * black rounded rects, offset downward, building a gradient penumbra that the
 * window body then covers (only the rim/below shows).
 *
 * The shadow geometry is INTENTIONALLY focus-independent: focus is conveyed by
 * the unfocused-window dim overlay (inside the surface rect, which repaints on
 * focus change) and the dimmed traffic lights. A focus-dependent shadow size
 * would need the out-of-surface shadow region marked dirty on every focus
 * change (which the surface-sized mark_all_dirty does not cover), leaving a
 * stale shadow ring. Keeping it fixed means the shadow only ever changes on
 * move/open/close — exactly the events that dirty the SHADOW_PAD-expanded
 * win_screen_rect. Must stay within SHADOW_PAD. */
static void
draw_window_shadow(surface_t *back, glyph_window_t *win)
{
    int bd = GLYPH_BORDER_WIDTH;
    int tb = GLYPH_TITLEBAR_HEIGHT;
    int w = win->client_w + 2 * bd;
    int h = win->client_h + tb + 2 * bd;
    int x = win->x, y = win->y;

    int off    = 5;        /* downward offset */
    int spread = 14;       /* how far it bleeds out (< SHADOW_PAD) */
    int alpha  = 8;        /* per-layer darkness */
    int layers = 5;

    for (int i = layers; i >= 1; i--) {
        int g = spread * i / layers;
        draw_blend_rounded_rect(back, x - g, y - g + off,
                                w + 2 * g, h + 2 * g,
                                R_MD + g, 0x00000000, alpha);
    }
}

/* Render one frame of a window's open animation: its opaque client content
 * scaled toward the client center + faded in (ease-out). No chrome/frost during
 * the brief animation — it snaps to the full frosted window when done. */
static void
draw_window_open_anim(surface_t *back, glyph_window_t *win, unsigned long long el)
{
    int p = (int)(el * 256ULL / ANIM_OPEN_MS);       /* 0..256 */
    if (p > 256) p = 256;
    int inv = 256 - p;
    int e = 256 - (inv * inv / 256) * inv / 256;     /* ease-out cubic, 0..256 */
    if (e < 0) e = 0;
    if (e > 256) e = 256;
    int alpha = e > 255 ? 255 : e;
    int sc = 236 + (256 - 236) * e / 256;            /* 0.92 .. 1.0 (×256) */

    uint32_t *base = win->surface.buf;
    if (!base) return;
    int bd = GLYPH_BORDER_WIDTH, tb = GLYPH_TITLEBAR_HEIGHT;
    int cw = win->client_w, ch = win->client_h;
    int cox = win->x + bd, coy = win->y + tb + bd;   /* client screen origin */
    int dw = cw * sc / 256, dh = ch * sc / 256;
    int dx = (cox + cw / 2) - dw / 2;                /* scale about client center */
    int dy = (coy + ch / 2) - dh / 2;
    /* Client content lives at (bd, tb+bd) within the chrome-padded surface. */
    const uint32_t *csrc = &base[(tb + bd) * win->surface.pitch + bd];
    draw_blit_scaled_alpha(back, dx, dy, dw, dh, csrc, cw, ch,
                           win->surface.pitch, alpha, C_SHADOW);
}

/* Window corner radius in the compositor (matches glyph window.c CORNER_R). */
#define WIN_CORNER_R   R_MD
#define WIN_CORNER_MAX 16   /* fixed save-block size; must be >= WIN_CORNER_R */

/* Save an r×r block of the backbuffer at (bx,by) into buf (out-of-bounds → 0). */
static void
save_corner_block(surface_t *back, int bx, int by, int r, uint32_t *buf)
{
    for (int j = 0; j < r; j++)
        for (int i = 0; i < r; i++) {
            int x = bx + i, y = by + j;
            buf[j * r + i] = (x >= 0 && x < back->w && y >= 0 && y < back->h)
                             ? back->buf[y * back->pitch + x] : 0;
        }
}

/* Restore the rounded-OFF pixels of one window corner from a saved block so the
 * square-composited window reads as rounded (the saved background — desktop +
 * shadow — shows through the corner). corner: 0=TL 1=TR 2=BL 3=BR. */
static void
round_window_corner(surface_t *back, int wx, int wy, int tw, int th,
                    int r, int corner, const uint32_t *saved)
{
    int bx, by, cx, cy;
    switch (corner) {
    case 0: bx = wx;          by = wy;          cx = wx + r;          cy = wy + r;          break;
    case 1: bx = wx + tw - r; by = wy;          cx = wx + tw - r - 1; cy = wy + r;          break;
    case 2: bx = wx;          by = wy + th - r; cx = wx + r;          cy = wy + th - r - 1; break;
    default:bx = wx + tw - r; by = wy + th - r; cx = wx + tw - r - 1; cy = wy + th - r - 1; break;
    }
    for (int j = 0; j < r; j++)
        for (int i = 0; i < r; i++) {
            int x = bx + i, y = by + j;
            int dx = x - cx, dy = y - cy;
            if (dx * dx + dy * dy > r * r) {       /* outside the rounded arc */
                if (x >= 0 && x < back->w && y >= 0 && y < back->h)
                    back->buf[y * back->pitch + x] = saved[j * r + i];
            }
        }
}

/* The footprint a frosted window blurs: its full surface rect. (Chromed and
 * chromeless frosted windows both blur (x,y,surf_w,surf_h) — for chromed
 * windows total_w/total_h == surf_w/surf_h because GLYPH_SHADOW_OFFSET == 0.) */
static glyph_rect_t
blur_footprint_rect(glyph_window_t *win)
{
    glyph_rect_t r = { win->x, win->y, win->surf_w, win->surf_h };
    return r;
}

/* Frosted-glass PANEL cache (per window). The "panel" is the blurred + tinted +
 * bordered glass substrate of a frosted window — i.e. everything UNDER its
 * title/traffic-lights/client content. Both the blur and the tints take only the
 * backdrop (desktop + windows below) plus constant tint colours as input, so the
 * panel is invariant on a content-only frame and changes only when the backdrop
 * does (or admin/theme, which force a full redraw). It is therefore cached and
 * stamped exactly like the blur was (this reuses the SAME per-window buffer and
 * the SAME validity flag the blur cache used — comp_update_blur_validity clears
 * win->blur_cache_valid on any backdrop change). Caching the tints too removes
 * the whole-client alpha-blend (the biggest per-frame bucket) from content-only
 * frames, on top of the blur. Per-frame chrome (title, traffic lights, client
 * keyed blit, unfocused dim, rounded corners) is still drawn fresh on top. */

/* Clamped footprint (x,y,surf_w,surf_h) ∩ backbuffer — the region the panel
 * occupies and the cache stores. */
static void
panel_footprint(glyph_window_t *win, int back_w, int back_h,
                int *ox, int *oy, int *ow, int *oh)
{
    int fx = win->x, fy = win->y;
    int x0 = fx < 0 ? 0 : fx;
    int y0 = fy < 0 ? 0 : fy;
    int x1 = fx + win->surf_w; if (x1 > back_w) x1 = back_w;
    int y1 = fy + win->surf_h; if (y1 > back_h) y1 = back_h;
    *ox = x0; *oy = y0; *ow = x1 - x0; *oh = y1 - y0;
}

/* Cache hit: stamp the cached panel into the footprint. Returns 1 on hit, 0 on
 * miss (caller must render the panel then call panel_capture). */
static int
panel_try_stamp(surface_t *back, glyph_window_t *win)
{
    int x0, y0, cw, ch;
    panel_footprint(win, back->w, back->h, &x0, &y0, &cw, &ch);
    if (cw <= 0 || ch <= 0)
        return 0;
    if (!win->blur_cache_valid || !win->blur_cache ||
        win->blur_cache_w != cw || win->blur_cache_h != ch ||
        win->blur_cache_x != x0 || win->blur_cache_y != y0)
        return 0;
    for (int y = 0; y < ch; y++)
        memcpy(&back->buf[(y0 + y) * back->pitch + x0],
               &win->blur_cache[y * cw], (size_t)cw * sizeof(uint32_t));
    return 1;
}

/* Capture the freshly-rendered panel (footprint of the backbuffer) into the
 * cache and mark it valid. */
static void
panel_capture(surface_t *back, glyph_window_t *win)
{
    int x0, y0, cw, ch;
    panel_footprint(win, back->w, back->h, &x0, &y0, &cw, &ch);
    if (cw <= 0 || ch <= 0)
        return;
    if (!win->blur_cache || win->blur_cache_w != cw || win->blur_cache_h != ch ||
        win->blur_cache_x != x0 || win->blur_cache_y != y0) {
        free(win->blur_cache);
        win->blur_cache = malloc((size_t)cw * ch * sizeof(uint32_t));
        if (!win->blur_cache) {
            win->blur_cache_w = win->blur_cache_h = 0;
            win->blur_cache_valid = 0;
            return;     /* allocation failed — degrade to per-frame recompute */
        }
        win->blur_cache_w = cw; win->blur_cache_h = ch;
        win->blur_cache_x = x0; win->blur_cache_y = y0;
    }
    for (int y = 0; y < ch; y++)
        memcpy(&win->blur_cache[y * cw],
               &back->buf[(y0 + y) * back->pitch + x0],
               (size_t)cw * sizeof(uint32_t));
    win->blur_cache_valid = 1;
}

/* Per-frame decision: is each frosted window's cached blurred backdrop still
 * valid? The backdrop changes iff (a) an EXTERNAL dirty rect overlaps the
 * footprint — i.e. one added by an event handler before per-window content
 * damage was collected: clock/volume repaint, window move/drag, a removed
 * window, selection/DnD ghost; or (b) some OTHER window whose own content
 * changed this frame (has_dirty) overlaps the footprint. A window's own content
 * damage is appended AFTER n_ext and never invalidates its own cache — that is
 * the win. Only windows BELOW W (lower z-index) are part of W's backdrop, so
 * only a dirty window below W invalidates it; a dirty window ABOVE W is painted
 * over W afterward and never changes its backdrop (without this, a terminal
 * typing on top of another window would needlessly re-blur the one beneath it
 * every frame). Move/resize is also caught structurally by the clamped-dims
 * check in panel_try_stamp/panel_capture (origin/size mismatch → miss). n_ext is
 * the dirty-rect count at composite entry; on dirty-list overflow we
 * conservatively invalidate everything. (The cache holds the blur+tint PANEL,
 * not just the blur — same buffer, same validity flag.) */
static void
comp_update_blur_validity(compositor_t *c, int n_ext)
{
    int overflow = (c->ndirty >= MAX_DIRTY_RECTS || n_ext >= MAX_DIRTY_RECTS);
    for (int i = 0; i < c->nwindows; i++) {
        glyph_window_t *W = c->windows[i];
        if (!W->frosted || !W->visible || !W->presented)
            continue;
        if (overflow) { W->blur_cache_valid = 0; continue; }
        glyph_rect_t Wf = blur_footprint_rect(W);
        int stale = 0;
        for (int ri = 0; ri < n_ext && !stale; ri++)
            if (glyph_rect_intersects(c->dirty_rects[ri], Wf))
                stale = 1;
        /* Only windows BELOW W (index < i) are in W's backdrop. */
        for (int j = 0; j < i && !stale; j++) {
            glyph_window_t *J = c->windows[j];
            if (!J->has_dirty || !J->visible || !J->presented)
                continue;
            if (glyph_rect_intersects(blur_footprint_rect(J), Wf))
                stale = 1;
        }
        if (stale)
            W->blur_cache_valid = 0;
    }
}

static void
blit_window_to_back(surface_t *back, glyph_window_t *win, int mode)
{
    /* Source pixels: a chromeless proxy window points blit_src at its
     * client's shared memfd buffer (no second copy); everyone else uses
     * surface.buf. */
    uint32_t *src = win->blit_src ? win->blit_src : win->surface.buf;

    /* Open animation: scale+fade the window in, then snap to normal. */
    if (win->anim) {
        unsigned long long el = now_ms() - win->anim_t0_ms;
        if (el >= ANIM_OPEN_MS) {
            win->anim = 0;          /* finished — fall through to normal render */
        } else if (mode == BLIT_FROST) {
            draw_window_open_anim(back, win, el);
            return;
        }
    }

    /* Soft drop shadow under chromed windows. Only on full-quality frost
     * passes (skipped mid-drag for smoothness; a full redraw restores it). */
    if (!win->chromeless && mode == BLIT_FROST)
        draw_window_shadow(back, win);

    if (win->frosted && win->chromeless && mode != BLIT_OPAQUE) {
        /* Chromeless frosted window (dropdown terminal, fullscreen launcher).
         * Panel = blur + the single dark tint; cached + stamped as one unit. */
        if (!(mode == BLIT_FROST && panel_try_stamp(back, win))) {
            if (mode == BLIT_FROST)
                draw_box_blur(back, win->x, win->y, win->surf_w, win->surf_h, 10);
            draw_blend_rect(back, win->x, win->y, win->surf_w, win->surf_h,
                            C_TERM_BG, 128);
            if (mode == BLIT_FROST)
                panel_capture(back, win);
        }
        draw_blit_keyed(back, win->x, win->y, src,
                        win->surf_w, win->surf_h, C_TERM_BG);
    } else if (win->frosted && mode != BLIT_OPAQUE) {
        int bd = GLYPH_BORDER_WIDTH;
        int tb = GLYPH_TITLEBAR_HEIGHT;
        int total_w = win->client_w + 2 * bd;
        int total_h = win->client_h + tb + 2 * bd;
        int rr = WIN_CORNER_R;

        /* Save the 4 corner blocks of the background (desktop + shadow) BEFORE
         * compositing, so we can round the window's corners back to it after. */
        uint32_t corner_tl[WIN_CORNER_MAX * WIN_CORNER_MAX];
        uint32_t corner_tr[WIN_CORNER_MAX * WIN_CORNER_MAX];
        uint32_t corner_bl[WIN_CORNER_MAX * WIN_CORNER_MAX];
        uint32_t corner_br[WIN_CORNER_MAX * WIN_CORNER_MAX];
        save_corner_block(back, win->x,                win->y,                rr, corner_tl);
        save_corner_block(back, win->x + total_w - rr, win->y,                rr, corner_tr);
        save_corner_block(back, win->x,                win->y + total_h - rr, rr, corner_bl);
        save_corner_block(back, win->x + total_w - rr, win->y + total_h - rr, rr, corner_br);

        /* Steps 1–4 form the frosted glass PANEL (blur + tints + borders),
         * which depends only on the backdrop + constant tint colours → cache it
         * as a unit and stamp on a content-only frame (full-quality frost only;
         * fast-frost during a drag recomputes without the blur, as before). */
        if (!(mode == BLIT_FROST && panel_try_stamp(back, win))) {
            /* 1. Blur the entire window footprint (skip during fast frost) */
            if (mode == BLIT_FROST)
                draw_box_blur(back, win->x, win->y, total_w, total_h, 10);

            /* 2. Titlebar tint. Normally a dark frosted glass; danger-red and
             * near-opaque when the window's shell is an elevated admin session
             * (admin flag set via LUMEN_OP_SET_ADMIN) so it's unmistakable.
             * (Frosted chromed windows draw their chrome HERE, not from the
             * client surface — so the red indicator must live in the compositor,
             * matching window.c's non-frosted C_CHROME_ADMIN.) */
            if (win->admin)
                draw_blend_rect(back, win->x, win->y, total_w, tb + bd,
                                0x00B0202A, 230);
            else
                draw_blend_rect(back, win->x, win->y, total_w, tb + bd,
                                0x00101020, 160);

            /* 3. Tint on client region — dark for terminals, light for widgets */
            if (win->priv) {
                /* Terminal: dark frosted glass matching terminal bg */
                draw_blend_rect(back, win->x + bd, win->y + tb + bd,
                                win->client_w, win->client_h,
                                0x000A0A14, 160);
            } else {
                /* Widget window: dark translucent glass (not white) */
                draw_blend_rect(back, win->x + bd, win->y + tb + bd,
                                win->client_w, win->client_h,
                                0x00181828, 150);
            }

            /* 4. Subtle border around entire window */
            draw_blend_rect(back, win->x, win->y, total_w, 1, 0x00FFFFFF, 30);
            draw_blend_rect(back, win->x, win->y + total_h - 1, total_w, 1, 0x00000000, 40);
            draw_blend_rect(back, win->x, win->y, 1, total_h, 0x00FFFFFF, 20);
            draw_blend_rect(back, win->x + total_w - 1, win->y, 1, total_h, 0x00000000, 30);

            /* Capture blur+tints+borders for reuse next content-only frame. */
            if (mode == BLIT_FROST)
                panel_capture(back, win);
        }

        /* 5. Title text — drawn directly on frosted backbuffer (no halo) */
        int title_cy = win->y + (tb + bd) / 2;
        if (g_font_ui) {
            int tw = font_text_width(g_font_ui, 13, win->title);
            int tx = win->x + (total_w - tw) / 2;
            int ty = title_cy - font_height(g_font_ui, 13) / 2;
            font_draw_text(back, g_font_ui, 13, tx, ty, win->title, 0x00FFFFFF);
        } else {
            int len = 0;
            const char *p = win->title;
            while (*p++) len++;
            int tx = win->x + (total_w - len * FONT_W) / 2;
            draw_text_t(back, tx, win->y + bd + 5, win->title, 0x00FFFFFF);
        }

        /* 6. Traffic-light circles — drawn directly on backbuffer.
         * Focused windows show full color; unfocused dim to grey (macOS). */
        int btn_cy = title_cy;
        int btn_x = win->x + bd + 8 + 7;
        int tl_focused = win->focused_window;
        uint32_t tl_r = tl_focused ? THEME_TL_RED    : 0x00555E6E;
        uint32_t tl_y = tl_focused ? THEME_TL_YELLOW : 0x00555E6E;
        uint32_t tl_g = tl_focused ? THEME_TL_GREEN  : 0x00555E6E;
        draw_circle_filled(back, btn_x, btn_cy, 7, tl_r);
        draw_circle_filled(back, btn_x + 22, btn_cy, 7, tl_y);
        draw_circle_filled(back, btn_x + 44, btn_cy, 7, tl_g);

        /* 7. Blit client area content with color keying (transparent pixels) */
        {
            int cx = bd, cy = bd + tb;
            int dx = win->x + cx, dy = win->y + cy;
            int cw = win->client_w, ch2 = win->client_h;
            /* Clamp to backbuffer bounds once */
            int sx0 = 0, sy0 = 0;
            if (dx < 0) { sx0 = -dx; cw += dx; dx = 0; }
            if (dy < 0) { sy0 = -dy; ch2 += dy; dy = 0; }
            if (dx + cw > back->w) cw = back->w - dx;
            if (dy + ch2 > back->h) ch2 = back->h - dy;
            uint32_t key = win->priv ? C_TERM_BG : C_SHADOW;
            for (int row = 0; row < ch2; row++) {
                uint32_t *src_row = &src[(cy + sy0 + row) * win->surface.pitch + cx + sx0];
                uint32_t *dst_row = &back->buf[(dy + row) * back->pitch + dx];
                for (int col = 0; col < cw; col++) {
                    if (src_row[col] != key)
                        dst_row[col] = src_row[col];
                }
            }
        }

        /* 8. Recede unfocused windows with a subtle dim (macOS-style). */
        if (!win->focused_window)
            draw_blend_rect(back, win->x, win->y, total_w, total_h,
                            0x00000000, 22);

        /* 9. Round the 4 corners by restoring the saved background (desktop +
         * shadow) outside the rounded arc — makes the square composite read as
         * a rounded-rectangle window matching the rounded shadow. */
        round_window_corner(back, win->x, win->y, total_w, total_h, rr, 0, corner_tl);
        round_window_corner(back, win->x, win->y, total_w, total_h, rr, 1, corner_tr);
        round_window_corner(back, win->x, win->y, total_w, total_h, rr, 2, corner_bl);
        round_window_corner(back, win->x, win->y, total_w, total_h, rr, 3, corner_br);
    } else if (!win->chromeless) {
        /* Opaque blit for a chromed window — used for the window being dragged
         * (frost/blur skipped for smoothness). Still round the corners so a
         * drag doesn't square them off (the bug this branch fixes). */
        int bd = GLYPH_BORDER_WIDTH;
        int tb = GLYPH_TITLEBAR_HEIGHT;
        int total_w = win->client_w + 2 * bd;
        int total_h = win->client_h + tb + 2 * bd;
        int rr = WIN_CORNER_R;

        uint32_t corner_tl[WIN_CORNER_MAX * WIN_CORNER_MAX];
        uint32_t corner_tr[WIN_CORNER_MAX * WIN_CORNER_MAX];
        uint32_t corner_bl[WIN_CORNER_MAX * WIN_CORNER_MAX];
        uint32_t corner_br[WIN_CORNER_MAX * WIN_CORNER_MAX];
        save_corner_block(back, win->x,                win->y,                rr, corner_tl);
        save_corner_block(back, win->x + total_w - rr, win->y,                rr, corner_tr);
        save_corner_block(back, win->x,                win->y + total_h - rr, rr, corner_bl);
        save_corner_block(back, win->x + total_w - rr, win->y + total_h - rr, rr, corner_br);

        draw_blit(back, win->x, win->y, src, win->surf_w, win->surf_h);

        round_window_corner(back, win->x, win->y, total_w, total_h, rr, 0, corner_tl);
        round_window_corner(back, win->x, win->y, total_w, total_h, rr, 1, corner_tr);
        round_window_corner(back, win->x, win->y, total_w, total_h, rr, 2, corner_bl);
        round_window_corner(back, win->x, win->y, total_w, total_h, rr, 3, corner_br);
    } else {
        /* Normal opaque blit (chromeless windows) */
        draw_blit(back, win->x, win->y, src,
                  win->surf_w, win->surf_h);
    }
}

/* Night Light: a warm cast applied to the final output (reduces green/blue).
 * Applied during the backbuffer→framebuffer flip so it tints everything once,
 * after compositing. */
static inline uint32_t
night_tint(uint32_t px)
{
    uint32_t r = (px >> 16) & 0xFF;
    uint32_t g = (px >> 8) & 0xFF;
    uint32_t b = px & 0xFF;
    g = (g * 236u) >> 8;   /* ~0.92 */
    b = (b * 200u) >> 8;   /* ~0.78 */
    return (px & 0xFF000000u) | (r << 16) | (g << 8) | b;
}

static void
partial_flip(surface_t *fb, surface_t *back, glyph_rect_t r)
{
    int nl = glyph_theme_night_light();
    /* Copy only the dirty rect from backbuffer to framebuffer */
    for (int y = r.y; y < r.y + r.h && y < fb->h; y++) {
        if (y < 0) continue;
        int x0 = r.x < 0 ? 0 : r.x;
        int x1 = r.x + r.w;
        if (x1 > fb->w) x1 = fb->w;
        int count = x1 - x0;
        if (count <= 0) continue;
        if (nl) {
            uint32_t *d = &fb->buf[y * fb->pitch + x0];
            uint32_t *s = &back->buf[y * back->pitch + x0];
            for (int i = 0; i < count; i++)
                d[i] = night_tint(s[i]);
        } else {
            memcpy(&fb->buf[y * fb->pitch + x0],
                   &back->buf[y * back->pitch + x0],
                   (unsigned)count * sizeof(uint32_t));
        }
    }
}

int
comp_composite(compositor_t *c)
{
    /* Dirty rects present now (added by event handlers this frame) are
     * "external" w.r.t. window content: any of them overlapping a frosted
     * window's footprint means the BACKDROP changed. Per-window content damage
     * is appended by the loop below (index >= n_ext) and must NOT invalidate a
     * window's own backdrop cache. Snapshot the boundary before collection. */
    int n_ext = c->ndirty;

    /* Collect dirty rects from windows */
    for (int i = 0; i < c->nwindows; i++) {
        glyph_window_t *win = c->windows[i];
        if (!win->visible || !win->presented)
            continue;
        glyph_rect_t wr;
        if (glyph_window_get_dirty_rect(win, &wr)) {
            /* Convert window-local dirty rect to screen coords */
            wr.x += win->x;
            wr.y += win->y;
            comp_add_dirty(c, wr);
        }
    }

    /* Full redraw path (first frame, window raise, etc.) */
    if (c->full_redraw) {
        /* Whole backdrop is being repainted → every frosted panel cache is
         * stale; panel_capture will recompute and refill on this frame. */
        for (int i = 0; i < c->nwindows; i++)
            c->windows[i]->blur_cache_valid = 0;

        /* Desktop background — wallpaper or solid fill */
        if (c->wallpaper.pixels) {
            if ((int)c->wallpaper.w == c->back.w &&
                (int)c->wallpaper.h == c->back.h) {
                /* Exact match — memcpy rows (pitch may differ from width) */
                for (int y = 0; y < c->back.h; y++)
                    memcpy(&c->back.buf[y * c->back.pitch],
                           &c->wallpaper.pixels[y * c->back.w],
                           (size_t)c->back.w * sizeof(uint32_t));
            } else {
                draw_blit_scaled(&c->back, 0, 0, c->back.w, c->back.h,
                                 c->wallpaper.pixels,
                                 (int)c->wallpaper.w, (int)c->wallpaper.h);
            }
        } else {
            /* Vertical gradient backdrop (no wallpaper). */
            for (int y = 0; y < c->back.h; y++) {
                uint32_t col = desktop_bg_at(y, c->back.h);
                uint32_t *row = &c->back.buf[y * c->back.pitch];
                for (int x = 0; x < c->back.w; x++)
                    row[x] = col;
            }
        }

        /* Desktop decorations */
        if (c->on_draw_desktop)
            c->on_draw_desktop(&c->back, c->back.w, c->back.h);

        /* Render and blit all windows */
        for (int i = 0; i < c->nwindows; i++) {
            glyph_window_t *win = c->windows[i];
            if (!win->visible || !win->presented)
                continue;
            glyph_window_mark_all_dirty(win);
            glyph_window_render(win);
            blit_window_to_back(&c->back, win, 0);
        }

        /* Desktop selection box */
        draw_selection_box(&c->back, c);

        /* Overlay (frosted glass dock etc.) -- after windows, before flip */
        if (c->on_draw_overlay)
            c->on_draw_overlay(&c->back, c->back.w, c->back.h);

        draw_dnd_ghost(c);

        /* Full flip (warm-tinted when Night Light is on). */
        if (glyph_theme_night_light()) {
            for (int y = 0; y < c->fb.h; y++) {
                uint32_t *d = &c->fb.buf[y * c->fb.pitch];
                uint32_t *s = &c->back.buf[y * c->back.pitch];
                for (int x = 0; x < c->fb.w; x++)
                    d[x] = night_tint(s[x]);
            }
        } else {
            memcpy(c->fb.buf, c->back.buf,
                   (size_t)c->fb.pitch * (size_t)c->fb.h * sizeof(uint32_t));
        }

        c->full_redraw = 0;
        c->ndirty = 0;
        c->bg_rendered = 1;
        return 1;
    }

    /* Skip if nothing dirty */
    if (c->ndirty == 0)
        return 0;

    /* Decide which frosted windows can reuse their cached blur this frame
     * (must run before glyph_window_render clears has_dirty, and while
     * dirty_rects[0..n_ext) still holds only the external/backdrop dirties). */
    comp_update_blur_validity(c, n_ext);

    /* Process each dirty rect individually instead of unioning into one
     * giant bounding box. This avoids redrawing the entire horizontal
     * span between two small dirty regions on opposite sides. */
    int saved_ndirty = c->ndirty;
    glyph_rect_t saved_rects[MAX_DIRTY_RECTS];
    for (int i = 0; i < saved_ndirty; i++)
        saved_rects[i] = c->dirty_rects[i];

    for (int ri = 0; ri < saved_ndirty; ri++) {
        glyph_rect_t dr = saved_rects[ri];

        /* Redraw background in this dirty rect */
        if (c->wallpaper.pixels) {
            if ((int)c->wallpaper.w == c->back.w &&
                (int)c->wallpaper.h == c->back.h) {
                int dy0 = dr.y < 0 ? 0 : dr.y;
                int dy1 = dr.y + dr.h;
                if (dy1 > c->back.h) dy1 = c->back.h;
                int x0 = dr.x < 0 ? 0 : dr.x;
                int x1 = dr.x + dr.w;
                if (x1 > c->back.w) x1 = c->back.w;
                int count = x1 - x0;
                if (count > 0) {
                    for (int y = dy0; y < dy1; y++)
                        memcpy(&c->back.buf[y * c->back.pitch + x0],
                               &c->wallpaper.pixels[y * c->back.w + x0],
                               (unsigned)count * sizeof(uint32_t));
                }
            } else {
                draw_blit_scaled(&c->back, 0, 0, c->back.w, c->back.h,
                                 c->wallpaper.pixels,
                                 (int)c->wallpaper.w, (int)c->wallpaper.h);
            }
        } else {
            int dy0 = dr.y < 0 ? 0 : dr.y;
            int dy1 = dr.y + dr.h;
            if (dy1 > c->back.h) dy1 = c->back.h;
            int x0 = dr.x < 0 ? 0 : dr.x;
            int x1 = dr.x + dr.w;
            if (x1 > c->back.w) x1 = c->back.w;
            for (int y = dy0; y < dy1; y++) {
                uint32_t col = desktop_bg_at(y, c->back.h);
                for (int x = x0; x < x1; x++)
                    c->back.buf[y * c->back.pitch + x] = col;
            }
        }
    }

    /* Desktop decorations (once — not per-rect) */
    if (c->on_draw_desktop)
        c->on_draw_desktop(&c->back, c->back.w, c->back.h);

    /* Render windows that overlap ANY dirty rect */
    for (int i = 0; i < c->nwindows; i++) {
        glyph_window_t *win = c->windows[i];
        if (!win->visible || !win->presented)
            continue;
        glyph_rect_t wr = win_screen_rect(win);
        int dominated = 0;
        for (int ri = 0; ri < saved_ndirty; ri++) {
            if (glyph_rect_intersects(wr, saved_rects[ri])) {
                dominated = 1;
                break;
            }
        }
        if (!dominated)
            continue;
        glyph_window_render(win);
        {
            int mode = BLIT_FROST;
            if (c->dragging)
                mode = (win == c->drag_win) ? BLIT_OPAQUE : BLIT_FAST_FROST;
            blit_window_to_back(&c->back, win, mode);
        }
    }

    /* Overlay (frosted glass dock etc.) — once, after windows */
    if (c->on_draw_overlay)
        c->on_draw_overlay(&c->back, c->back.w, c->back.h);

    draw_dnd_ghost(c);

    /* Partial flip — per dirty rect */
    for (int ri = 0; ri < saved_ndirty; ri++)
        partial_flip(&c->fb, &c->back, saved_rects[ri]);

    c->ndirty = 0;
    return 1;
}

/* ---- Drag-and-drop (compositor-brokered) ---- */

#define DND_GHOST_H   24
#define DND_GHOST_OFF 14   /* offset from cursor hotspot */

static int dnd_ghost_w(compositor_t *c)
{
    int tw;
    if (g_font_ui) tw = font_text_width(g_font_ui, 14, c->dnd_label);
    else           tw = (int)strlen(c->dnd_label) * FONT_W;
    int w = tw + 18;
    if (w > 220) w = 220;
    if (w < 40)  w = 40;
    return w;
}

static glyph_rect_t dnd_ghost_rect_at(compositor_t *c, int cx, int cy)
{
    glyph_rect_t r = { cx + DND_GHOST_OFF, cy + DND_GHOST_OFF,
                       dnd_ghost_w(c) + 2, DND_GHOST_H + 2 };
    return r;
}

/* Draw the drag ghost into the back buffer (called from comp_composite
 * after windows + overlay, before the flip). */
static void draw_dnd_ghost(compositor_t *c)
{
    if (!c->dnd_active) return;
    int gx = c->cursor_x + DND_GHOST_OFF;
    int gy = c->cursor_y + DND_GHOST_OFF;
    int gw = dnd_ghost_w(c);
    draw_blend_rounded_rect(&c->back, gx, gy, gw, DND_GHOST_H, 6,
                            THEME_ACCENT, 200);
    if (g_font_ui)
        font_draw_text(&c->back, g_font_ui, 14, gx + 9, gy + 4,
                       c->dnd_label, 0x00FFFFFF);
    else
        draw_text_t(&c->back, gx + 9, gy + 2, c->dnd_label, 0x00FFFFFF);
}

/* Resolve the DnD target under the pointer: proxy windows only (they
 * have priv set and tag < 0 — the dropdown terminal has tag >= 0). */
static glyph_window_t *dnd_target_at(compositor_t *c, int x, int y)
{
    glyph_window_t *win = comp_window_at(c, x, y);
    if (win && win->priv && win->tag < 0)
        return win;
    return NULL;
}

void
comp_dnd_start(compositor_t *c, glyph_window_t *src, uint8_t op,
               const char *label, const char *path)
{
    /* The press that began the gesture must still be held; a DRAG_START
     * arriving after release is stale — drop it. */
    if (!(c->prev_buttons & 1)) return;
    c->dnd_active = 1;
    c->dnd_op     = op;
    c->dnd_source = src;
    c->dnd_over   = NULL;
    snprintf(c->dnd_label, sizeof(c->dnd_label), "%s",
             (label && label[0]) ? label : "1 item");
    snprintf(c->dnd_path, sizeof(c->dnd_path), "%s", path ? path : "");
    /* Compositor owns the gesture now — stop routing content-drag moves
     * back to the source window. */
    c->content_drag_win = NULL;
    comp_add_dirty(c, dnd_ghost_rect_at(c, c->cursor_x, c->cursor_y));
}

/* ---- Mouse handling ---- */

/* Hit-test the close button (red circle in titlebar) */
static int
hit_close_button(glyph_window_t *win, int mx, int my)
{
    /* Circle center: same as render_chrome btn_x + BTN_RADIUS */
    int cx = win->x + GLYPH_BORDER_WIDTH + 8 + 7;
    int cy = win->y + (GLYPH_TITLEBAR_HEIGHT + GLYPH_BORDER_WIDTH) / 2;
    int dx = mx - cx, dy = my - cy;
    return dx * dx + dy * dy <= 10 * 10;
}

/* Hit-test the titlebar area of a glyph window */
static int
hit_titlebar(glyph_window_t *win, int mx, int my)
{
    int tb_x = win->x + GLYPH_BORDER_WIDTH;
    int tb_y = win->y + GLYPH_BORDER_WIDTH;
    int tb_w = win->client_w;
    int tb_h = GLYPH_TITLEBAR_HEIGHT;
    return mx >= tb_x && mx < tb_x + tb_w &&
           my >= tb_y && my < tb_y + tb_h;
}

void
comp_handle_mouse(compositor_t *c, uint8_t buttons, int16_t dx, int16_t dy,
                  int16_t scroll)
{
    int left = buttons & 1;
    int prev_left = c->prev_buttons & 1;
    int old_cx = c->cursor_x;
    int old_cy = c->cursor_y;

    /* Update cursor position with the user's pointer-speed multiplier
     * (percent; 150 = the historical 1.5x). Refreshed once/sec via prefs. */
    int spd = glyph_theme_pointer_speed();
    if (spd < 50)  spd = 50;
    if (spd > 400) spd = 400;
    c->cursor_x += (int)dx * spd / 100;
    c->cursor_y += (int)dy * spd / 100;
    if (c->cursor_x < 0) c->cursor_x = 0;
    if (c->cursor_y < 0) c->cursor_y = 0;
    if (c->cursor_x >= c->fb.w) c->cursor_x = c->fb.w - 1;
    if (c->cursor_y >= c->fb.h) c->cursor_y = c->fb.h - 1;

    /* If cursor moved, add old and new cursor rects as dirty */
    if (c->cursor_x != old_cx || c->cursor_y != old_cy) {
        glyph_rect_t old_r = { old_cx, old_cy, CURSOR_W, CURSOR_H };
        glyph_rect_t new_r = { c->cursor_x, c->cursor_y, CURSOR_W, CURSOR_H };
        comp_add_dirty(c, old_r);
        comp_add_dirty(c, new_r);
    }

    /* Wheel: deliver to the window under the cursor, independent of buttons
     * or movement. Proxy windows (tag < 0) get on_mouse_wheel with the
     * cursor position; the dropdown terminal (tag >= 0) uses its position-
     * less on_scroll for scrollback. Not blocked by DnD (you can't scroll
     * mid-drag anyway — no wheel arrives then). */
    if (scroll != 0 && !c->dnd_active) {
        /* Natural scrolling inverts wheel direction (content follows fingers).
         * Setting is refreshed once/second from the clock tick. */
        if (glyph_theme_natural_scroll())
            scroll = -scroll;
        glyph_window_t *ww = comp_window_at(c, c->cursor_x, c->cursor_y);
        if (ww && ww->tag < 0 && ww->on_mouse_wheel)
            ww->on_mouse_wheel(ww, c->cursor_x - ww->x, c->cursor_y - ww->y,
                               scroll);
        else if (ww && ww->tag >= 0 && ww->on_scroll)
            ww->on_scroll(ww, scroll > 0 ? 1 : -1);
    }

    /* Drag-and-drop in progress: ghost follows the cursor; the proxy
     * window under the pointer gets DRAG_OVER/DRAG_LEAVE, and the one
     * under it at release gets the DROP with the path payload. */
    if (c->dnd_active && left) {
        if (c->cursor_x != old_cx || c->cursor_y != old_cy) {
            comp_add_dirty(c, dnd_ghost_rect_at(c, old_cx, old_cy));
            comp_add_dirty(c, dnd_ghost_rect_at(c, c->cursor_x, c->cursor_y));
        }
        glyph_window_t *over = dnd_target_at(c, c->cursor_x, c->cursor_y);
        if (over != c->dnd_over) {
            if (c->dnd_over)
                lumen_proxy_send_drag_leave(c->dnd_over);
            c->dnd_over = over;
        }
        if (over)
            lumen_proxy_send_drag_over(over, c->cursor_x - over->x,
                                       c->cursor_y - over->y, c->dnd_op);
        c->prev_buttons = buttons;
        return;
    }

    /* Drag-and-drop released */
    if (c->dnd_active && !left) {
        glyph_window_t *over = dnd_target_at(c, c->cursor_x, c->cursor_y);
        if (over)
            lumen_proxy_send_drop(over, c->cursor_x - over->x,
                                  c->cursor_y - over->y,
                                  c->dnd_op, c->dnd_path);
        c->dnd_active = 0;
        c->dnd_source = NULL;
        c->dnd_over   = NULL;
        c->full_redraw = 1;   /* erase the ghost wherever it was */
        c->prev_buttons = buttons;
        return;
    }

    /* Hover MOVE for proxy windows: external clients (dock highlight,
     * file manager menu hover) need cursor motion even with no button
     * held. Forward no-button moves to the proxy under the cursor. */
    if (!left && !c->dragging && !c->selecting && !c->content_drag_win &&
        (c->cursor_x != old_cx || c->cursor_y != old_cy)) {
        glyph_window_t *hw = comp_window_at(c, c->cursor_x, c->cursor_y);
        /* tag < 0 distinguishes proxy windows from the dropdown terminal
         * (tag = PTY fd >= 0), whose on_mouse_move extends the text
         * selection and must only fire while the button is held. */
        if (hw && hw->on_mouse_move && hw->priv && hw->tag < 0)
            hw->on_mouse_move(hw, c->cursor_x - hw->x, c->cursor_y - hw->y);
    }

    /* Desktop selection in progress */
    if (c->selecting && left) {
        glyph_rect_t old_sel = {
            c->sel_x0 < c->sel_x1 ? c->sel_x0 : c->sel_x1,
            c->sel_y0 < c->sel_y1 ? c->sel_y0 : c->sel_y1,
            abs(c->sel_x1 - c->sel_x0) + 1,
            abs(c->sel_y1 - c->sel_y0) + 1
        };
        c->sel_x1 = c->cursor_x;
        c->sel_y1 = c->cursor_y;
        glyph_rect_t new_sel = {
            c->sel_x0 < c->sel_x1 ? c->sel_x0 : c->sel_x1,
            c->sel_y0 < c->sel_y1 ? c->sel_y0 : c->sel_y1,
            abs(c->sel_x1 - c->sel_x0) + 1,
            abs(c->sel_y1 - c->sel_y0) + 1
        };
        comp_add_dirty(c, old_sel);
        comp_add_dirty(c, new_sel);
        c->prev_buttons = buttons;
        return;
    }

    /* Desktop selection released */
    if (c->selecting && !left) {
        glyph_rect_t sel_r = {
            c->sel_x0 < c->sel_x1 ? c->sel_x0 : c->sel_x1,
            c->sel_y0 < c->sel_y1 ? c->sel_y0 : c->sel_y1,
            abs(c->sel_x1 - c->sel_x0) + 1,
            abs(c->sel_y1 - c->sel_y0) + 1
        };
        comp_add_dirty(c, sel_r);
        c->selecting = 0;
        c->prev_buttons = buttons;
        return;
    }

    /* Content drag in progress (mouse selection in client area) */
    if (c->content_drag_win && left) {
        int local_x = c->cursor_x - c->content_drag_win->x;
        int local_y = c->cursor_y - c->content_drag_win->y;
        if (c->content_drag_win->on_mouse_move)
            c->content_drag_win->on_mouse_move(c->content_drag_win, local_x, local_y);
        c->prev_buttons = buttons;
        return;
    }

    /* Content drag released */
    if (c->content_drag_win && !left) {
        int local_x = c->cursor_x - c->content_drag_win->x;
        int local_y = c->cursor_y - c->content_drag_win->y;
        if (c->content_drag_win->on_mouse_up)
            c->content_drag_win->on_mouse_up(c->content_drag_win, local_x, local_y);
        c->content_drag_win = NULL;
        c->prev_buttons = buttons;
        return;
    }

    /* Titlebar drag in progress */
    if (c->dragging && left) {
        if (c->drag_win) {
            glyph_rect_t old_r = win_screen_rect(c->drag_win);
            c->drag_win->x = c->cursor_x - c->drag_dx;
            c->drag_win->y = c->cursor_y - c->drag_dy;
            glyph_rect_t new_r = win_screen_rect(c->drag_win);
            comp_add_dirty(c, old_r);
            comp_add_dirty(c, new_r);
            glyph_window_mark_all_dirty(c->drag_win);
        }
        c->prev_buttons = buttons;
        return;
    }

    /* Titlebar drag released — restore full frost */
    if (c->dragging && !left) {
        glyph_window_t *dw = c->drag_win;
        c->dragging = 0;
        c->drag_win = NULL;
        if (dw) {
            glyph_window_mark_all_dirty(dw);
            c->full_redraw = 1;
        }
        c->prev_buttons = buttons;
        return;
    }

    /* Right-button press edge — only windows that opt in via
     * on_mouse_rdown receive it (lumen_server registers it on proxy
     * windows; in-process windows ignore right clicks as before).
     * Focus/raise semantics mirror a left click. */
    {
        int right = buttons & 2;
        int prev_right = c->prev_buttons & 2;
        if (right && !prev_right && !left) {
            glyph_window_t *win = comp_window_at(c, c->cursor_x, c->cursor_y);
            if (win && win->on_mouse_rdown) {
                if (!win->chromeless && c->focused != win) {
                    if (c->focused) {
                        c->focused->focused_window = 0;
                        glyph_window_mark_all_dirty(c->focused);
                    }
                    c->focused = win;
                    win->focused_window = 1;
                    comp_raise_window(c, win);
                    glyph_window_mark_all_dirty(win);
                }
                win->on_mouse_rdown(win, c->cursor_x - win->x,
                                    c->cursor_y - win->y);
            }
        }
    }

    /* Button press edge (0 -> 1) */
    if (left && !prev_left) {
        glyph_window_t *win = comp_window_at(c, c->cursor_x, c->cursor_y);
        if (win) {
            /* Close button.  If the window has an on_close callback (e.g.
             * a proxy window owned by an external client), invoke it so the
             * owner can run its own teardown; otherwise destroy directly. */
            if (win->closeable && hit_close_button(win, c->cursor_x, c->cursor_y)) {
                if (win->on_close)
                    win->on_close(win);
                else
                    comp_remove_window(c, win);
                c->prev_buttons = buttons;
                return;
            }

            /* Titlebar drag — skip for chromeless windows (panels) */
            if (!win->chromeless && hit_titlebar(win, c->cursor_x, c->cursor_y)) {
                c->dragging = 1;
                c->drag_win = win;
                c->drag_dx = c->cursor_x - win->x;
                c->drag_dy = c->cursor_y - win->y;
                if (c->focused != win) {
                    if (c->focused)
                        c->focused->focused_window = 0;
                    c->focused = win;
                    win->focused_window = 1;
                    comp_raise_window(c, win);
                    for (int i = 0; i < c->nwindows; i++)
                        glyph_window_mark_all_dirty(c->windows[i]);
                }


                c->prev_buttons = buttons;
                return;
            }

            /* Click on window — chromeless panels don't steal focus or raise */
            if (!win->chromeless && c->focused != win) {
                if (c->focused) {
                    c->focused->focused_window = 0;
                    glyph_window_mark_all_dirty(c->focused);
                }
                c->focused = win;
                win->focused_window = 1;
                comp_raise_window(c, win);
                glyph_window_mark_all_dirty(win);
            }

            /* Dispatch to glyph window (converts to window-local coords) */
            int local_x = c->cursor_x - win->x;
            int local_y = c->cursor_y - win->y;

            /* Content area mouse-down: start content drag for text selection.
             * Only for terminal windows (have on_mouse_down), not widget windows. */
            if (win->on_mouse_down && win->priv) {
                win->on_mouse_down(win, local_x, local_y);
                c->content_drag_win = win;
            }

            glyph_window_dispatch_mouse(win, 1, local_x, local_y);
        } else {
            /* Click on empty desktop — start selection box */
            c->selecting = 1;
            c->sel_x0 = c->cursor_x;
            c->sel_y0 = c->cursor_y;
            c->sel_x1 = c->cursor_x;
            c->sel_y1 = c->cursor_y;
        }
    }

    c->prev_buttons = buttons;
}
