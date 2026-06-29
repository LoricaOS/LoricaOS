/* lumen_server.c — Lumen external window protocol server */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <glyph.h>
#include "compositor.h"
#include "lumen_server.h"
#include "lumen_proto.h"

#define LUMEN_MAX_CLIENTS            8
#define LUMEN_MAX_WINDOWS_PER_CLIENT 8

typedef struct proxy_window proxy_window_t;

typedef struct {
    int             fd;
    proxy_window_t *windows[LUMEN_MAX_WINDOWS_PER_CLIENT];
    int             nwindows;
    uint32_t        next_id;
} lumen_client_t;

struct proxy_window {
    glyph_window_t *win;
    lumen_client_t *client;
    uint32_t        id;
    int             memfd;
    void           *shared;
};

static lumen_client_t *s_clients[LUMEN_MAX_CLIENTS];
static int              s_ncli;
static lumen_invoke_fn  s_invoke_fn;

void lumen_server_set_invoke_handler(lumen_invoke_fn fn) { s_invoke_fn = fn; }

/* ── Proxy window callbacks ─────────────────────────────────────────── */

static void proxy_on_render(glyph_window_t *win)
{
    /* Chromeless proxy windows composite directly from the shared buffer
     * (win->blit_src aliases pw->shared) — there is no surface.buf to copy
     * into, so rendering is a no-op. */
    if (win->blit_src)
        return;

    proxy_window_t *pw = win->priv;
    int client_w  = win->client_w;
    int client_h  = win->client_h;
    int surf_pitch = win->surface.pitch;

    /* Chromeless panels: surface IS the client area (no titlebar/border).
     * Regular windows: blit into the inset client region. */
    int oy = win->chromeless ? 0 : (GLYPH_TITLEBAR_HEIGHT + GLYPH_BORDER_WIDTH);
    int ox = win->chromeless ? 0 : GLYPH_BORDER_WIDTH;

    uint32_t *dst = win->surface.buf + oy * surf_pitch + ox;
    const uint32_t *src = pw->shared;

    for (int row = 0; row < client_h; row++)
        memcpy(dst + row * surf_pitch,
               src + row * client_w,
               (size_t)client_w * sizeof(uint32_t));
}

static void proxy_on_close(glyph_window_t *win)
{
    proxy_window_t *pw = win->priv;
    lumen_msg_hdr_t hdr = { LUMEN_EV_CLOSE_REQUEST,
                             sizeof(lumen_close_request_t) };
    lumen_close_request_t ev = { pw->id };
    write(pw->client->fd, &hdr, sizeof(hdr));
    write(pw->client->fd, &ev,  sizeof(ev));
}

static void proxy_on_key(glyph_window_t *win, char key)
{
    proxy_window_t *pw = win->priv;
    lumen_msg_hdr_t hdr = { LUMEN_EV_KEY, sizeof(lumen_key_event_t) };
    lumen_key_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.window_id = pw->id;
    ev.keycode   = (uint32_t)(uint8_t)key;
    ev.pressed   = 1;
    write(pw->client->fd, &hdr, sizeof(hdr));
    write(pw->client->fd, &ev,  sizeof(ev));
}

static void send_mouse_event_scroll(proxy_window_t *pw, int win_x, int win_y,
                                    uint8_t buttons, uint8_t evtype,
                                    int8_t scroll)
{
    /* Chromeless panels: window-local coords ARE client-area coords. */
    int border = pw->win->chromeless ? 0 : GLYPH_BORDER_WIDTH;
    int titleh = pw->win->chromeless ? 0 : GLYPH_TITLEBAR_HEIGHT;
    int cx = win_x - border;
    int cy = win_y - titleh - border;
    lumen_msg_hdr_t hdr = { LUMEN_EV_MOUSE, sizeof(lumen_mouse_event_t) };
    lumen_mouse_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.window_id = pw->id;
    ev.x         = cx;
    ev.y         = cy;
    ev.buttons   = buttons;
    ev.evtype    = evtype;
    ev.scroll    = scroll;
    write(pw->client->fd, &hdr, sizeof(hdr));
    write(pw->client->fd, &ev,  sizeof(ev));
}

