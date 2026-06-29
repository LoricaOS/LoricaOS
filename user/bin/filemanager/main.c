/* user/bin/filemanager/main.c — Aegis File Manager (external Lumen client)
 *
 * A traditional file manager speaking the Lumen external window protocol
 * (same pattern as settings / terminal / editor):
 *
 *   - toolbar: back / forward / up navigation + current path display,
 *     New Folder / Rename / Delete action buttons
 *   - column list view (Name | Size), folders first, scrollbar
 *   - history stacks behind back/forward
 *   - file operations: mkdir, rename, unlink/rmdir (with confirm),
 *     copy / cut / paste via an internal clipboard
 *   - Enter (or click on the selection) opens: folders descend, files
 *     spawn /bin/editor <path> as a sibling Lumen client
 *
 * Keys: arrows move, Enter opens, Backspace goes up, Esc closes (or
 * cancels a dialog), ^N new folder, ^R rename, ^D delete, ^C copy,
 * ^X cut, ^V paste. (Delete/F2 are E0 scancodes the PS/2 path doesn't
 * deliver yet — Phase 48 — hence the control-key bindings.)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/syscall.h>

#include <glyph.h>
#include <lumen_client.h>
#include "font.h"

#define WIN_W 640
#define WIN_H 480

#define TOOLBAR_H 36
#define COLHDR_H  22
#define STATUS_H  24
#define ROW_H     24
#define SBAR_W    10

#define MAX_ENTRIES 512
#define HIST_MAX    32

#define KEY_UP    ((char)0xF1)
#define KEY_DOWN  ((char)0xF2)
#define KEY_RIGHT ((char)0xF3)
#define KEY_LEFT  ((char)0xF4)

#define SYS_SPAWN 514

/* NOT C_TERM_BG: the compositor color-keys external window pixels on
 * exactly C_TERM_BG (frosted-glass for the terminal), so a flat fill of
 * it renders as translucent glass. THEME_SURFACE is well off the key. */
#define FM_BG THEME_SURFACE

/* Toolbar buttons (indexes into the hit-test table). */
enum {
    BTN_BACK, BTN_FWD, BTN_UP,
    BTN_NEWDIR, BTN_RENAME, BTN_DELETE,
    BTN_COUNT
};

typedef enum {
    MODAL_NONE = 0,
    MODAL_NEWDIR,
    MODAL_RENAME,
    MODAL_DELETE,
} modal_t;

/* Context-menu actions. */
enum {
    MA_OPEN, MA_RENAME, MA_COPY, MA_CUT, MA_PASTE, MA_DELETE, MA_NEWDIR,
    /* "Open with…" drills the same panel into a list of openers. */
    MA_OPENWITH, MA_OW_TOGGLE, MA_OW_EDITOR, MA_OW_TUNES, MA_OW_IMAGE
};

#define MENU_W       150
#define MENU_ITEM_H  26
#define MENU_MAX     8

#define DBLCLICK_MS  400
#define DRAG_SLOP    6   /* px of motion before a press becomes a drag */

typedef struct { const char *label; int action; } menu_item_t;

typedef struct {
    char  name[256];
    int   is_dir;
    long  size;
} fm_entry_t;

typedef struct {
    int             lfd;
    lumen_window_t *lwin;
    surface_t       surf;
    int             fb_w, fb_h;
    int             dirty;
    int             done;

    char       cwd[512];
    fm_entry_t entries[MAX_ENTRIES];
    int        nent;
    int        sel;
    int        top;            /* scroll: first visible row */

    /* history (back/forward) */
    char hist[HIST_MAX][512];
    int  hist_len;
    int  hist_pos;             /* index of current dir in hist */

    /* clipboard */
    char clip[800];
    char clip_name[256];
    int  clip_valid;
    int  clip_cut;
    int  clip_is_dir;

    /* modal dialog */
    modal_t modal;
    char    input[128];
    int     input_len;
    char    modal_msg[200];

    /* scrollbar drag */
    int sb_drag;
    int sb_drag_off;           /* pointer offset inside thumb at grab */

    /* context menu */
    int         menu_open;
    int         menu_x, menu_y;
    int         menu_hover;    /* item index under cursor, -1 none */
    menu_item_t menu_items[MENU_MAX];
    int         menu_n;

    /* "Open with" submenu: the file's extension, the "set default" checkbox
     * (unchecked each time the submenu is opened), and its dynamic label. */
    char        ow_ext[16];
    int         ow_default;
    char        ow_label[40];

    /* drag-to-move */
    int  press_idx;            /* row armed by left press, -1 none */
    int  press_x, press_y;
    int  drag_active;
    int  drag_target;          /* folder row idx, -2 = up button, -1 none */
    int  cur_mx, cur_my;       /* last pointer position */

    /* double-click detection */
    long last_click_ms;
    int  last_click_idx;

    char     status[160];
    uint32_t status_color;

    int  show_hidden;          /* 1 = show dotfiles (Ctrl+H toggles) */
} fm_state_t;

static fm_state_t g_fm;
static volatile sig_atomic_t s_term;
static void sigterm_handler(int s) { (void)s; s_term = 1; }

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* ── Drawing helpers ──────────────────────────────────────────────────── */

static void ui_text(int sz, int x, int y, const char *s, uint32_t color)
{
    if (g_font_ui)
        font_draw_text(&g_fm.surf, g_font_ui, sz, x, y, s, color);
    else
        draw_text_t(&g_fm.surf, x, y, s, color);
}

static int ui_w(int sz, const char *s)
{
    if (g_font_ui) return font_text_width(g_font_ui, sz, s);
    return (int)strlen(s) * FONT_W;
}

static int list_y(void)    { return TOOLBAR_H + COLHDR_H; }
static int list_h(void)    { return g_fm.fb_h - list_y() - STATUS_H; }
static int vis_rows(void)  { return list_h() / ROW_H; }

static void set_status(const char *msg, uint32_t color)
{
    snprintf(g_fm.status, sizeof(g_fm.status), "%s", msg);
    g_fm.status_color = color;
    g_fm.dirty = 1;
}

static void set_status_err(const char *op, int err)
{
    snprintf(g_fm.status, sizeof(g_fm.status), "%s: %s", op, strerror(err));
    g_fm.status_color = THEME_ERROR;
    g_fm.dirty = 1;
}

/* Human-readable size: "412 B", "1.3 KB", "2.0 MB". */
static void fmt_size(char *out, size_t cap, long sz)
{
    if (sz < 1024) {
        snprintf(out, cap, "%ld B", sz);
    } else if (sz < 1024L * 1024) {
        long k10 = (sz * 10) / 1024;
        snprintf(out, cap, "%ld.%ld KB", k10 / 10, k10 % 10);
    } else {
        long m10 = (sz * 10) / (1024L * 1024);
        snprintf(out, cap, "%ld.%ld MB", m10 / 10, m10 % 10);
    }
}

/* ── Directory loading ────────────────────────────────────────────────── */

static int entry_cmp(const void *a, const void *b)
{
    const fm_entry_t *x = a, *y = b;
    if (x->is_dir != y->is_dir) return y->is_dir - x->is_dir; /* dirs first */
    return strcasecmp(x->name, y->name);
}

/* Join dir and name into out (handles the "/" root case). */
static void join_path(char *out, size_t cap, const char *dir, const char *name)
{
    if (strcmp(dir, "/") == 0)
        snprintf(out, cap, "/%s", name);
    else
        snprintf(out, cap, "%s/%s", dir, name);
}

