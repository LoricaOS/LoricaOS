/* lumen_client.c — Lumen external window protocol client implementation */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>

#include "lumen_client.h"

/* Write a diagnostic line to BOTH stderr AND /dev/console.
 * stderr may be a PTY (invisible in test-harness serial output) when
 * the client was launched from a Lumen terminal window. /dev/console
 * always reaches the kernel serial driver. */
static void
lumen_diag(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (n > (int)sizeof(buf)) n = (int)sizeof(buf);
    write(2, buf, (size_t)n);
    int cfd = open("/dev/console", O_WRONLY);
    if (cfd >= 0) { write(cfd, buf, (size_t)n); close(cfd); }
}

/* Read exactly n bytes from a stream fd, accumulating across short reads.
 * AF_UNIX SOCK_STREAM gives no message-boundary guarantee: a single read() of
 * a fixed-size protocol struct can return fewer bytes than requested, which
 * desyncs the framed wire format (every subsequent header then lands
 * mid-struct). This is the framing/reassembly primitive for Lumen clients —
 * read the header, then read exactly the body. Retries EINTR and EAGAIN (the
 * caller polls for readability first, and a partially-delivered frame's
 * remaining bytes are imminent on a local socket). Returns 0 on success, -1 on
 * peer close (EOF) or a hard error. Exported for other external clients. */
int lumen_read_full(int fd, void *buf, size_t n)
{
    uint8_t *p = (uint8_t *)buf;
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, p + got, n - got);
        if (r > 0) { got += (size_t)r; continue; }
        if (r == 0) return -1;                 /* peer closed mid-frame */
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
            continue;
        return -1;                             /* hard error */
    }
    return 0;
}

int lumen_connect(void)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        lumen_diag( "[LUMEN-CLI] socket: errno=%d\n", errno);
        return -errno;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, "/run/lumen.sock",
            sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        int err = errno;
        /* Silent on ECONNREFUSED (server not yet up — caller will retry).
         * Surface anything else immediately. */
        if (err != 111)
            lumen_diag( "[LUMEN-CLI] connect: errno=%d\n", err);
        close(fd);
        return -err;
    }

    lumen_hello_t hello = { LUMEN_MAGIC, LUMEN_VERSION };
    ssize_t wn = write(fd, &hello, sizeof(hello));
    if (wn != (ssize_t)sizeof(hello)) {
        lumen_diag( "[LUMEN-CLI] write hello: rc=%ld errno=%d\n", (long)wn, errno);
        close(fd); return -EIO;
    }

    lumen_hello_reply_t reply;
    ssize_t rn = read(fd, &reply, sizeof(reply));
    if (rn != (ssize_t)sizeof(reply)) {
        lumen_diag( "[LUMEN-CLI] read reply: rc=%ld errno=%d\n", (long)rn, errno);
        close(fd); return -EIO;
    }
    if (reply.magic != LUMEN_MAGIC || reply.status != 0) {
        lumen_diag( "[LUMEN-CLI] bad reply magic=0x%x status=%u\n",
                reply.magic, reply.status);
        close(fd); return -EPROTO;
    }

    return fd;
}

/* Connect, retrying while the compositor's listener isn't up yet. Aegis
 * AF_UNIX returns ECONNREFUSED (-111) until lumen's accept() runs, so a
 * client spawned alongside the compositor races its startup. 50×100ms = 5s.
 * Any non-ECONNREFUSED result (success or hard error) returns immediately. */
int lumen_connect_retry(void)
{
    int fd = -111;
    for (int attempt = 0; attempt < 50; attempt++) {
        fd = lumen_connect();
        if (fd != -111) break;
        usleep(100000);
    }
    return fd;
}