static void send_mouse_event(proxy_window_t *pw, int win_x, int win_y,
                              uint8_t buttons, uint8_t evtype)
{
    send_mouse_event_scroll(pw, win_x, win_y, buttons, evtype, 0);
}

static void proxy_on_mouse_down(glyph_window_t *win, int x, int y)
{
    send_mouse_event(win->priv, x, y, 1, LUMEN_MOUSE_DOWN);
}

static void proxy_on_mouse_move(glyph_window_t *win, int x, int y)
{
    send_mouse_event(win->priv, x, y, 0, LUMEN_MOUSE_MOVE);
}

static void proxy_on_mouse_up(glyph_window_t *win, int x, int y)
{
    send_mouse_event(win->priv, x, y, 0, LUMEN_MOUSE_UP);
}

/* Right-button press: same DOWN event, buttons=2 (clients that only
 * test `buttons & 1` keep ignoring it). */
static void proxy_on_mouse_rdown(glyph_window_t *win, int x, int y)
{
    send_mouse_event(win->priv, x, y, 2, LUMEN_MOUSE_DOWN);
}

/* Wheel: LUMEN_MOUSE_WHEEL with the signed delta. Clients that don't
 * handle wheel just ignore the evtype. */
static void proxy_on_mouse_wheel(glyph_window_t *win, int x, int y, int scroll)
{
    int8_t s = scroll > 127 ? 127 : (scroll < -128 ? -128 : (int8_t)scroll);
    send_mouse_event_scroll(win->priv, x, y, 0, LUMEN_MOUSE_WHEEL, s);
}

/* ── Drag-and-drop delivery (called from the compositor) ─────────────── */

/* Convert window-local coords to client-area coords (same math as
 * send_mouse_event) and emit a DnD event. */
static void dnd_client_xy(proxy_window_t *pw, int win_x, int win_y,
                          int32_t *cx, int32_t *cy)
{
    int border = pw->win->chromeless ? 0 : GLYPH_BORDER_WIDTH;
    int titleh = pw->win->chromeless ? 0 : GLYPH_TITLEBAR_HEIGHT;
    *cx = win_x - border;
    *cy = win_y - titleh - border;
}

void lumen_proxy_send_drag_over(glyph_window_t *win, int x, int y, uint8_t op)
{
    if (!win || !win->priv) return;
    proxy_window_t *pw = win->priv;
    if (win->on_render != proxy_on_render) return;
    lumen_msg_hdr_t hdr = { LUMEN_EV_DRAG_OVER, sizeof(lumen_drag_over_t) };
    lumen_drag_over_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.window_id = pw->id;
    dnd_client_xy(pw, x, y, &ev.x, &ev.y);
    ev.op = op;
    write(pw->client->fd, &hdr, sizeof(hdr));
    write(pw->client->fd, &ev,  sizeof(ev));
}

void lumen_proxy_send_drag_leave(glyph_window_t *win)
{
    if (!win || !win->priv) return;
    proxy_window_t *pw = win->priv;
    if (win->on_render != proxy_on_render) return;
    lumen_msg_hdr_t hdr = { LUMEN_EV_DRAG_LEAVE, sizeof(lumen_drag_leave_t) };
    lumen_drag_leave_t ev = { pw->id };
    write(pw->client->fd, &hdr, sizeof(hdr));
    write(pw->client->fd, &ev,  sizeof(ev));
}