/* Load path into the entry list. Returns 0 on success, -1 on failure
 * (cwd and list untouched on failure). */
static int load_dir(const char *path)
{
    DIR *d = opendir(path);
    if (!d) {
        set_status_err(path, errno ? errno : ENOENT);
        return -1;
    }

    snprintf(g_fm.cwd, sizeof(g_fm.cwd), "%s", path);
    g_fm.nent = 0;
    g_fm.sel = 0;
    g_fm.top = 0;

    struct dirent *de;
    while ((de = readdir(d)) != NULL && g_fm.nent < MAX_ENTRIES) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        if (!g_fm.show_hidden && de->d_name[0] == '.')
            continue;
        fm_entry_t *e = &g_fm.entries[g_fm.nent];
        snprintf(e->name, sizeof(e->name), "%s", de->d_name);

        char full[800];
        join_path(full, sizeof(full), g_fm.cwd, de->d_name);
        struct stat stbuf;
        if (stat(full, &stbuf) == 0) {
            e->is_dir = S_ISDIR(stbuf.st_mode) ? 1 : 0;
            e->size = (long)stbuf.st_size;
        } else {
            e->is_dir = 0;
            e->size = 0;
        }
        g_fm.nent++;
    }
    closedir(d);

    qsort(g_fm.entries, (size_t)g_fm.nent, sizeof(fm_entry_t), entry_cmp);

    int dirs = 0, files = 0;
    for (int i = 0; i < g_fm.nent; i++) {
        if (g_fm.entries[i].is_dir) dirs++;
        else                        files++;
    }
    char msg[96];
    snprintf(msg, sizeof(msg), "%d folder%s, %d file%s",
             dirs, dirs == 1 ? "" : "s", files, files == 1 ? "" : "s");
    set_status(msg, THEME_TEXT_DIM);
    return 0;
}

/* Move selection to the entry with this name, if present. */
static void select_name(const char *name)
{
    for (int i = 0; i < g_fm.nent; i++) {
        if (strcmp(g_fm.entries[i].name, name) == 0) {
            g_fm.sel = i;
            break;
        }
    }
}

static void scroll_to_sel(void)
{
    int vr = vis_rows();
    if (vr < 1) vr = 1;
    if (g_fm.sel < g_fm.top) g_fm.top = g_fm.sel;
    if (g_fm.sel >= g_fm.top + vr) g_fm.top = g_fm.sel - vr + 1;
    if (g_fm.top < 0) g_fm.top = 0;
}

/* Reload cwd in place (after a file operation), keeping selection sane. */
static void reload_dir(void)
{
    char keep[256] = "";
    if (g_fm.sel >= 0 && g_fm.sel < g_fm.nent)
        snprintf(keep, sizeof(keep), "%s", g_fm.entries[g_fm.sel].name);
    char cur[512];
    snprintf(cur, sizeof(cur), "%s", g_fm.cwd);
    if (load_dir(cur) == 0 && keep[0])
        select_name(keep);
    if (g_fm.sel >= g_fm.nent) g_fm.sel = g_fm.nent - 1;
    if (g_fm.sel < 0) g_fm.sel = 0;
    scroll_to_sel();
}

/* ── History (back / forward) ─────────────────────────────────────────── */

static void hist_push(const char *path)
{
    /* drop forward entries */
    g_fm.hist_len = g_fm.hist_pos + 1;
    if (g_fm.hist_len >= HIST_MAX) {
        memmove(g_fm.hist[0], g_fm.hist[1],
                (size_t)(HIST_MAX - 1) * sizeof(g_fm.hist[0]));
        g_fm.hist_len = HIST_MAX - 1;
    }
    snprintf(g_fm.hist[g_fm.hist_len], sizeof(g_fm.hist[0]), "%s", path);
    g_fm.hist_pos = g_fm.hist_len;
    g_fm.hist_len++;
}

/* Navigate to a new directory, recording it in history. */
static void nav_to(const char *path)
{
    char target[512];
    snprintf(target, sizeof(target), "%s", path);
    if (load_dir(target) == 0)
        hist_push(target);
}

static void nav_back(void)
{
    if (g_fm.hist_pos <= 0) return;
    if (load_dir(g_fm.hist[g_fm.hist_pos - 1]) == 0)
        g_fm.hist_pos--;
}

static void nav_forward(void)
{
    if (g_fm.hist_pos + 1 >= g_fm.hist_len) return;
    if (load_dir(g_fm.hist[g_fm.hist_pos + 1]) == 0)
        g_fm.hist_pos++;
}

/* Parent directory of cwd ("/" stays "/"). */
static void parent_of(char *out, size_t cap)
{
    snprintf(out, cap, "%s", g_fm.cwd);
    char *cut = strrchr(out, '/');
    if (cut == out) out[1] = '\0';   /* "/foo" -> "/" */
    else if (cut)   *cut = '\0';     /* "/a/b" -> "/a" */
}

static void nav_up(void)
{
    if (strcmp(g_fm.cwd, "/") == 0) return;
    char child[256];
    /* select the dir we came from after going up */
    const char *slash = strrchr(g_fm.cwd, '/');
    snprintf(child, sizeof(child), "%s", slash ? slash + 1 : "");
    char parent[512];
    parent_of(parent, sizeof(parent));
    nav_to(parent);
    if (child[0]) { select_name(child); scroll_to_sel(); }
}

/* ── Opening entries ──────────────────────────────────────────────────── */

