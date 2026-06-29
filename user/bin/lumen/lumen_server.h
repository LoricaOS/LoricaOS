/* lumen_server.h — Lumen external window server interface */
#ifndef LUMEN_SERVER_H
#define LUMEN_SERVER_H

#include "compositor.h"

int lumen_server_init(void);
int lumen_server_tick(compositor_t *comp, int listen_fd);

/* Notify a proxy window of focus change. win may be NULL or non-proxy (no-op). */
void lumen_proxy_notify_focus(glyph_window_t *win, int focused);

/* Drag-and-drop event delivery to proxy windows (called from the
 * compositor while it brokers a drag). x,y are window-local; the
 * helpers convert to client-area coords like mouse events do. */
void lumen_proxy_send_drag_over(glyph_window_t *win, int x, int y, uint8_t op);
void lumen_proxy_send_drag_leave(glyph_window_t *win);
void lumen_proxy_send_drop(glyph_window_t *win, int x, int y, uint8_t op,
                           const char *path);

/* Register a handler for LUMEN_OP_INVOKE messages.
 * The handler receives the compositor and the requested name.
 * Lumen's main.c registers a dispatcher: "applications" (/bin/applications
 * shell component) is special; every other name resolves through the /apps
 * bundle registry (glyph_apps_find) and spawns the bundle's ELF. */
typedef void (*lumen_invoke_fn)(compositor_t *comp, const char *name);
void lumen_server_set_invoke_handler(lumen_invoke_fn fn);

#endif /* LUMEN_SERVER_H */