void lumen_proxy_send_drop(glyph_window_t *win, int x, int y, uint8_t op,
                           const char *path)
{
    if (!win || !win->priv) return;
    proxy_window_t *pw = win->priv;
    if (win->on_render != proxy_on_render) return;
    lumen_msg_hdr_t hdr = { LUMEN_EV_DROP, sizeof(lumen_drop_event_t) };
    lumen_drop_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.window_id = pw->id;
    dnd_client_xy(pw, x, y, &ev.x, &ev.y);
    ev.op = op;
    snprintf(ev.path, sizeof(ev.path), "%s", path ? path : "");
    write(pw->client->fd, &hdr, sizeof(hdr));
    write(pw->client->fd, &ev,  sizeof(ev));
}

/* ── Focus notification (public — called from compositor) ────────────── */

void lumen_proxy_notify_focus(glyph_window_t *win, int focused)
{
    if (!win || !win->priv) return;
    proxy_window_t *pw = win->priv;
    if (win->on_render != proxy_on_render) return;
    lumen_msg_hdr_t hdr = { LUMEN_EV_FOCUS, sizeof(lumen_focus_event_t) };
    lumen_focus_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.window_id = pw->id;
    ev.focused   = (uint8_t)focused;
    write(pw->client->fd, &hdr, sizeof(hdr));
    write(pw->client->fd, &ev,  sizeof(ev));
}

/* ── CREATE_WINDOW / CREATE_PANEL shared core ─────────────────────────── */

/* Allocate a chromeless glyph_window whose surface IS the client area.
 *
 * want_surface=0 leaves surface.buf NULL — for proxy windows that composite
 * directly from the client's shared buffer (caller sets win->blit_src).
 * This avoids glyph_window_create's chrome-padded surface alloc entirely:
 * the old path allocated a chrome-padded buffer, freed it, allocated a
 * tight one, and the proxy path then freed THAT too — ~2 full-window
 * malloc/free churns per window open, which musl couldn't fully reuse
 * (size-class mismatch) → a steady per-open VmSize leak. want_surface=1
 * (dropdown terminal) still gets a tight client-sized buffer.
 */
static glyph_window_t *make_chromeless_window(int w, int h, int want_surface)
{
    glyph_window_t *win = calloc(1, sizeof(*win));
    if (!win) return NULL;

    /* Initialize the fields glyph_window_create would have set, minus the
     * chrome-padded surface allocation. */
    win->client_w = w;
    win->client_h = h;
    win->surf_w   = w;
    win->surf_h   = h;
    win->surface.w     = w;
    win->surface.h     = h;
    win->surface.pitch = w;
    win->surface.buf   = NULL;
    if (want_surface) {
        win->surface.buf = calloc((unsigned)(w * h), sizeof(uint32_t));
        if (!win->surface.buf) { free(win); return NULL; }
    }

    win->visible    = 1;
    win->presented  = 1;   /* caller (proxy) overrides to 0 for defer-present */
    win->chromeless = 1;
    win->frosted    = 0;   /* no blur/tint — opaque blit so own pixels show */
    win->closeable  = 0;
    win->tag        = -1;  /* sentinel (matches glyph_window_create) */
    win->has_dirty  = 1;
    win->dirty_rect.x = 0;
    win->dirty_rect.y = 0;
    win->dirty_rect.w = w;
    win->dirty_rect.h = h;
    return win;
}

/* Window style for handle_create_common */
#define WSTYLE_NORMAL     0  /* chrome, centered, focused */
#define WSTYLE_PANEL      1  /* chromeless, bottom-anchored, never focused */
#define WSTYLE_FULLSCREEN 2  /* chromeless, fb-sized at 0,0, focused;
                              * frosted=1 so key-color pixels show the
                              * desktop through frosted glass (launcher) */

/* Build a proxy_window + glyph_window, register with compositor, send the
 * lumen_window_created_t reply with SCM_RIGHTS memfd. style selects
 * geometry, chrome, and focus behavior (WSTYLE_*). */
