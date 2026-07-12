/* lumen_proto.h — Lumen external window protocol wire structs.
 * Included by both the client library and the Lumen server. */
#ifndef LUMEN_PROTO_H
#define LUMEN_PROTO_H

#include <stdint.h>

#define LUMEN_MAGIC     0x4C4D454Eu  /* "LMEN" */
#define LUMEN_VERSION   1u

/* ── Handshake (no lumen_msg_hdr_t wrapper — sent raw) ──────────────── */

typedef struct {
    uint32_t magic;    /* LUMEN_MAGIC */
    uint32_t version;  /* LUMEN_VERSION */
} lumen_hello_t;

typedef struct {
    uint32_t magic;    /* LUMEN_MAGIC */
    uint32_t version;  /* echoed */
    uint32_t status;   /* 0=OK, 1=version unsupported, 2=server full */
} lumen_hello_reply_t;

/* ── Common framed message header ───────────────────────────────────── */

typedef struct {
    uint32_t op;   /* opcode */
    uint32_t len;  /* bytes of payload following this header */
} lumen_msg_hdr_t;

/* ── Client → server opcodes ────────────────────────────────────────── */

#define LUMEN_OP_CREATE_WINDOW   1u
#define LUMEN_OP_DAMAGE          2u
#define LUMEN_OP_SET_TITLE       3u
#define LUMEN_OP_DESTROY_WINDOW  4u
#define LUMEN_OP_CREATE_PANEL    5u  /* chromeless, bottom-anchored, no focus */
#define LUMEN_OP_INVOKE          6u  /* ask server to run a built-in by name */
#define LUMEN_OP_DRAG_START      7u  /* begin compositor-brokered drag-and-drop */
#define LUMEN_OP_SET_ADMIN       8u  /* set per-window admin (red toolbar) flag */
#define LUMEN_OP_RESIZE_BUFFER   9u  /* client asks for a new-sized shared buffer;
                                      * reply = lumen_window_created_t + memfd,
                                      * exactly like CREATE_WINDOW. Sent by the
                                      * client after it receives LUMEN_EV_RESIZED. */
#define LUMEN_OP_ACTIVATE_WINDOW 10u /* dock → server: un-minimize + raise + focus
                                      * the window with this global id */
#define LUMEN_OP_RESIZE_SELF     11u /* client (panel) → server: resize me to w×h.
                                      * Reply = lumen_window_created_t + memfd. Lets
                                      * the dock grow/shrink to fit its task area. */

/* Drag-and-drop operation requested by the SOURCE. The TARGET executes
 * it (the payload is a path, so the receiving app decides how to copy
 * or move). */
#define LUMEN_DND_MOVE 1u
#define LUMEN_DND_COPY 2u

/* CREATE_WINDOW flags */
#define LUMEN_WIN_FLAG_FULLSCREEN 0x0001u  /* chromeless, framebuffer-sized,
                                            * at 0,0, focusable; width/height
                                            * in the request are ignored and
                                            * the reply carries the real fb
                                            * dimensions. Used by the
                                            * Applications launcher. */
#define LUMEN_WIN_FLAG_RESIZABLE  0x0002u  /* window can be maximized/snapped;
                                            * the compositor shows the green
                                            * button and the client must handle
                                            * LUMEN_EV_RESIZED (rebuild its
                                            * surface). Off = today's fixed
                                            * window, unchanged. */
#define LUMEN_WIN_FLAG_ONTOP      0x0004u  /* composite this window above all
                                            * normal windows (never covered by a
                                            * raised window). Panels get this by
                                            * default; regular windows opt in. */

typedef struct {
    uint16_t width;
    uint16_t height;
    uint16_t flags;      /* LUMEN_WIN_FLAG_* */
    uint16_t _pad;
    char     title[64];  /* null-terminated */
} lumen_create_window_t;

/* Panel: no chrome, no focus, server positions at bottom-center.
 * Reply uses lumen_window_created_t (same as CREATE_WINDOW) + memfd. */
typedef struct {
    uint16_t width;
    uint16_t height;
} lumen_create_panel_t;

typedef struct {
    char name[32];   /* null-terminated, e.g. "terminal", "widgets" */
} lumen_invoke_t;

typedef struct {
    uint32_t window_id;
} lumen_damage_t;

typedef struct {
    uint32_t window_id;
    char     title[64];
} lumen_set_title_t;

typedef struct {
    uint32_t window_id;
} lumen_destroy_window_t;

/* SET_ADMIN: terminal reports its shell's admin-session state so Lumen
 * tints THAT window's titlebar red. admin=1 elevated, 0 normal. (Future:
 * scope to the active tab once multi-tab terminal windows exist.) */
typedef struct {
    uint32_t window_id;
    uint32_t admin;
} lumen_set_admin_t;

/* RESIZE_BUFFER: client's response to LUMEN_EV_RESIZED — asks the server to
 * allocate a fresh shared buffer at the new client-area size. The server
 * replies with lumen_window_created_t + the new memfd via SCM_RIGHTS. */