/* Case-insensitive check that `name` ends with `.<ext>`. */
static int has_ext(const char *name, const char *ext)
{
    size_t nl = strlen(name), el = strlen(ext);
    if (nl < el + 1 || name[nl - el - 1] != '.') return 0;
    const char *s = name + nl - el;
    for (size_t i = 0; i < el; i++) {
        char a = s[i], b = ext[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
    }
    return 1;
}

static int is_image(const char *name)
{
    return has_ext(name, "png") || has_ext(name, "bmp") ||
           has_ext(name, "jpg") || has_ext(name, "jpeg");
}

static int is_audio(const char *name)
{
    return has_ext(name, "wav") || has_ext(name, "mp3");
}

/* Spawn `app` (an /apps bundle ELF) on `path`. Returns the pid or <0. */
static long spawn_app(const char *app, const char *path, const char *label)
{
    extern char **environ;
    if (access(app, F_OK) != 0) {
        char m[120];
        snprintf(m, sizeof(m), "no %s installed", label);
        set_status(m, THEME_ERROR);
        return -1;
    }
    char *argv[] = { (char *)app, (char *)path, NULL };
    long pid = syscall(SYS_SPAWN, app, argv, environ,
                       2 /* stderr→/dev/console */, 0);
    if (pid < 0) {
        set_status_err("open", (int)-pid);
        return pid;
    }
    char msg[300];
    snprintf(msg, sizeof(msg), "opened %s", path);
    set_status(msg, THEME_OK);
    dprintf(2, "[FILES] open=%s %s_pid=%ld\n", path, label, pid);
    return pid;
}

/* ── Per-user "open with" defaults ($HOME/.openwith, one "ext app" per line,
 *    e.g. "mp3 /apps/tunes/tunes"). Set via the Open-with checkbox. ───────── */

static void openwith_path(char *out, size_t n)
{
    const char *home = getenv("HOME");
    if (!home || !*home) home = "/root";
    snprintf(out, n, "%s/.openwith", home);
}

/* Lowercase extension of `name` (after the last '.'), "" if none. */
static void file_ext(const char *name, char *out, size_t n)
{
    const char *dot = NULL;
    for (const char *s = name; *s; s++) if (*s == '.') dot = s + 1;
    size_t i = 0;
    if (dot)
        for (; dot[i] && i + 1 < n; i++) {
            char c = dot[i];
            out[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
        }
    out[i] = '\0';
}

static int openwith_read(char *buf, int cap)
{
    char path[256]; openwith_path(path, sizeof(path));
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    int n = (int)read(fd, buf, cap - 1);
    close(fd);
    if (n < 0) n = 0;
    buf[n] = '\0';
    return n;
}

/* Default app for `ext`; returns 1 + fills app[], or 0. */
static int openwith_lookup(const char *ext, char *app, size_t n)
{
    if (!ext || !*ext) return 0;
    char buf[2048];
    if (!openwith_read(buf, sizeof(buf))) return 0;
    for (char *line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
        char *sp = strchr(line, ' ');
        if (!sp) continue;
        *sp = '\0';
        if (strcmp(line, ext) == 0) { snprintf(app, n, "%s", sp + 1); return 1; }
    }
    return 0;
}

/* Set `ext` → `app`, replacing any prior association for `ext`. */
static void openwith_save(const char *ext, const char *app)
{
    if (!ext || !*ext) return;
    char buf[2048], out[2048];
    int o = 0;
    if (openwith_read(buf, sizeof(buf))) {
        for (char *line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
            char *sp = strchr(line, ' ');
            if (sp) {
                *sp = '\0';
                if (strcmp(line, ext) == 0) continue;   /* drop the old one */
                *sp = ' ';
            }
            o += snprintf(out + o, sizeof(out) - o, "%s\n", line);
            if (o >= (int)sizeof(out) - 64) break;
        }
    }
    o += snprintf(out + o, sizeof(out) - o, "%s %s\n", ext, app);
    char path[256]; openwith_path(path, sizeof(path));
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    (void)!write(fd, out, (size_t)o);
    close(fd);
}

static void activate(int idx)
{
    if (idx < 0 || idx >= g_fm.nent) return;
    fm_entry_t *e = &g_fm.entries[idx];
    char full[800];
    join_path(full, sizeof(full), g_fm.cwd, e->name);
    if (e->is_dir) {
        nav_to(full);
        return;
    }
    /* A user-set default for this extension wins over the built-in routing. */
    char ext[16], app[256];
    file_ext(e->name, ext, sizeof(ext));
    if (openwith_lookup(ext, app, sizeof(app))) {
        spawn_app(app, full, "default");
        return;
    }
    if (is_image(e->name)) {
        /* Image viewer has its own (larger) size handling — no editor cap. */
        spawn_app("/apps/imageviewer/imageviewer", full, "imageviewer");
    } else if (is_audio(e->name)) {
        spawn_app("/apps/tunes/tunes", full, "tunes");
    } else if (e->size > 512L * 1024) {
        set_status("file too large to open in the editor", THEME_WARN);
    } else {
        spawn_app("/apps/editor/editor", full, "editor");
    }
}

/* ── File operations ──────────────────────────────────────────────────── */

static fm_entry_t *sel_entry(void)
{
    if (g_fm.sel < 0 || g_fm.sel >= g_fm.nent) return NULL;
    return &g_fm.entries[g_fm.sel];
}

static void modal_open(modal_t kind)
{
    fm_entry_t *e = sel_entry();
    g_fm.input[0] = '\0';
    g_fm.input_len = 0;
    switch (kind) {
    case MODAL_NEWDIR:
        snprintf(g_fm.modal_msg, sizeof(g_fm.modal_msg), "New folder name:");
        break;
    case MODAL_RENAME:
        if (!e) return;
        snprintf(g_fm.modal_msg, sizeof(g_fm.modal_msg), "Rename %s to:",
                 e->name);
        snprintf(g_fm.input, sizeof(g_fm.input), "%s", e->name);
        g_fm.input_len = (int)strlen(g_fm.input);
        break;
    case MODAL_DELETE:
        if (!e) return;
        snprintf(g_fm.modal_msg, sizeof(g_fm.modal_msg),
                 "Delete %s \"%s\"?", e->is_dir ? "folder" : "file", e->name);
        break;
    default:
        return;
    }
    g_fm.modal = kind;
    g_fm.dirty = 1;
}

static void do_newdir(void)
{
    if (g_fm.input_len == 0) return;
    char full[800];
    join_path(full, sizeof(full), g_fm.cwd, g_fm.input);
    if (mkdir(full, 0755) != 0) {
        set_status_err("new folder", errno);
    } else {
        char keep[128];
        snprintf(keep, sizeof(keep), "%s", g_fm.input);
        reload_dir();
        select_name(keep);
        scroll_to_sel();
        dprintf(2, "[FILES] op=mkdir path=%s\n", full);
    }
}

static void do_rename(void)
{
    fm_entry_t *e = sel_entry();
    if (!e || g_fm.input_len == 0 || strcmp(e->name, g_fm.input) == 0)
        return;
    char from[800], to[800];
    join_path(from, sizeof(from), g_fm.cwd, e->name);
    join_path(to, sizeof(to), g_fm.cwd, g_fm.input);
    struct stat stbuf;
    if (stat(to, &stbuf) == 0) {
        set_status("rename: target already exists", THEME_ERROR);
        return;
    }
    if (rename(from, to) != 0) {
        set_status_err("rename", errno);
    } else {
        char keep[128];
        snprintf(keep, sizeof(keep), "%s", g_fm.input);
        reload_dir();
        select_name(keep);
        scroll_to_sel();
        dprintf(2, "[FILES] op=rename from=%s to=%s\n", from, to);
    }
}

static void do_delete(void)
{
    fm_entry_t *e = sel_entry();
    if (!e) return;
    char full[800];
    join_path(full, sizeof(full), g_fm.cwd, e->name);
    int r = e->is_dir ? rmdir(full) : unlink(full);
    if (r != 0) {
        set_status_err("delete", errno);
    } else {
        reload_dir();
        dprintf(2, "[FILES] op=delete path=%s\n", full);
    }
}

static void clip_set(int cut)
{
    fm_entry_t *e = sel_entry();
    if (!e) return;
    join_path(g_fm.clip, sizeof(g_fm.clip), g_fm.cwd, e->name);
    snprintf(g_fm.clip_name, sizeof(g_fm.clip_name), "%s", e->name);
    g_fm.clip_valid = 1;
    g_fm.clip_cut = cut;
    g_fm.clip_is_dir = e->is_dir;
    char msg[300];
    snprintf(msg, sizeof(msg), "%s %s — ^V to paste",
             cut ? "cut" : "copied", e->name);
    set_status(msg, THEME_OK);
}

/* Chunked file copy. Returns 0 on success, -errno on failure. */
static int copy_file(const char *from, const char *to)
{
    int sfd = open(from, O_RDONLY);
    if (sfd < 0) return -errno;
    int dfd = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (dfd < 0) { int e = errno; close(sfd); return -e; }
    static char buf[16384];
    int rc = 0;
    for (;;) {
        ssize_t n = read(sfd, buf, sizeof(buf));
        if (n == 0) break;
        if (n < 0) { rc = -errno; break; }
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(dfd, buf + off, (size_t)(n - off));
            if (w <= 0) { rc = -errno; break; }
            off += w;
        }
        if (rc) break;
    }
    close(sfd);
    close(dfd);
    if (rc) unlink(to);
    return rc;
}

static void do_paste(void)
{
    if (!g_fm.clip_valid) {
        set_status("clipboard is empty", THEME_WARN);
        return;
    }
    char dest[800];
    join_path(dest, sizeof(dest), g_fm.cwd, g_fm.clip_name);
    if (strcmp(dest, g_fm.clip) == 0) {
        set_status("source and destination are the same", THEME_WARN);
        return;
    }
    struct stat stbuf;
    if (stat(dest, &stbuf) == 0) {
        set_status("paste: target already exists", THEME_ERROR);
        return;
    }

    if (g_fm.clip_cut) {
        int r = rename(g_fm.clip, dest);
        if (r != 0 && !g_fm.clip_is_dir) {
            /* cross-fs move (e.g. /tmp ↔ ext2): copy + unlink fallback */
            r = copy_file(g_fm.clip, dest);
            if (r == 0) r = unlink(g_fm.clip) ? -errno : 0;
            else        errno = -r;
        }
        if (r != 0) { set_status_err("move", errno); return; }
        g_fm.clip_valid = 0;
        dprintf(2, "[FILES] op=move from=%s to=%s\n", g_fm.clip, dest);
    } else {
        if (g_fm.clip_is_dir) {
            set_status("copying folders is not supported yet", THEME_WARN);
            return;
        }
        int r = copy_file(g_fm.clip, dest);
        if (r != 0) { set_status_err("copy", -r); return; }
        dprintf(2, "[FILES] op=copy from=%s to=%s\n", g_fm.clip, dest);
    }
    reload_dir();
    select_name(g_fm.clip_name);
    scroll_to_sel();
}

/* ── Layout / hit testing ─────────────────────────────────────────────── */

typedef struct { int x, y, w, h; const char *label; } btn_rect_t;

/* Fill rects for all toolbar buttons (computed from fb_w so a clamped
 * window keeps a sane layout). */
static void toolbar_layout(btn_rect_t out[BTN_COUNT])
{
    int y = 5, h = TOOLBAR_H - 10;
    out[BTN_BACK]   = (btn_rect_t){ 8,   y, 30, h, "<" };
    out[BTN_FWD]    = (btn_rect_t){ 42,  y, 30, h, ">" };
    out[BTN_UP]     = (btn_rect_t){ 76,  y, 30, h, "^" };
    int dw = 60, rw = 70, nw = 92;
    int dx = g_fm.fb_w - 8 - dw;
    int rx = dx - 6 - rw;
    int nx = rx - 6 - nw;
    out[BTN_NEWDIR] = (btn_rect_t){ nx, y, nw, h, "New Folder" };
    out[BTN_RENAME] = (btn_rect_t){ rx, y, rw, h, "Rename" };
    out[BTN_DELETE] = (btn_rect_t){ dx, y, dw, h, "Delete" };
}

static int btn_enabled(int i)
{
    switch (i) {
    case BTN_BACK:   return g_fm.hist_pos > 0;
    case BTN_FWD:    return g_fm.hist_pos + 1 < g_fm.hist_len;
    case BTN_UP:     return strcmp(g_fm.cwd, "/") != 0;
    case BTN_RENAME:
    case BTN_DELETE: return sel_entry() != NULL;
    default:         return 1;
    }
}

static void btn_press(int i)
{
    if (!btn_enabled(i)) return;
    switch (i) {
    case BTN_BACK:   nav_back();              break;
    case BTN_FWD:    nav_forward();           break;
    case BTN_UP:     nav_up();                break;
    case BTN_NEWDIR: modal_open(MODAL_NEWDIR); break;
    case BTN_RENAME: modal_open(MODAL_RENAME); break;
    case BTN_DELETE: modal_open(MODAL_DELETE); break;
    }
}

/* Scrollbar geometry. Returns 0 if no scrollbar is needed. */
static int sbar_geom(int *track_y, int *track_h, int *thumb_y, int *thumb_h)
{
    int vr = vis_rows();
    if (g_fm.nent <= vr) return 0;
    *track_y = list_y();
    *track_h = list_h();
    int th = *track_h * vr / g_fm.nent;
    if (th < 24) th = 24;
    int range = *track_h - th;
    int maxtop = g_fm.nent - vr;
    *thumb_y = *track_y + (maxtop > 0 ? range * g_fm.top / maxtop : 0);
    *thumb_h = th;
    return 1;
}

/* ── Drag-and-drop (compositor-brokered, see lumen_proto.h) ──────────── */

/* Execute a drop: move/copy `path` into dest_dir. Shared by every DnD
 * source — our own row drags routed back through Lumen, and any future
 * app (desktop, viewer, ...) that drags a file onto this window. */
static void drop_into(const char *path, const char *dest_dir, int op)
{
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    if (!base[0]) return;

    struct stat stsrc;
    if (stat(path, &stsrc) != 0) {
        set_status_err("drop", errno ? errno : ENOENT);
        return;
    }
    int src_is_dir = S_ISDIR(stsrc.st_mode) ? 1 : 0;

    char dest[800];
    join_path(dest, sizeof(dest), dest_dir, base);
    if (strcmp(dest, path) == 0) return;   /* dropped where it lives */

    if (src_is_dir) {
        size_t plen = strlen(path);
        if (strncmp(dest_dir, path, plen) == 0 &&
            (dest_dir[plen] == '/' || dest_dir[plen] == '\0')) {
            set_status("cannot move a folder into itself", THEME_WARN);
            return;
        }
    }

    struct stat stbuf;
    if (stat(dest, &stbuf) == 0) {
        set_status("drop: target already exists", THEME_ERROR);
        return;
    }

    if (op == LUMEN_DND_COPY) {
        if (src_is_dir) {
            set_status("copying folders is not supported yet", THEME_WARN);
            return;
        }
        int r = copy_file(path, dest);
        if (r != 0) { set_status_err("copy", -r); return; }
        dprintf(2, "[FILES] op=copy from=%s to=%s\n", path, dest);
    } else {
        int r = rename(path, dest);
        if (r != 0 && !src_is_dir) {
            /* cross-fs move: copy + unlink fallback */
            r = copy_file(path, dest);
            if (r == 0) r = unlink(path) ? -errno : 0;
            else        errno = -r;
        }
        if (r != 0) { set_status_err("move", errno); return; }
        dprintf(2, "[FILES] op=move from=%s to=%s\n", path, dest);
    }
    reload_dir();
    select_name(base);
    scroll_to_sel();
}

/* Resolve the drop/hover target at client point (x, y).
 * Returns 0 = cwd, 1 = folder row (*row set), 2 = parent (via the "^"
 * toolbar button), -1 = not droppable right now. */
static int drop_resolve(int x, int y, char *out, size_t cap, int *row)
{
    *row = -1;
    if (g_fm.modal != MODAL_NONE) return -1;
    if (y < TOOLBAR_H) {
        btn_rect_t b[BTN_COUNT];
        toolbar_layout(b);
        if (x >= b[BTN_UP].x && x < b[BTN_UP].x + b[BTN_UP].w &&
            strcmp(g_fm.cwd, "/") != 0) {
            parent_of(out, cap);
            return 2;
        }
        snprintf(out, cap, "%s", g_fm.cwd);
        return 0;
    }
    int ly = list_y();
    if (y >= ly && y < ly + vis_rows() * ROW_H && x < g_fm.fb_w - SBAR_W) {
        int idx = g_fm.top + (y - ly) / ROW_H;
        if (idx >= 0 && idx < g_fm.nent && g_fm.entries[idx].is_dir) {
            *row = idx;
            join_path(out, cap, g_fm.cwd, g_fm.entries[idx].name);
            return 1;
        }
    }
    snprintf(out, cap, "%s", g_fm.cwd);
    return 0;
}

static void handle_drag_over(int x, int y)
{
    char dir[800];
    int row;
    int kind = drop_resolve(x, y, dir, sizeof(dir), &row);
    int t = (kind == 1) ? row : (kind == 2 ? -2 : -1);
    if (t != g_fm.drag_target) {
        g_fm.drag_target = t;
        g_fm.dirty = 1;
    }
}

static void handle_drag_leave(void)
{
    if (g_fm.drag_target != -1) {
        g_fm.drag_target = -1;
        g_fm.dirty = 1;
    }
}

static void handle_drop(int x, int y, int op, const char *path)
{
    g_fm.drag_target = -1;
    g_fm.dirty = 1;
    char dir[800];
    int row;
    int kind = drop_resolve(x, y, dir, sizeof(dir), &row);
    if (kind < 0 || !path[0]) return;
    drop_into(path, dir, op);
}

/* ── Context menu ─────────────────────────────────────────────────────── */

static void menu_add(const char *label, int action)
{
    if (g_fm.menu_n >= MENU_MAX) return;
    g_fm.menu_items[g_fm.menu_n].label  = label;
    g_fm.menu_items[g_fm.menu_n].action = action;
    g_fm.menu_n++;
}

static void ctxmenu_open(int x, int y, int target_row)
{
    g_fm.menu_n = 0;
    if (target_row >= 0) {
        g_fm.sel = target_row;
        scroll_to_sel();
        menu_add("Open",       MA_OPEN);
        if (!g_fm.entries[target_row].is_dir)
            menu_add("Open with…", MA_OPENWITH);
        menu_add("Rename", MA_RENAME);
        menu_add("Copy",   MA_COPY);
        menu_add("Cut",    MA_CUT);
        if (g_fm.clip_valid) menu_add("Paste", MA_PASTE);
        menu_add("Delete", MA_DELETE);
    } else {
        menu_add("New Folder", MA_NEWDIR);
        if (g_fm.clip_valid) menu_add("Paste", MA_PASTE);
    }
    int mh = g_fm.menu_n * MENU_ITEM_H + 8;
    if (x + MENU_W > g_fm.fb_w) x = g_fm.fb_w - MENU_W - 2;
    if (y + mh > g_fm.fb_h)     y = g_fm.fb_h - mh - 2;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    g_fm.menu_x = x;
    g_fm.menu_y = y;
    g_fm.menu_hover = -1;
    g_fm.menu_open = 1;
    g_fm.dirty = 1;
}

static int ctxmenu_item_at(int x, int y)
{
    if (x < g_fm.menu_x || x >= g_fm.menu_x + MENU_W) return -1;
    int rel = y - g_fm.menu_y - 4;
    if (rel < 0) return -1;
    int i = rel / MENU_ITEM_H;
    return (i < g_fm.menu_n) ? i : -1;
}

/* Re-fill the open context-menu panel with the list of openers (a drill-in,
 * so we reuse the single-panel render + hit-test). Anchored at the current
 * menu position, re-clamped for the new height. */
static void ctxmenu_openwith(void)
{
    int x = g_fm.menu_x, y = g_fm.menu_y;
    g_fm.menu_n = 0;
    /* A "set default" checkbox (unchecked by default) — only when the file has
     * an extension to key the association on. */
    if (g_fm.ow_ext[0]) {
        snprintf(g_fm.ow_label, sizeof(g_fm.ow_label), "%s Set as default",
                 g_fm.ow_default ? "[x]" : "[ ]");
        menu_add(g_fm.ow_label, MA_OW_TOGGLE);
    }
    menu_add("Editor",       MA_OW_EDITOR);
    menu_add("Tunes",        MA_OW_TUNES);
    menu_add("Image Viewer", MA_OW_IMAGE);
    int mh = g_fm.menu_n * MENU_ITEM_H + 8;
    if (y + mh > g_fm.fb_h) y = g_fm.fb_h - mh - 2;
    if (y < 0) y = 0;
    g_fm.menu_x = x;
    g_fm.menu_y = y;
    g_fm.menu_hover = -1;
    g_fm.menu_open = 1;
    g_fm.dirty = 1;
}

/* Spawn a specific app on the selected file. If the "set default" box was
 * ticked, remember ext→app so future opens use it (the Windows behavior). */
static void open_with(const char *app, const char *label)
{
    fm_entry_t *e = sel_entry();
    if (!e || e->is_dir) return;
    char full[800];
    join_path(full, sizeof(full), g_fm.cwd, e->name);
    spawn_app(app, full, label);
    if (g_fm.ow_default && g_fm.ow_ext[0]) {
        openwith_save(g_fm.ow_ext, app);
        char m[80];
        snprintf(m, sizeof(m), "default for .%s set to %s", g_fm.ow_ext, label);
        set_status(m, THEME_OK);
    }
}

static void ctxmenu_exec(int action)
{
    switch (action) {
    case MA_OPEN:      activate(g_fm.sel);       break;
    case MA_OPENWITH: {
        fm_entry_t *e = sel_entry();
        if (e) file_ext(e->name, g_fm.ow_ext, sizeof(g_fm.ow_ext));
        else   g_fm.ow_ext[0] = '\0';
        g_fm.ow_default = 0;                     /* unchecked by default */
        ctxmenu_openwith();
        break;
    }
    case MA_OW_TOGGLE:
        g_fm.ow_default = !g_fm.ow_default;
        ctxmenu_openwith();                      /* redraw with the new state */
        break;
    case MA_OW_EDITOR: open_with("/apps/editor/editor", "editor");           break;
    case MA_OW_TUNES:  open_with("/apps/tunes/tunes", "tunes");              break;
    case MA_OW_IMAGE:  open_with("/apps/imageviewer/imageviewer", "imageviewer"); break;
    case MA_RENAME:    modal_open(MODAL_RENAME); break;
    case MA_COPY:      clip_set(0);              break;
    case MA_CUT:       clip_set(1);              break;
    case MA_PASTE:     do_paste();               break;
    case MA_DELETE:    modal_open(MODAL_DELETE); break;
    case MA_NEWDIR:    modal_open(MODAL_NEWDIR); break;
    }
}

/* ── Render ───────────────────────────────────────────────────────────── */

static void draw_folder_glyph(int x, int y)
{
    draw_rounded_rect(&g_fm.surf, x, y + 4, 8, 4, 1, 0x00CC9944);
    draw_rounded_rect(&g_fm.surf, x, y + 6, 16, 11, 2, 0x00CC9944);
}

static void draw_file_glyph(int x, int y)
{
    draw_fill_rect(&g_fm.surf, x + 2, y + 3, 12, 15, 0x00708090);
    draw_fill_rect(&g_fm.surf, x + 4, y + 6, 8, 1, 0x00C0C8D0);
    draw_fill_rect(&g_fm.surf, x + 4, y + 9, 8, 1, 0x00C0C8D0);
    draw_fill_rect(&g_fm.surf, x + 4, y + 12, 6, 1, 0x00C0C8D0);
}

static void render_toolbar(void)
{
    surface_t *s = &g_fm.surf;
    draw_fill_rect(s, 0, 0, g_fm.fb_w, TOOLBAR_H, THEME_SURFACE_2);
    draw_fill_rect(s, 0, TOOLBAR_H - 1, g_fm.fb_w, 1, THEME_BORDER);

    btn_rect_t b[BTN_COUNT];
    toolbar_layout(b);
    for (int i = 0; i < BTN_COUNT; i++) {
        int en = btn_enabled(i);
        draw_rounded_rect(s, b[i].x, b[i].y, b[i].w, b[i].h, 4,
                          en ? THEME_SURFACE_2 : THEME_SURFACE);
        uint32_t fg = en ? THEME_TEXT : THEME_TEXT_FAINT;
        int lw = ui_w(14, b[i].label);
        ui_text(14, b[i].x + (b[i].w - lw) / 2,
                b[i].y + (b[i].h - 16) / 2, b[i].label, fg);
    }
    /* DnD hovering the "^" button: dropping moves into the parent. */
    if (g_fm.drag_target == -2)
        draw_rect(s, b[BTN_UP].x - 1, b[BTN_UP].y - 1,
                  b[BTN_UP].w + 2, b[BTN_UP].h + 2, C_ACCENT);

    /* Path display between nav and action buttons, tail-truncated. */
    int px = b[BTN_UP].x + b[BTN_UP].w + 10;
    int pw = b[BTN_NEWDIR].x - 10 - px;
    if (pw > 40) {
        draw_rounded_rect(s, px, 5, pw, TOOLBAR_H - 10, 4, C_INPUT_BG);
        const char *p = g_fm.cwd;
        size_t plen = strlen(p);
        while (plen > 1 && ui_w(14, p) > pw - 16) { p++; plen--; }
        char shown[520];
        if (p != g_fm.cwd) snprintf(shown, sizeof(shown), "…%s", p + 1);
        else               snprintf(shown, sizeof(shown), "%s", p);
        ui_text(14, px + 8, 5 + (TOOLBAR_H - 10 - 16) / 2, shown,
                THEME_TEXT);
    }
}

static void render_modal(void)
{
    surface_t *s = &g_fm.surf;
    draw_blend_rect(s, 0, 0, g_fm.fb_w, g_fm.fb_h, 0x00000000, 120);

    int mw = 400, mh = (g_fm.modal == MODAL_DELETE) ? 110 : 140;
    int mx = (g_fm.fb_w - mw) / 2, my = (g_fm.fb_h - mh) / 2;
    draw_rounded_rect(s, mx, my, mw, mh, 8, THEME_SURFACE_2);
    draw_rect(s, mx, my, mw, mh, THEME_BORDER_STRONG);

    ui_text(15, mx + 16, my + 12, g_fm.modal_msg, THEME_TEXT);

    int by = my + mh - 40;
    if (g_fm.modal != MODAL_DELETE) {
        /* input box with cursor */
        draw_rounded_rect(s, mx + 16, my + 44, mw - 32, 28, 4, C_INPUT_BG);
        draw_rect(s, mx + 16, my + 44, mw - 32, 28, C_INPUT_BD);
        ui_text(15, mx + 24, my + 49, g_fm.input, THEME_TEXT);
        int cx = mx + 24 + ui_w(15, g_fm.input);
        draw_fill_rect(s, cx + 1, my + 48, 2, 20, THEME_TEXT);
    }

    const char *ok = (g_fm.modal == MODAL_DELETE) ? "Delete" : "OK";
    /* OK + Cancel buttons (geometry mirrored in modal_click) */
    draw_rounded_rect(s, mx + mw - 180, by, 80, 28, 4,
                      (g_fm.modal == MODAL_DELETE) ? THEME_ERROR : C_BTN);
    ui_text(14, mx + mw - 180 + (80 - ui_w(14, ok)) / 2, by + 6, ok,
            THEME_TEXT_ON_ACCENT);
    draw_rounded_rect(s, mx + mw - 92, by, 80, 28, 4, THEME_SURFACE_2);
    ui_text(14, mx + mw - 92 + (80 - ui_w(14, "Cancel")) / 2, by + 6,
            "Cancel", THEME_TEXT);
}

static void render(void)
{
    if (!g_fm.dirty) return;
    g_fm.dirty = 0;
    surface_t *s = &g_fm.surf;

    draw_fill_rect(s, 0, 0, g_fm.fb_w, g_fm.fb_h, FM_BG);

    render_toolbar();

    /* Column headers. */
    int hy = TOOLBAR_H;
    draw_fill_rect(s, 0, hy, g_fm.fb_w, COLHDR_H, THEME_SURFACE_2);
    draw_fill_rect(s, 0, hy + COLHDR_H - 1, g_fm.fb_w, 1, THEME_BORDER);
    ui_text(13, 40, hy + 3, "Name", THEME_TEXT_DIM);
    int szw = ui_w(13, "Size");
    ui_text(13, g_fm.fb_w - SBAR_W - 14 - szw, hy + 3, "Size", THEME_TEXT_DIM);

    /* List. */
    int vr = vis_rows();
    int ly = list_y();
    for (int r = 0; r < vr; r++) {
        int idx = g_fm.top + r;
        if (idx >= g_fm.nent) break;
        fm_entry_t *e = &g_fm.entries[idx];
        int y = ly + r * ROW_H;
        if (idx == g_fm.sel)
            draw_fill_rect(s, 0, y, g_fm.fb_w - SBAR_W, ROW_H, C_SEL_BG);
        else if (r & 1)
            draw_fill_rect(s, 0, y, g_fm.fb_w - SBAR_W, ROW_H, THEME_INPUT_BG);
        if (idx == g_fm.drag_target)   /* DnD: folder under the pointer */
            draw_rect(s, 0, y, g_fm.fb_w - SBAR_W, ROW_H, C_ACCENT);
        if (e->is_dir) draw_folder_glyph(12, y + (ROW_H - 18) / 2);
        else           draw_file_glyph(12, y + (ROW_H - 18) / 2);

        /* dimmed cut-clipboard entry */
        uint32_t col = e->is_dir ? THEME_TEXT : THEME_TEXT_DIM;
        if (g_fm.clip_valid && g_fm.clip_cut) {
            char full[800];
            join_path(full, sizeof(full), g_fm.cwd, e->name);
            if (strcmp(full, g_fm.clip) == 0) col = THEME_TEXT_FAINT;
        }
        ui_text(15, 40, y + (ROW_H - 17) / 2, e->name, col);

        if (!e->is_dir) {
            char sz[32];
            fmt_size(sz, sizeof(sz), e->size);
            int w = ui_w(13, sz);
            ui_text(13, g_fm.fb_w - SBAR_W - 14 - w, y + (ROW_H - 15) / 2,
                    sz, THEME_TEXT_DIM);
        }
    }

    /* Scrollbar. */
    {
        int ty, th, my, mh;
        if (sbar_geom(&ty, &th, &my, &mh)) {
            draw_fill_rect(s, g_fm.fb_w - SBAR_W, ty, SBAR_W, th, THEME_INPUT_BG);
            draw_rounded_rect(s, g_fm.fb_w - SBAR_W + 2, my, SBAR_W - 4, mh,
                              3, THEME_BORDER_STRONG);
        }
    }

    /* Status bar. */
    int sy = g_fm.fb_h - STATUS_H;
    draw_fill_rect(s, 0, sy, g_fm.fb_w, STATUS_H, THEME_SURFACE_2);
    draw_fill_rect(s, 0, sy, g_fm.fb_w, 1, THEME_BORDER);
    if (g_fm.status[0])
        ui_text(13, 12, sy + (STATUS_H - 15) / 2, g_fm.status,
                g_fm.status_color);
    const char *hint = "^N new  ^R rename  ^D delete  ^C/^X/^V clipboard";
    int hw = ui_w(13, hint);
    if (12 + ui_w(13, g_fm.status) + 24 + hw < g_fm.fb_w)
        ui_text(13, g_fm.fb_w - hw - 12, sy + (STATUS_H - 15) / 2,
                hint, THEME_TEXT_DIM);

    if (g_fm.modal != MODAL_NONE)
        render_modal();

    if (g_fm.menu_open) {
        int mh = g_fm.menu_n * MENU_ITEM_H + 8;
        draw_fill_rect(s, g_fm.menu_x + 3, g_fm.menu_y + 3, MENU_W, mh,
                       0x00060610);   /* soft shadow */
        draw_fill_rect(s, g_fm.menu_x, g_fm.menu_y, MENU_W, mh, THEME_SURFACE_2);
        draw_rect(s, g_fm.menu_x, g_fm.menu_y, MENU_W, mh, THEME_BORDER_STRONG);
        for (int i = 0; i < g_fm.menu_n; i++) {
            int iy = g_fm.menu_y + 4 + i * MENU_ITEM_H;
            if (i == g_fm.menu_hover)
                draw_fill_rect(s, g_fm.menu_x + 2, iy, MENU_W - 4,
                               MENU_ITEM_H, C_SEL_BG);
            ui_text(14, g_fm.menu_x + 14, iy + (MENU_ITEM_H - 16) / 2,
                    g_fm.menu_items[i].label, THEME_TEXT);
        }
    }

    lumen_window_present(g_fm.lwin);
}

/* ── Input ────────────────────────────────────────────────────────────── */

static void modal_confirm(void)
{
    modal_t m = g_fm.modal;
    g_fm.modal = MODAL_NONE;
    g_fm.dirty = 1;
    switch (m) {
    case MODAL_NEWDIR: do_newdir(); break;
    case MODAL_RENAME: do_rename(); break;
    case MODAL_DELETE: do_delete(); break;
    default: break;
    }
}

static void modal_key(char c)
{
    if (c == '\x1b') { g_fm.modal = MODAL_NONE; g_fm.dirty = 1; return; }
    if (c == '\r' || c == '\n') { modal_confirm(); return; }
    if (g_fm.modal == MODAL_DELETE) return;  /* confirm-only dialog */
    if (c == '\b' || c == 0x7f) {
        if (g_fm.input_len > 0) {
            g_fm.input[--g_fm.input_len] = '\0';
            g_fm.dirty = 1;
        }
        return;
    }
    /* printable, no '/' (it would create paths) */
    if (c >= 0x20 && c < 0x7f && c != '/' &&
        g_fm.input_len < (int)sizeof(g_fm.input) - 1) {
        g_fm.input[g_fm.input_len++] = c;
        g_fm.input[g_fm.input_len] = '\0';
        g_fm.dirty = 1;
    }
}

static void handle_key(char c)
{
    if (g_fm.menu_open) {
        if (c == '\x1b') { g_fm.menu_open = 0; g_fm.dirty = 1; }
        else if (c == KEY_UP) {
            g_fm.menu_hover = (g_fm.menu_hover <= 0) ? g_fm.menu_n - 1
                                                     : g_fm.menu_hover - 1;
            g_fm.dirty = 1;
        } else if (c == KEY_DOWN) {
            g_fm.menu_hover = (g_fm.menu_hover + 1) % g_fm.menu_n;
            g_fm.dirty = 1;
        } else if (c == '\r' || c == '\n') {
            int i = g_fm.menu_hover;
            g_fm.menu_open = 0;
            g_fm.dirty = 1;
            if (i >= 0 && i < g_fm.menu_n)
                ctxmenu_exec(g_fm.menu_items[i].action);
        }
        return;
    }
    if (g_fm.modal != MODAL_NONE) { modal_key(c); return; }

    switch (c) {
    case '\x1b': g_fm.done = 1; return;
    case KEY_UP:
        if (g_fm.sel > 0) { g_fm.sel--; scroll_to_sel(); g_fm.dirty = 1; }
        break;
    case KEY_DOWN:
        if (g_fm.sel < g_fm.nent - 1) { g_fm.sel++; scroll_to_sel(); g_fm.dirty = 1; }
        break;
    case KEY_LEFT:  nav_back();    break;
    case KEY_RIGHT:
    case '\r': case '\n':
        activate(g_fm.sel);
        break;
    case '\b': case 0x7f:
        nav_up();
        break;
    case 0x0E: modal_open(MODAL_NEWDIR); break;  /* ^N */
    case 0x12: modal_open(MODAL_RENAME); break;  /* ^R */
    case 0x04: modal_open(MODAL_DELETE); break;  /* ^D */
    case 0x03: clip_set(0); break;               /* ^C */
    case 0x18: clip_set(1); break;               /* ^X */
    case 0x16: do_paste();  break;               /* ^V */
    case 'm': case 'M':                          /* open context menu on selection */
        ctxmenu_open(g_fm.fb_w / 3, 120,
                     (g_fm.sel >= 0 && g_fm.sel < g_fm.nent) ? g_fm.sel : -1);
        break;
    case '.':                                    /* toggle hidden (dotfiles) */
        g_fm.show_hidden = !g_fm.show_hidden;
        reload_dir();
        set_status(g_fm.show_hidden ? "hidden files shown"
                                    : "hidden files hidden", THEME_TEXT_DIM);
        g_fm.dirty = 1;
        break;
    default: break;
    }
}

static void modal_click(int x, int y)
{
    int mw = 400, mh = (g_fm.modal == MODAL_DELETE) ? 110 : 140;
    int mx = (g_fm.fb_w - mw) / 2, my = (g_fm.fb_h - mh) / 2;
    int by = my + mh - 40;
    if (y >= by && y < by + 28) {
        if (x >= mx + mw - 180 && x < mx + mw - 100) { modal_confirm(); return; }
        if (x >= mx + mw - 92 && x < mx + mw - 12) {
            g_fm.modal = MODAL_NONE;
            g_fm.dirty = 1;
            return;
        }
    }
}

static void handle_mouse(lumen_event_t *ev)
{
    int x = ev->mouse.x, y = ev->mouse.y;
    g_fm.cur_mx = x;
    g_fm.cur_my = y;

    if (ev->mouse.evtype == LUMEN_MOUSE_UP) {
        g_fm.sb_drag = 0;
        g_fm.press_idx = -1;
        return;
    }

    if (ev->mouse.evtype == LUMEN_MOUSE_MOVE) {
        if (g_fm.menu_open) {
            int h = ctxmenu_item_at(x, y);
            if (h != g_fm.menu_hover) { g_fm.menu_hover = h; g_fm.dirty = 1; }
            return;
        }
        if (g_fm.sb_drag) {
            int ty, th, my, mh;
            if (sbar_geom(&ty, &th, &my, &mh)) {
                int range = th - mh;
                int maxtop = g_fm.nent - vis_rows();
                if (range > 0 && maxtop > 0) {
                    int rel = y - g_fm.sb_drag_off - ty;
                    if (rel < 0) rel = 0;
                    if (rel > range) rel = range;
                    int newtop = rel * maxtop / range;
                    if (newtop != g_fm.top) { g_fm.top = newtop; g_fm.dirty = 1; }
                }
            }
            return;
        }
        /* Button held on a row past the slop: hand the gesture to Lumen
         * as a drag-and-drop. From here the compositor draws the ghost
         * and we hear back via DRAG_OVER/DRAG_LEAVE/DROP events. */
        if (g_fm.press_idx >= 0 && g_fm.press_idx < g_fm.nent &&
            abs(x - g_fm.press_x) + abs(y - g_fm.press_y) > DRAG_SLOP) {
            fm_entry_t *e = &g_fm.entries[g_fm.press_idx];
            char src[800];
            join_path(src, sizeof(src), g_fm.cwd, e->name);
            lumen_drag_start(g_fm.lwin, LUMEN_DND_MOVE, e->name, src);
            dprintf(2, "[FILES] drag_start path=%s\n", src);
            g_fm.press_idx = -1;
        }
        return;
    }

    if (ev->mouse.evtype != LUMEN_MOUSE_DOWN)
        return;

    /* Right-click: context menu (row menu or background menu). */
    if (ev->mouse.buttons & 2) {
        if (g_fm.modal != MODAL_NONE) return;
        int ly = list_y();
        if (y >= ly && y < ly + vis_rows() * ROW_H &&
            x < g_fm.fb_w - SBAR_W) {
            int idx = g_fm.top + (y - ly) / ROW_H;
            ctxmenu_open(x, y, (idx >= 0 && idx < g_fm.nent) ? idx : -1);
        } else if (y >= ly && y < g_fm.fb_h - STATUS_H) {
            ctxmenu_open(x, y, -1);
        }
        return;
    }

    if (!(ev->mouse.buttons & 1))
        return;

    /* Open menu: clicks act on it (or dismiss it). */
    if (g_fm.menu_open) {
        int i = ctxmenu_item_at(x, y);
        g_fm.menu_open = 0;
        g_fm.dirty = 1;
        if (i >= 0) ctxmenu_exec(g_fm.menu_items[i].action);
        return;
    }

    if (g_fm.modal != MODAL_NONE) { modal_click(x, y); return; }

    /* Toolbar buttons. */
    if (y < TOOLBAR_H) {
        btn_rect_t b[BTN_COUNT];
        toolbar_layout(b);
        for (int i = 0; i < BTN_COUNT; i++) {
            if (x >= b[i].x && x < b[i].x + b[i].w &&
                y >= b[i].y && y < b[i].y + b[i].h) {
                btn_press(i);
                return;
            }
        }
        return;
    }

    /* Scrollbar. */
    {
        int ty, th, my, mh;
        if (x >= g_fm.fb_w - SBAR_W && sbar_geom(&ty, &th, &my, &mh)) {
            if (y >= my && y < my + mh) {
                g_fm.sb_drag = 1;
                g_fm.sb_drag_off = y - my;
            } else if (y >= ty && y < ty + th) {
                int vr = vis_rows();
                g_fm.top += (y < my) ? -vr : vr;
                if (g_fm.top > g_fm.nent - vr) g_fm.top = g_fm.nent - vr;
                if (g_fm.top < 0) g_fm.top = 0;
                g_fm.dirty = 1;
            }
            return;
        }
    }

    /* List rows: press selects (and arms a potential drag); a second
     * press on the same row within DBLCLICK_MS opens it. */
    int ly = list_y();
    if (y >= ly && y < ly + vis_rows() * ROW_H) {
        int idx = g_fm.top + (y - ly) / ROW_H;
        if (idx < 0 || idx >= g_fm.nent) { g_fm.press_idx = -1; return; }
        long now = now_ms();
        if (idx == g_fm.last_click_idx &&
            now - g_fm.last_click_ms < DBLCLICK_MS) {
            g_fm.last_click_idx = -1;
            g_fm.press_idx = -1;
            activate(idx);
            return;
        }
        g_fm.sel = idx;
        g_fm.dirty = 1;
        g_fm.press_idx = idx;
        g_fm.press_x = x;
        g_fm.press_y = y;
        g_fm.last_click_idx = idx;
        g_fm.last_click_ms = now;
    }
}

/* ── Main ─────────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    const char *start = (argc > 1) ? argv[1] : NULL;
    if (!start) {
        start = getenv("HOME");
        if (!start || !start[0]) start = "/";
    }

    g_fm.lfd = lumen_connect_retry();
    if (g_fm.lfd < 0) {
        dprintf(2, "[FILES] lumen_connect failed (%d)\n", g_fm.lfd);
        return 1;
    }

    /* Clamp the window on small framebuffers. */
    int win_w = WIN_W, win_h = WIN_H;
    {
        const char *fw = getenv("LUMEN_FB_W");
        const char *fh = getenv("LUMEN_FB_H");
        if (fw && atoi(fw) > 0 && win_w > atoi(fw) - 64)
            win_w = atoi(fw) - 64;
        if (fh && atoi(fh) > 0 && win_h > atoi(fh) - 96)
            win_h = atoi(fh) - 96;
        if (win_w < 400) win_w = 400;
        if (win_h < 300) win_h = 300;
    }

    g_fm.lwin = lumen_window_create(g_fm.lfd, "Files", win_w, win_h);
    if (!g_fm.lwin) {
        dprintf(2, "[FILES] lumen_window_create failed\n");
        close(g_fm.lfd);
        return 1;
    }
    g_fm.fb_w = g_fm.lwin->w;
    g_fm.fb_h = g_fm.lwin->h;
    g_fm.surf = (surface_t){
        .buf = (uint32_t *)g_fm.lwin->backbuf,
        .w = g_fm.fb_w, .h = g_fm.fb_h, .pitch = g_fm.lwin->stride,
    };

    font_init();

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigterm_handler;
    sigaction(SIGTERM, &sa, NULL);

    g_fm.press_idx      = -1;
    g_fm.drag_target    = -1;
    g_fm.last_click_idx = -1;
    g_fm.menu_hover     = -1;

    if (load_dir(start) != 0) {
        if (load_dir("/") != 0) {
            dprintf(2, "[FILES] cannot open / — giving up\n");
            return 1;
        }
    }
    hist_push(g_fm.cwd);
    g_fm.dirty = 1;
    render();

    dprintf(2, "[FILES] connected %dx%d at %d,%d cwd=%s\n",
            g_fm.lwin->w, g_fm.lwin->h, g_fm.lwin->x, g_fm.lwin->y,
            g_fm.cwd);

    while (!s_term && !g_fm.done) {
        lumen_event_t ev;
        int r = lumen_wait_event(g_fm.lfd, &ev, 16);
        if (r < 0) break;
        if (r == 1) {
            if (ev.type == LUMEN_EV_CLOSE_REQUEST) break;
            if (ev.type == LUMEN_EV_KEY && ev.key.pressed)
                handle_key((char)ev.key.keycode);
            if (ev.type == LUMEN_EV_MOUSE)
                handle_mouse(&ev);
            if (ev.type == LUMEN_EV_DRAG_OVER)
                handle_drag_over(ev.drag.x, ev.drag.y);
            if (ev.type == LUMEN_EV_DRAG_LEAVE)
                handle_drag_leave();
            if (ev.type == LUMEN_EV_DROP)
                handle_drop(ev.drop.x, ev.drop.y, ev.drop.op, ev.drop.path);
        }
        render();
    }

    lumen_window_destroy(g_fm.lwin);
    close(g_fm.lfd);
    dprintf(2, "[FILES] exit\n");
    return 0;
}