static int handle_create_common(compositor_t *comp, lumen_client_t *cli,
                                int w, int h, const char *title, int style)
{
    if (cli->nwindows >= LUMEN_MAX_WINDOWS_PER_CLIENT)
        goto err_reply;

    if (style == WSTYLE_FULLSCREEN) {
        /* Request dims are ignored; the reply tells the client the real
         * size and recv_created_reply sizes its buffers from the reply. */
        w = comp->fb.w;
        h = comp->fb.h;
    }

    size_t bufsz = (size_t)w * h * sizeof(uint32_t);

    int memfd = memfd_create("lumen_win", 0);
    if (memfd < 0) {
        dprintf(2, "[LUMEN-SRV] memfd_create failed errno=%d\n", errno);
        goto err_reply;
    }
    if (ftruncate(memfd, (off_t)bufsz) < 0) {
        dprintf(2, "[LUMEN-SRV] ftruncate(%lu) failed errno=%d\n",
            (unsigned long)bufsz, errno);
        close(memfd); goto err_reply;
    }

    void *shared = mmap(NULL, bufsz, PROT_READ, MAP_SHARED, memfd, 0);
    if (shared == MAP_FAILED) {
        dprintf(2, "[LUMEN-SRV] mmap failed errno=%d bufsz=%lu\n",
            errno, (unsigned long)bufsz);
        close(memfd); goto err_reply;
    }

    proxy_window_t *pw = calloc(1, sizeof(*pw));
    if (!pw) { munmap(shared, bufsz); close(memfd); goto err_reply; }

    pw->client = cli;
    pw->id     = cli->next_id++;
    pw->memfd  = memfd;
    pw->shared = shared;

    if (style == WSTYLE_PANEL || style == WSTYLE_FULLSCREEN) {
        /* No surface.buf: the compositor reads the client's shared buffer
         * directly (blit_src). Chromeless windows have no chrome to draw,
         * so this needs no second copy and no per-frame memcpy — a
         * fullscreen launcher avoids a ~4 MB buffer plus the alloc churn. */
        pw->win = make_chromeless_window(w, h, 0 /* want_surface */);
        if (pw->win) {
            /* Both the dock panel and the fullscreen launcher render as
             * frosted glass: key-color (C_TERM_BG) pixels show the blurred,
             * tinted desktop through the surface. */
            pw->win->frosted = 1;
            pw->win->blit_src = pw->shared;
        }
    } else {
        char tbuf[64];
        memset(tbuf, 0, sizeof(tbuf));
        if (title) strncpy(tbuf, title, sizeof(tbuf) - 1);
        pw->win = glyph_window_create(tbuf, w, h);
    }
    if (!pw->win) {
        dprintf(2, "[LUMEN-SRV] window alloc failed (style=%d)\n", style);
        free(pw); munmap(shared, bufsz); close(memfd); goto err_reply;
    }

    /* Created but not yet drawn: stay unpresented until the first DAMAGE so
     * the uninitialized (zero) buffer never flashes — fixes the launcher's
     * black-flash-on-open. */
    pw->win->presented     = 0;

    pw->win->priv          = pw;
    pw->win->on_render     = proxy_on_render;
    pw->win->on_close      = proxy_on_close;
    pw->win->on_mouse_down  = proxy_on_mouse_down;
    pw->win->on_mouse_move  = proxy_on_mouse_move;
    pw->win->on_mouse_up    = proxy_on_mouse_up;
    pw->win->on_mouse_rdown = proxy_on_mouse_rdown;
    pw->win->on_mouse_wheel = proxy_on_mouse_wheel;
    /* Panels never receive keyboard input — leave on_key NULL so the
     * compositor's key dispatch falls through. */
    if (style != WSTYLE_PANEL)
        pw->win->on_key = proxy_on_key;

    if (style == WSTYLE_PANEL) {
        /* Bottom-anchored, horizontally centered. Margin matches old dock. */
        pw->win->x = (comp->fb.w - pw->win->surf_w) / 2;
        pw->win->y = comp->fb.h - pw->win->surf_h - 10;
    } else if (style == WSTYLE_FULLSCREEN) {
        pw->win->x = 0;
        pw->win->y = 0;
    } else {
        pw->win->x = (comp->fb.w - pw->win->surf_w) / 2;
        pw->win->y = (comp->fb.h - pw->win->surf_h) / 2;
    }

    comp_add_window(comp, pw->win);
    /* Panels must not steal focus (no keyboard, no chrome). */
    if (style == WSTYLE_PANEL) {
        pw->win->focused_window = 0;
        /* comp_add_window set focused = pw->win; restore prior focus. */
        glyph_window_t *new_focus = NULL;
        for (int i = comp->nwindows - 2; i >= 0; i--) {
            if (comp->windows[i]->visible && !comp->windows[i]->chromeless) {
                new_focus = comp->windows[i];
                break;
            }
        }
        comp->focused = new_focus;
        if (new_focus) new_focus->focused_window = 1;
    }
    glyph_window_mark_all_dirty(pw->win);
    comp->full_redraw = 1;

    cli->windows[cli->nwindows++] = pw;

    /* Reply: lumen_window_created_t + memfd via SCM_RIGHTS */
    lumen_window_created_t reply_data = {
        .status    = 0,
        .window_id = pw->id,
        .width     = (uint32_t)w,
        .height    = (uint32_t)h,
        .x         = pw->win->x,
        .y         = pw->win->y,
    };
    lumen_msg_hdr_t rhdr = { 0, sizeof(reply_data) };

    char cmsgbuf[CMSG_SPACE(sizeof(int))];
    struct iovec iov[2] = {
        { .iov_base = &rhdr,       .iov_len = sizeof(rhdr)       },
        { .iov_base = &reply_data, .iov_len = sizeof(reply_data) },
    };
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov        = iov;
    msg.msg_iovlen     = 2;
    msg.msg_control    = cmsgbuf;
    msg.msg_controllen = sizeof(cmsgbuf);

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type  = SCM_RIGHTS;
    cmsg->cmsg_len   = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &memfd, sizeof(int));
    msg.msg_controllen = cmsg->cmsg_len;

    if (sendmsg(cli->fd, &msg, 0) < 0)
        dprintf(2, "[LUMEN-SRV] sendmsg reply failed errno=%d\n", errno);
    return 1;