static int recv_event(int fd, lumen_event_t *ev)
{
    lumen_msg_hdr_t hdr;
    if (lumen_read_full(fd, &hdr, sizeof(hdr)) != 0) return -1;

    memset(ev, 0, sizeof(*ev));
    ev->type = hdr.op;

    switch (hdr.op) {
    case LUMEN_EV_KEY: {
        lumen_key_event_t k;
        if (lumen_read_full(fd, &k, sizeof(k)) != 0) return -1;
        ev->window_id     = k.window_id;
        ev->key.keycode   = k.keycode;
        ev->key.modifiers = k.modifiers;
        ev->key.pressed   = k.pressed;
        break;
    }
    case LUMEN_EV_MOUSE: {
        lumen_mouse_event_t m;
        if (lumen_read_full(fd, &m, sizeof(m)) != 0) return -1;
        ev->window_id      = m.window_id;
        ev->mouse.x        = m.x;
        ev->mouse.y        = m.y;
        ev->mouse.buttons  = m.buttons;
        ev->mouse.evtype   = m.evtype;
        ev->mouse.scroll   = m.scroll;
        break;
    }
    case LUMEN_EV_CLOSE_REQUEST: {
        lumen_close_request_t c;
        if (lumen_read_full(fd, &c, sizeof(c)) != 0) return -1;
        ev->window_id = c.window_id;
        break;
    }
    case LUMEN_EV_FOCUS: {
        lumen_focus_event_t f;
        if (lumen_read_full(fd, &f, sizeof(f)) != 0) return -1;
        ev->window_id    = f.window_id;
        ev->focus.focused = f.focused;
        break;
    }
    case LUMEN_EV_RESIZED: {
        lumen_resized_event_t r;
        if (lumen_read_full(fd, &r, sizeof(r)) != 0) return -1;
        ev->window_id     = r.window_id;
        ev->resized.new_w = r.new_width;
        ev->resized.new_h = r.new_height;
        break;
    }
    case LUMEN_EV_DRAG_OVER: {
        lumen_drag_over_t d;
        if (lumen_read_full(fd, &d, sizeof(d)) != 0) return -1;
        ev->window_id = d.window_id;
        ev->drag.x    = d.x;
        ev->drag.y    = d.y;
        ev->drag.op   = d.op;
        break;
    }
    case LUMEN_EV_DRAG_LEAVE: {
        lumen_drag_leave_t d;
        if (lumen_read_full(fd, &d, sizeof(d)) != 0) return -1;
        ev->window_id = d.window_id;
        break;
    }
    case LUMEN_EV_DROP: {
        lumen_drop_event_t d;
        if (lumen_read_full(fd, &d, sizeof(d)) != 0) return -1;
        ev->window_id = d.window_id;
        ev->drop.x    = d.x;
        ev->drop.y    = d.y;
        ev->drop.op   = d.op;
        memcpy(ev->drop.path, d.path, sizeof(ev->drop.path));
        ev->drop.path[sizeof(ev->drop.path) - 1] = '\0';
        break;
    }
    case LUMEN_EV_WINDOW_LIST: {
        static lumen_window_info_t s_wins[16];
        int count = (int)(hdr.len / sizeof(lumen_window_info_t));
        int cap = count > 16 ? 16 : count;
        for (int i = 0; i < cap; i++)
            if (lumen_read_full(fd, &s_wins[i], sizeof(s_wins[i])) != 0) return -1;
        uint32_t rem = hdr.len - (uint32_t)cap * sizeof(lumen_window_info_t);
        char tmp[256];
        while (rem > 0) {
            ssize_t r = read(fd, tmp,
                             rem < (uint32_t)sizeof(tmp) ? rem : (uint32_t)sizeof(tmp));
            if (r <= 0) return -1;
            rem -= (uint32_t)r;
        }
        ev->windows.count = cap;
        ev->windows.items = s_wins;
        break;
    }
    default: {
        char tmp[256];
        uint32_t rem = hdr.len;
        while (rem > 0) {
            ssize_t r = read(fd, tmp,
                             rem < (uint32_t)sizeof(tmp) ? rem : (uint32_t)sizeof(tmp));
            if (r <= 0) return -1;
            rem -= (uint32_t)r;
        }
        ev->type = 0;
        break;
    }
    }
    return 1;
}

/* Receive lumen_window_created_t reply + SCM_RIGHTS memfd, mmap, build win.
 * Used by both lumen_window_create() and lumen_panel_create(). */
