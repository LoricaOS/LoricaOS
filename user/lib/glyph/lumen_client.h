/* lumen_client.h — Client API for connecting external processes to Lumen */
#ifndef LUMEN_CLIENT_H
#define LUMEN_CLIENT_H

#include <stdint.h>
#include <stddef.h>
#include "lumen_proto.h"

typedef struct {
    int      fd;      /* connection socket */
    uint32_t id;      /* window_id assigned by Lumen */
    int      memfd;   /* shared pixel buffer fd */
    void    *shared;  /* MAP_SHARED — Lumen reads this */
    void    *backbuf; /* malloc'd — caller renders here */
    int      w, h;    /* client area dimensions in pixels */
    int      stride;  /* stride in pixels (== w in v1) */
    int      x, y;    /* screen position Lumen placed the window at */
} lumen_window_t;

typedef struct {
    uint32_t type;       /* LUMEN_EV_* opcode */
    uint32_t window_id;
    union {
        struct { uint32_t keycode, modifiers; uint8_t pressed; } key;
        struct { int32_t x, y; uint8_t buttons, evtype; int8_t scroll; } mouse;
        struct { uint8_t focused; }                               focus;
        struct { uint32_t new_w, new_h; }                        resized;
        struct { int32_t x, y; uint8_t op; }                     drag;  /* DRAG_OVER */
        struct { int32_t x, y; uint8_t op; char path[256]; }     drop;  /* DROP */
        /* WINDOW_LIST: items points at a lib-owned lumen_window_info_t[count],
         * valid until the next lumen_poll/wait_event call. */
        struct { int count; const void *items; }                 windows;
    };
} lumen_event_t;

/* Read exactly n bytes from a stream fd, accumulating across short reads
 * (AF_UNIX SOCK_STREAM has no message boundaries). Returns 0 on success, -1 on
 * EOF/error. The framing primitive for external Lumen protocol clients. */
int lumen_read_full(int fd, void *buf, size_t n);
int lumen_connect(void);
/* connect(), retrying on ECONNREFUSED (server starting up): 50×100ms = 5s */
int lumen_connect_retry(void);
lumen_window_t *lumen_window_create(int fd, const char *title, int w, int h);
/* flags = LUMEN_WIN_FLAG_*. With FULLSCREEN, w/h are ignored and the
 * returned window carries the framebuffer dimensions. */
lumen_window_t *lumen_window_create_ex(int fd, const char *title,
                                       int w, int h, unsigned flags);
lumen_window_t *lumen_panel_create(int fd, int w, int h);
void lumen_window_present(lumen_window_t *win);
/* Apply a LUMEN_EV_RESIZED event: fetch a new-sized shared buffer and remap in
 * place (updates win->w/h/stride/shared/backbuf). Returns 0 on success. After
 * this the caller must rebuild any cached surface_t (it points at the new
 * backbuf) and repaint. Only meaningful for LUMEN_WIN_FLAG_RESIZABLE windows. */
int lumen_window_apply_resize(lumen_window_t *win, int new_w, int new_h);
/* Resize this (panel) window to w×h — client-initiated. Remaps in place like
 * apply_resize; returns 0 on success. Used by the dock to fit its task area. */
int lumen_window_resize_self(lumen_window_t *win, int w, int h);
/* Ask the compositor to restore+raise+focus the window with this global id
 * (from a LUMEN_EV_WINDOW_LIST entry). */
void lumen_activate_window(int fd, uint32_t gid);
int lumen_poll_event(int fd, lumen_event_t *ev);
int lumen_wait_event(int fd, lumen_event_t *ev, int timeout_ms);
void lumen_window_destroy(lumen_window_t *win);
int lumen_invoke(int fd, const char *name);
/* Tell Lumen this window's shell is (un)elevated to an admin session, so
 * the compositor tints the window titlebar red. admin != 0 = elevated. */
void lumen_window_set_admin(lumen_window_t *win, int admin);
/* Begin a compositor-brokered drag. Call while the left button (whose
 * press landed in win's content area) is still held. op = LUMEN_DND_*.
 * The window under the pointer at release receives LUMEN_EV_DROP with
 * the path payload and executes the operation itself. */
int lumen_drag_start(lumen_window_t *win, int op,
                     const char *label, const char *path);

#endif /* LUMEN_CLIENT_H */