err_reply: {
        lumen_window_created_t err_data = { (uint32_t)EIO, 0, 0, 0 };
        lumen_msg_hdr_t ehdr = { 0, sizeof(err_data) };
        write(cli->fd, &ehdr,     sizeof(ehdr));
        write(cli->fd, &err_data, sizeof(err_data));
        return 0;
    }
}

static int handle_create_window(compositor_t *comp, lumen_client_t *cli,
                                  const lumen_create_window_t *req)
{
    int style = (req->flags & LUMEN_WIN_FLAG_FULLSCREEN)
                    ? WSTYLE_FULLSCREEN : WSTYLE_NORMAL;
    return handle_create_common(comp, cli, req->width, req->height,
                                req->title, style);
}

static int handle_create_panel(compositor_t *comp, lumen_client_t *cli,
                                 const lumen_create_panel_t *req)
{
    return handle_create_common(comp, cli, req->width, req->height, NULL,
                                WSTYLE_PANEL);
}

static int handle_invoke(compositor_t *comp, const lumen_invoke_t *req)
{
    if (!s_invoke_fn) return 0;
    char name[33];
    memcpy(name, req->name, 32);
    name[32] = '\0';
    s_invoke_fn(comp, name);
    return 1;
}

/* ── Helpers ─────────────────────────────────────────────────────────── */

static proxy_window_t *find_proxy(lumen_client_t *cli, uint32_t id)
{
    for (int i = 0; i < cli->nwindows; i++)
        if (cli->windows[i]->id == id)
            return cli->windows[i];
    return NULL;
}