static lumen_window_t *recv_created_reply(int fd, const char *what)
{
    lumen_msg_hdr_t rhdr;
    lumen_window_created_t created;

    char cmsgbuf[CMSG_SPACE(sizeof(int))];
    struct iovec iov[2] = {
        { .iov_base = &rhdr,    .iov_len = sizeof(rhdr)    },
        { .iov_base = &created, .iov_len = sizeof(created) },
    };
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov        = iov;
    msg.msg_iovlen     = 2;
    msg.msg_control    = cmsgbuf;
    msg.msg_controllen = sizeof(cmsgbuf);

    if (recvmsg(fd, &msg, 0) < 0) {
        lumen_diag("[LUMEN-CLI] %s: recvmsg errno=%d\n", what, errno);
        return NULL;
    }
    if (created.status != 0) {
        lumen_diag("[LUMEN-CLI] %s: server status=%u\n", what, created.status);
        return NULL;
    }

    int memfd = -1;
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (cmsg && cmsg->cmsg_level == SOL_SOCKET &&
        cmsg->cmsg_type == SCM_RIGHTS)
        memcpy(&memfd, CMSG_DATA(cmsg), sizeof(int));
    if (memfd < 0) {
        lumen_diag("[LUMEN-CLI] %s: no memfd in cmsg\n", what);
        return NULL;
    }

    size_t bufsz = (size_t)created.width * created.height * sizeof(uint32_t);

    void *shared = mmap(NULL, bufsz, PROT_READ | PROT_WRITE,
                        MAP_SHARED, memfd, 0);
    if (shared == MAP_FAILED) {
        lumen_diag("[LUMEN-CLI] %s: mmap errno=%d bufsz=%lu\n",
            what, errno, (unsigned long)bufsz);
        close(memfd); return NULL;
    }

    void *backbuf = malloc(bufsz);
    if (!backbuf) { munmap(shared, bufsz); close(memfd); return NULL; }
    memset(backbuf, 0, bufsz);

    lumen_window_t *win = malloc(sizeof(*win));
    if (!win) { free(backbuf); munmap(shared, bufsz); close(memfd); return NULL; }

    win->fd      = fd;
    win->id      = created.window_id;
    win->memfd   = memfd;
    win->shared  = shared;
    win->backbuf = backbuf;
    win->w       = (int)created.width;
    win->h       = (int)created.height;
    win->stride  = (int)created.width;
    win->x       = created.x;
    win->y       = created.y;

    return win;
}

lumen_window_t *lumen_window_create(int fd, const char *title, int w, int h)
{
    return lumen_window_create_ex(fd, title, w, h, 0);
}

lumen_window_t *lumen_window_create_ex(int fd, const char *title,
                                       int w, int h, unsigned flags)
{
    lumen_msg_hdr_t hdr = { LUMEN_OP_CREATE_WINDOW,
                             sizeof(lumen_create_window_t) };
    lumen_create_window_t req;
    memset(&req, 0, sizeof(req));
    req.width  = (uint16_t)w;
    req.height = (uint16_t)h;
    req.flags  = (uint16_t)flags;
    strncpy(req.title, title ? title : "", sizeof(req.title) - 1);

    if (write(fd, &hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr)) {
        lumen_diag("[LUMEN-CLI] window_create: write hdr errno=%d\n", errno);
        return NULL;
    }
    if (write(fd, &req, sizeof(req)) != (ssize_t)sizeof(req)) {
        lumen_diag("[LUMEN-CLI] window_create: write req errno=%d\n", errno);
        return NULL;
    }
    return recv_created_reply(fd, "window_create");
}

lumen_window_t *lumen_panel_create(int fd, int w, int h)
{
    lumen_msg_hdr_t hdr = { LUMEN_OP_CREATE_PANEL,
                             sizeof(lumen_create_panel_t) };
    lumen_create_panel_t req = { (uint16_t)w, (uint16_t)h };

    if (write(fd, &hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr)) {
        lumen_diag("[LUMEN-CLI] panel_create: write hdr errno=%d\n", errno);
        return NULL;
    }
    if (write(fd, &req, sizeof(req)) != (ssize_t)sizeof(req)) {
        lumen_diag("[LUMEN-CLI] panel_create: write req errno=%d\n", errno);
        return NULL;
    }
    return recv_created_reply(fd, "panel_create");
}