typedef struct {
    uint32_t window_id;
    uint32_t width;
    uint32_t height;
} lumen_resize_buffer_t;

/* ACTIVATE_WINDOW: dock asks the server to restore+raise+focus a window by its
 * global id (from LUMEN_EV_WINDOW_LIST). */
typedef struct {
    uint32_t gid;
} lumen_activate_window_t;

/* RESIZE_SELF: a client (the dock panel) asks to be resized to w×h. Reply is a
 * lumen_window_created_t + new memfd, same shape as CREATE_WINDOW. */
typedef struct {
    uint32_t width;
    uint32_t height;
} lumen_resize_self_t;

/* One window in LUMEN_EV_WINDOW_LIST. The event body is `count` of these back to
 * back (hdr.len = count * sizeof). */
typedef struct {
    uint32_t gid;
    uint8_t  minimized;
    uint8_t  focused;
    uint8_t  _pad[2];
    char     title[64];
} lumen_window_info_t;

/* DRAG_START: sent while the source still holds the left button (the
 * press that began in its content area). The compositor takes over:
 * draws the ghost label at the cursor, sends DRAG_OVER/DRAG_LEAVE to
 * the proxy window under the pointer, and delivers DROP on release. */
typedef struct {
    uint32_t window_id;  /* source window */
    uint8_t  op;         /* LUMEN_DND_* */
    uint8_t  _pad[3];
    char     label[64];  /* ghost text (e.g. file basename) */
    char     path[256];  /* payload: absolute path */
} lumen_drag_start_t;

/* ── Server → client (CREATE_WINDOW reply) ──────────────────────────── */
/* Sent as framed message (hdr.op=0) via sendmsg with SCM_RIGHTS memfd  */

typedef struct {
    uint32_t status;     /* 0=OK, nonzero=errno */
    uint32_t window_id;
    uint32_t width;      /* actual width Lumen assigned */
    uint32_t height;     /* actual height Lumen assigned */
    int32_t  x, y;       /* screen position Lumen placed the window at */
} lumen_window_created_t;

/* ── Server → client event opcodes ─────────────────────────────────── */

#define LUMEN_EV_KEY           0x10u
#define LUMEN_EV_MOUSE         0x11u
#define LUMEN_EV_CLOSE_REQUEST 0x12u
#define LUMEN_EV_FOCUS         0x13u
#define LUMEN_EV_RESIZED       0x14u  /* v1: defined but never sent */
#define LUMEN_EV_DRAG_OVER     0x15u  /* pointer over this window during DnD */
#define LUMEN_EV_DRAG_LEAVE    0x16u  /* pointer left this window during DnD */
#define LUMEN_EV_DROP          0x17u  /* release: payload delivered here */
#define LUMEN_EV_WINDOW_LIST   0x18u  /* server → dock: the open-window list
                                       * (body = N × lumen_window_info_t) */

/* LUMEN_EV_MOUSE evtype values */
#define LUMEN_MOUSE_MOVE  0u
#define LUMEN_MOUSE_DOWN  1u
#define LUMEN_MOUSE_UP    2u
#define LUMEN_MOUSE_WHEEL 3u  /* `scroll` carries the wheel delta (+up/−down) */

typedef struct {
    uint32_t window_id;
    uint32_t keycode;    /* (uint8_t)char from keyboard ISR */
    uint32_t modifiers;  /* reserved, 0 in v1 */
    uint8_t  pressed;    /* 1=down */
    uint8_t  _pad[3];
} lumen_key_event_t;

typedef struct {
    uint32_t window_id;
    int32_t  x, y;       /* client-area-relative (after subtracting chrome) */
    uint8_t  buttons;    /* bitmask: bit0=left, bit1=right, bit2=middle */
    uint8_t  evtype;     /* LUMEN_MOUSE_* */
    int8_t   scroll;     /* wheel delta (+up/−down), only for WHEEL evtype */
    uint8_t  _pad;
} lumen_mouse_event_t;

typedef struct {
    uint32_t window_id;
} lumen_close_request_t;

typedef struct {
    uint32_t window_id;
    uint8_t  focused;
    uint8_t  _pad[3];
} lumen_focus_event_t;

typedef struct {
    uint32_t window_id;
    uint32_t new_width;
    uint32_t new_height;
} lumen_resized_event_t;

typedef struct {
    uint32_t window_id;
    int32_t  x, y;       /* client-area-relative */
    uint8_t  op;         /* LUMEN_DND_* */
    uint8_t  _pad[3];
} lumen_drag_over_t;

typedef struct {
    uint32_t window_id;
} lumen_drag_leave_t;

typedef struct {
    uint32_t window_id;
    int32_t  x, y;       /* client-area-relative drop point */
    uint8_t  op;         /* LUMEN_DND_* */
    uint8_t  _pad[3];
    char     path[256];  /* payload */
} lumen_drop_event_t;

#endif /* LUMEN_PROTO_H */