static int handle_damage(compositor_t *comp, lumen_client_t *cli,
                           uint32_t window_id)
{
    proxy_window_t *pw = find_proxy(cli, window_id);
    if (!pw) return 0;
    /* First DAMAGE = first frame is ready → reveal the window (it was
     * created unpresented so its uninitialized buffer never flashed). The
     * 0→1 transition also kicks off the window's open (scale+fade) animation. */
    if (!pw->win->presented) {
        pw->win->presented = 1;
        comp_start_open_anim(pw->win);
    }
    glyph_window_mark_all_dirty(pw->win);
    comp->full_redraw = 1;
    return 1;
}

static int handle_set_admin(compositor_t *comp, lumen_client_t *cli,
                            const lumen_set_admin_t *req)
{
    proxy_window_t *pw = find_proxy(cli, req->window_id);
    if (!pw) return 0;  /* not this client's window — ignore */
    int admin = req->admin ? 1 : 0;
    if (pw->win->admin == admin) return 0;  /* no change */
    pw->win->admin = admin;
    glyph_window_mark_all_dirty(pw->win);
    comp->full_redraw = 1;
    dprintf(2, "[LUMEN] window %u admin=%d\n", req->window_id, admin);
    return 1;
}

static int handle_drag_start(compositor_t *comp, lumen_client_t *cli,
                             const lumen_drag_start_t *req)
{
    proxy_window_t *pw = find_proxy(cli, req->window_id);
    if (!pw) return 0;  /* not this client's window — ignore */
    if (req->op != LUMEN_DND_MOVE && req->op != LUMEN_DND_COPY) return 0;
    /* Wire strings are fixed-size and may lack the NUL — re-terminate. */
    char label[64], path[256];
    memcpy(label, req->label, sizeof(label)); label[sizeof(label) - 1] = '\0';
    memcpy(path, req->path, sizeof(path));    path[sizeof(path) - 1]   = '\0';
    if (!path[0]) return 0;
    comp_dnd_start(comp, pw->win, req->op, label, path);
    return 1;
}

static int handle_destroy_window(compositor_t *comp, lumen_client_t *cli,
                                   uint32_t window_id)
{
    for (int i = 0; i < cli->nwindows; i++) {
        proxy_window_t *pw = cli->windows[i];
        if (pw->id != window_id) continue;

        /* Snapshot dimensions before comp_remove_window frees pw->win. */
        size_t bufsz = (size_t)pw->win->client_w * pw->win->client_h
                       * sizeof(uint32_t);

        /* comp_remove_window calls glyph_window_destroy(pw->win); after this
         * pw->win is dangling and must not be touched. */
        comp_remove_window(comp, pw->win);
        comp->full_redraw = 1;

        munmap(pw->shared, bufsz);
        close(pw->memfd);
        free(pw);

        cli->windows[i] = cli->windows[--cli->nwindows];
        return 1;
    }
    return 0;
}

/* ── Client read + hangup ───────────────────────────────────────────── */