int lumen_drag_start(lumen_window_t *win, int op,
                     const char *label, const char *path)
{
    if (!win || !path || !path[0]) return -EINVAL;
    lumen_msg_hdr_t hdr = { LUMEN_OP_DRAG_START, sizeof(lumen_drag_start_t) };
    lumen_drag_start_t req;
    memset(&req, 0, sizeof(req));
    req.window_id = win->id;
    req.op = (uint8_t)op;
    if (label) strncpy(req.label, label, sizeof(req.label) - 1);
    strncpy(req.path, path, sizeof(req.path) - 1);
    if (write(win->fd, &hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr)) {
        lumen_diag("[LUMEN-CLI] drag_start: write hdr errno=%d\n", errno);
        return -EIO;
    }
    if (write(win->fd, &req, sizeof(req)) != (ssize_t)sizeof(req)) {
        lumen_diag("[LUMEN-CLI] drag_start: write req errno=%d\n", errno);
        return -EIO;
    }
    return 0;
}

int lumen_invoke(int fd, const char *name)
{
    lumen_msg_hdr_t hdr = { LUMEN_OP_INVOKE, sizeof(lumen_invoke_t) };
    lumen_invoke_t req;
    memset(&req, 0, sizeof(req));
    if (name) strncpy(req.name, name, sizeof(req.name) - 1);
    if (write(fd, &hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr)) {
        lumen_diag("[LUMEN-CLI] invoke: write hdr errno=%d\n", errno);
        return -EIO;
    }
    if (write(fd, &req, sizeof(req)) != (ssize_t)sizeof(req)) {
        lumen_diag("[LUMEN-CLI] invoke: write req errno=%d\n", errno);
        return -EIO;
    }
    return 0;
}

void lumen_window_set_admin(lumen_window_t *win, int admin)
{
    if (!win) return;
    lumen_msg_hdr_t hdr = { LUMEN_OP_SET_ADMIN, sizeof(lumen_set_admin_t) };
    lumen_set_admin_t req = { win->id, admin ? 1u : 0u };
    if (write(win->fd, &hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr)) {
        lumen_diag("[LUMEN-CLI] set_admin: write hdr errno=%d\n", errno);
        return;
    }
    if (write(win->fd, &req, sizeof(req)) != (ssize_t)sizeof(req))
        lumen_diag("[LUMEN-CLI] set_admin: write req errno=%d\n", errno);
}

void lumen_window_present(lumen_window_t *win)
{
    size_t bufsz = (size_t)win->w * win->h * sizeof(uint32_t);
    memcpy(win->shared, win->backbuf, bufsz);

    lumen_msg_hdr_t hdr = { LUMEN_OP_DAMAGE, sizeof(lumen_damage_t) };
    lumen_damage_t dmg  = { win->id };
    write(win->fd, &hdr, sizeof(hdr));
    write(win->fd, &dmg, sizeof(dmg));
}

/* Apply a server-initiated resize (received as LUMEN_EV_RESIZED). Asks the
 * server for a fresh buffer at new_w×new_h and remaps shared+backbuf in place;
 * win->w/h/stride are updated. Returns 0 on success, -1 on failure (window
 * left at its old size). The caller must rebuild its surface_t (it points at
 * the new backbuf) and repaint.
 *
 * ponytail: assumes the RESIZE_BUFFER reply is the next message on the socket
 * (the server handles it synchronously, like CREATE_WINDOW). True today; if the
 * server ever interleaves events before the reply, this needs a queue. */
/* Receive a resize reply (lumen_window_created_t + new memfd via SCM_RIGHTS) and
 * remap win->shared/backbuf in place. Shared by apply_resize (server-initiated)
 * and resize_self (client-initiated) — both send a request, then read this. */