static int lumen_server_read(compositor_t *comp, lumen_client_t *cli)
{
    lumen_msg_hdr_t hdr;
    ssize_t n = read(cli->fd, &hdr, sizeof(hdr));
    if (n == 0) return -1;
    if (n < 0)  return (errno == EAGAIN) ? 0 : -1;
    if (n != (ssize_t)sizeof(hdr)) return -1;

    /* For every fixed-size opcode, require the client-declared hdr.len to
     * exactly match the kernel-side struct size before reading the body.
     * Without this the handler reads sizeof(req) regardless of what hdr.len
     * claimed, so a malformed hdr.len silently desyncs the stream against
     * the wire framing. Drop the connection on mismatch (safer than guessing).
     * The default path below still honours hdr.len for forward extensibility. */
    switch (hdr.op) {
    case LUMEN_OP_CREATE_WINDOW: {
        if (hdr.len != (uint32_t)sizeof(lumen_create_window_t)) return -1;
        lumen_create_window_t req;
        if (read(cli->fd, &req, sizeof(req)) != (ssize_t)sizeof(req)) return -1;
        return handle_create_window(comp, cli, &req);
    }
    case LUMEN_OP_DAMAGE: {
        if (hdr.len != (uint32_t)sizeof(lumen_damage_t)) return -1;
        lumen_damage_t req;
        if (read(cli->fd, &req, sizeof(req)) != (ssize_t)sizeof(req)) return -1;
        return handle_damage(comp, cli, req.window_id);
    }
    case LUMEN_OP_SET_TITLE: {
        if (hdr.len != (uint32_t)sizeof(lumen_set_title_t)) return -1;
        lumen_set_title_t req;
        if (read(cli->fd, &req, sizeof(req)) != (ssize_t)sizeof(req)) return -1;
        return 0;
    }
    case LUMEN_OP_DESTROY_WINDOW: {
        if (hdr.len != (uint32_t)sizeof(lumen_destroy_window_t)) return -1;
        lumen_destroy_window_t req;
        if (read(cli->fd, &req, sizeof(req)) != (ssize_t)sizeof(req)) return -1;
        return handle_destroy_window(comp, cli, req.window_id);
    }
    case LUMEN_OP_CREATE_PANEL: {
        if (hdr.len != (uint32_t)sizeof(lumen_create_panel_t)) return -1;
        lumen_create_panel_t req;
        if (read(cli->fd, &req, sizeof(req)) != (ssize_t)sizeof(req)) return -1;
        return handle_create_panel(comp, cli, &req);
    }
    case LUMEN_OP_INVOKE: {
        if (hdr.len != (uint32_t)sizeof(lumen_invoke_t)) return -1;
        lumen_invoke_t req;
        if (read(cli->fd, &req, sizeof(req)) != (ssize_t)sizeof(req)) return -1;
        return handle_invoke(comp, &req);
    }
    case LUMEN_OP_DRAG_START: {
        if (hdr.len != (uint32_t)sizeof(lumen_drag_start_t)) return -1;
        lumen_drag_start_t req;
        if (read(cli->fd, &req, sizeof(req)) != (ssize_t)sizeof(req)) return -1;
        return handle_drag_start(comp, cli, &req);
    }
    case LUMEN_OP_SET_ADMIN: {
        if (hdr.len != (uint32_t)sizeof(lumen_set_admin_t)) return -1;
        lumen_set_admin_t req;
        if (read(cli->fd, &req, sizeof(req)) != (ssize_t)sizeof(req)) return -1;
        return handle_set_admin(comp, cli, &req);
    }
    default: {
        char tmp[256];
        uint32_t rem = hdr.len;
        while (rem > 0) {
            ssize_t r = read(cli->fd, tmp,
                             rem < (uint32_t)sizeof(tmp)
                             ? rem : (uint32_t)sizeof(tmp));
            if (r <= 0) return -1;
            rem -= (uint32_t)r;
        }
        return 0;
    }
    }
}

static void lumen_server_hangup(compositor_t *comp, lumen_client_t *cli)
{
    for (int i = 0; i < cli->nwindows; i++) {
        proxy_window_t *pw = cli->windows[i];
        /* Snapshot dimensions before comp_remove_window frees pw->win. */
        size_t bufsz = (size_t)pw->win->client_w * pw->win->client_h
                       * sizeof(uint32_t);
        /* comp_remove_window calls glyph_window_destroy(pw->win); after this
         * pw->win is dangling and must not be touched. */
        comp_remove_window(comp, pw->win);
        munmap(pw->shared, bufsz);
        close(pw->memfd);
        free(pw);
    }
    if (cli->nwindows > 0)
        comp->full_redraw = 1;

    close(cli->fd);

    for (int i = 0; i < s_ncli; i++) {
        if (s_clients[i] == cli) {
            s_clients[i] = s_clients[--s_ncli];
            break;
        }
    }
    free(cli);
}

static void lumen_server_accept_fd(compositor_t *comp, int listen_fd)
{
    (void)comp;

    int fd = accept(listen_fd, NULL, NULL);
    if (fd < 0) {
        dprintf(2, "[LUMEN-SRV] accept failed: errno=%d\n", errno);
        return;
    }
    dprintf(2, "[LUMEN-SRV] accepted fd=%d\n", fd);

    fcntl(fd, F_SETFL, O_NONBLOCK);

    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int pr = poll(&pfd, 1, 500);
    if (pr <= 0) {
        dprintf(2, "[LUMEN-SRV] poll(hello) returned %d errno=%d, closing\n", pr, errno);
        close(fd);
        return;
    }

    lumen_hello_t hello;
    ssize_t rn = read(fd, &hello, sizeof(hello));
    if (rn != (ssize_t)sizeof(hello)) {
        dprintf(2, "[LUMEN-SRV] read(hello) returned %ld errno=%d, closing\n",
                (long)rn, errno);
        close(fd);
        return;
    }
    dprintf(2, "[LUMEN-SRV] hello read: magic=0x%x version=%u\n",
            hello.magic, hello.version);

    lumen_hello_reply_t reply;
    reply.magic   = LUMEN_MAGIC;
    reply.version = LUMEN_VERSION;

    if (hello.magic != LUMEN_MAGIC || hello.version != LUMEN_VERSION) {
        reply.status = 1;
        write(fd, &reply, sizeof(reply));
        close(fd);
        return;
    }

    if (s_ncli >= LUMEN_MAX_CLIENTS) {
        reply.status = 2;
        write(fd, &reply, sizeof(reply));
        close(fd);
        return;
    }

    reply.status = 0;
    write(fd, &reply, sizeof(reply));

    lumen_client_t *cli = calloc(1, sizeof(*cli));
    if (!cli) { close(fd); return; }
    cli->fd      = fd;
    cli->next_id = 1;
    s_clients[s_ncli++] = cli;
}

int lumen_server_init(void)
{
    int mr = mkdir("/run", 0755);
    if (mr < 0 && errno != EEXIST)
        dprintf(2, "[LUMEN-SRV] mkdir(/run) failed: errno=%d\n", errno);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        dprintf(2, "[LUMEN-SRV] socket(AF_UNIX) failed: errno=%d\n", errno);
        return -1;
    }

    unlink("/run/lumen.sock");

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, "/run/lumen.sock",
            sizeof(addr.sun_path) - 1);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        dprintf(2, "[LUMEN-SRV] bind(/run/lumen.sock) failed: errno=%d\n", errno);
        close(fd);
        return -1;
    }

    if (listen(fd, 8) < 0) {
        dprintf(2, "[LUMEN-SRV] listen failed: errno=%d\n", errno);
        close(fd);
        return -1;
    }

    fcntl(fd, F_SETFL, O_NONBLOCK);

    dprintf(2, "[LUMEN-SRV] listening on /run/lumen.sock fd=%d\n", fd);
    return fd;
}

int lumen_server_tick(compositor_t *comp, int listen_fd)
{
    int dirtied = 0;

    {
        struct pollfd pfd = { .fd = listen_fd, .events = POLLIN };
        if (poll(&pfd, 1, 0) > 0)
            lumen_server_accept_fd(comp, listen_fd);
    }

    for (int i = 0; i < s_ncli; ) {
        struct pollfd pfd = { .fd = s_clients[i]->fd, .events = POLLIN };
        if (poll(&pfd, 1, 0) > 0) {
            int r = lumen_server_read(comp, s_clients[i]);
            if (r < 0) {
                lumen_server_hangup(comp, s_clients[i]);
                continue;
            }
            if (r > 0)
                dirtied = 1;
        }
        i++;
    }

    return dirtied;
}