static int recv_buffer_reply(lumen_window_t *win)
{
    lumen_msg_hdr_t rhdr;
    lumen_window_created_t created;
    char cmsgbuf[CMSG_SPACE(sizeof(int))];
    struct iovec iov[2] = {
        { .iov_base = &rhdr,    .iov_len = sizeof(rhdr)    },
        { .iov_base = &created, .iov_len = sizeof(created) },
    };
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = iov; msg.msg_iovlen = 2;
    msg.msg_control = cmsgbuf; msg.msg_controllen = sizeof(cmsgbuf);
    if (recvmsg(win->fd, &msg, 0) < 0 || created.status != 0) {
        lumen_diag("[LUMEN-CLI] resize: recvmsg errno=%d status=%u\n",
                   errno, created.status);
        return -1;
    }

    int memfd = -1;
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (cmsg && cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS)
        memcpy(&memfd, CMSG_DATA(cmsg), sizeof(int));
    if (memfd < 0) { lumen_diag("[LUMEN-CLI] resize: no memfd\n"); return -1; }

    size_t nbufsz = (size_t)created.width * created.height * sizeof(uint32_t);
    void *nshared = mmap(NULL, nbufsz, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, 0);
    if (nshared == MAP_FAILED) { close(memfd); return -1; }
    void *nback = malloc(nbufsz);
    if (!nback) { munmap(nshared, nbufsz); close(memfd); return -1; }
    memset(nback, 0, nbufsz);

    munmap(win->shared, (size_t)win->w * win->h * sizeof(uint32_t));
    close(win->memfd);
    free(win->backbuf);
    win->memfd  = memfd;
    win->shared = nshared;
    win->backbuf = nback;
    win->w = (int)created.width;
    win->h = (int)created.height;
    win->stride = (int)created.width;
    win->x = created.x;
    win->y = created.y;
    return 0;
}

/* ponytail: assumes the reply is the next message on the socket (the server
 * handles it synchronously, like CREATE_WINDOW). True today; if the server ever
 * interleaves events before the reply, this needs a queue. */
int lumen_window_apply_resize(lumen_window_t *win, int new_w, int new_h)
{
    if (!win || new_w < 1 || new_h < 1) return -1;
    lumen_msg_hdr_t hdr = { LUMEN_OP_RESIZE_BUFFER, sizeof(lumen_resize_buffer_t) };
    lumen_resize_buffer_t req = { win->id, (uint32_t)new_w, (uint32_t)new_h };
    if (write(win->fd, &hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr) ||
        write(win->fd, &req, sizeof(req)) != (ssize_t)sizeof(req)) {
        lumen_diag("[LUMEN-CLI] resize: write errno=%d\n", errno);
        return -1;
    }
    return recv_buffer_reply(win);
}

int lumen_window_resize_self(lumen_window_t *win, int w, int h)
{
    if (!win || w < 1 || h < 1) return -1;
    lumen_msg_hdr_t hdr = { LUMEN_OP_RESIZE_SELF, sizeof(lumen_resize_self_t) };
    lumen_resize_self_t req = { (uint32_t)w, (uint32_t)h };
    if (write(win->fd, &hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr) ||
        write(win->fd, &req, sizeof(req)) != (ssize_t)sizeof(req))
        return -1;
    return recv_buffer_reply(win);
}

void lumen_activate_window(int fd, uint32_t gid)
{
    lumen_msg_hdr_t hdr = { LUMEN_OP_ACTIVATE_WINDOW, sizeof(lumen_activate_window_t) };
    lumen_activate_window_t req = { gid };
    write(fd, &hdr, sizeof(hdr));
    write(fd, &req, sizeof(req));
}

int lumen_poll_event(int fd, lumen_event_t *ev)
{
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    if (poll(&pfd, 1, 0) <= 0) return 0;
    return recv_event(fd, ev);
}

int lumen_wait_event(int fd, lumen_event_t *ev, int timeout_ms)
{
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int r = poll(&pfd, 1, timeout_ms);
    if (r == 0) return 0;
    if (r < 0)  return -1;
    return recv_event(fd, ev);
}

void lumen_window_destroy(lumen_window_t *win)
{
    lumen_msg_hdr_t hdr  = { LUMEN_OP_DESTROY_WINDOW,
                              sizeof(lumen_destroy_window_t) };
    lumen_destroy_window_t req = { win->id };
    write(win->fd, &hdr, sizeof(hdr));
    write(win->fd, &req, sizeof(req));

    size_t bufsz = (size_t)win->w * win->h * sizeof(uint32_t);
    munmap(win->shared, bufsz);
    close(win->memfd);
    free(win->backbuf);
    free(win);
}
