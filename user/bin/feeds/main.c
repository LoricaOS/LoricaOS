/* user/bin/feeds/main.c -- Aegis "Feeds" RSS/Atom reader (Lumen GUI integrator)
 *
 * The fifth and final module of the Feeds app: the three-pane reader that ties
 * together its four sibling modules -- feeds_net (fetch), feeds_parse (RSS/Atom),
 * feeds_store (subscriptions + read-state), feeds_text (HTML->plain text). This
 * file owns the Lumen window, the layout, the input handling and the controller
 * loop; it only ever *calls* the siblings through the prototypes in feeds.h.
 *
 *   ┌────────────────────────────────────────────────────────────────────┐
 *   │ Feeds                              5 unread · r refresh · a add · q  │  top bar
 *   ├──────────────┬───────────────────────┬─────────────────────────────┤
 *   │ Feeds        │ Articles              │ Reader                       │
 *   │  LWN         │  A title......   Jun 12   │  Article title (large)       │
 *   │ *Hacker News │ •B title......   Jun 11   │  Jun 12 · https://...          │
 *   │  Phoronix  3 │  C title......   Jun 10   │  ──────────────────────────  │
 *   │              │                       │  Word-wrapped body text that │
 *   │              │                       │  scrolls vertically.........       │
 *   └──────────────┴───────────────────────┴─────────────────────────────┘
 *     pane 1 (250)    pane 2 (360)            pane 3 (rest)
 *
 * Controls:
 *   Tab / Shift-Tab   cycle focus between the three panes
 *   Up/Down, k/j      move selection in the focused list / scroll reader a line
 *   mouse wheel       page the focused list / scroll the reader (PgUp/PgDn are
 *                     E0 scancodes the PS/2 path doesn't deliver yet, Phase 48)
 *   Enter             Feeds->load+focus Articles; Articles->open+mark read+focus Reader
 *   r / R             refresh selected feed / all feeds
 *   a                 add a subscription (top bar becomes a URL input)
 *   d                 delete the selected feed
 *   q / Esc           quit (Esc first cancels add-mode); close button quits
 *
 * Pure userspace (musl). Integer-only -- the Aegis FPU is fragile, so all text
 * measurement goes through font_text_width and all scrolling is in pixels/lines.
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
#include "font.h"

#include "feeds.h"

/* ── Window + pane geometry ───────────────────────────────────────────── */
#define WIN_W       1024
#define WIN_H       680

#define TOPBAR_H    40
#define PANE_Y      TOPBAR_H            /* panes start below the top bar      */
#define PANE_HDR_H  24                  /* "Feeds"/"Articles"/"Reader" header */

#define P1_W        250                 /* Feeds pane preferred width         */
#define P2_W        360                 /* Articles pane preferred width      */
#define P3_MIN      280                 /* Reader pane minimum usable width   */
#define DIV_W       1                   /* divider line between panes         */

#define FEED_ROW_H  30                  /* one subscription row               */
#define ART_ROW_H   42                  /* one article row (title + date)     */

#define PAD         12                  /* inner padding inside a pane        */
#define SBAR_W      8                   /* reader scrollbar width             */

/* Font sizes (pixels). */
#define FS_TITLE    20                  /* top-bar "Feeds"                    */
#define FS_HDR      13                  /* pane header labels                 */
#define FS_FEED     15                  /* subscription title                 */
#define FS_ART      15                  /* article title                      */
#define FS_DATE     12                  /* article / reader dates             */
#define FS_BIG      24                  /* reader article title               */
#define FS_BODY     16                  /* reader body                        */
#define FS_HINT     13                  /* top-bar status / hint              */

#define LINE_H      22                  /* reader body line advance (px)      */

/* Synthetic arrow keycodes Lumen delivers to proxy windows (same as 2048). */
#define KEY_UP      ((char)0xF1)
#define KEY_DOWN    ((char)0xF2)
#define KEY_RIGHT   ((char)0xF3)
#define KEY_LEFT    ((char)0xF4)
#define KEY_ESC     '\x1b'
#define KEY_TAB     '\t'

/* ── Colors ───────────────────────────────────────────────────────────────
 * Reuse draw.h's dark palette. The compositor color-keys external window
 * pixels on exactly C_TERM_BG (0x000A0A14) for frosted glass, so a flat fill
 * of it would ghost -- every background here stays one step off the key.
 * Only the names below are NEW (not provided by draw.h). */
#define FEEDS_BG    0x000E1420          /* window backdrop (off the C_TERM_BG key) */
#define FEEDS_PANE  0x00121A28          /* pane body                          */
#define FEEDS_HDRBG 0x00182233          /* pane header strip                  */
#define FEEDS_BAR   0x001A2433          /* top bar                            */
#define FEEDS_SEL   0x00305880          /* selection in a FOCUSED pane (==C_SEL_BG) */
#define FEEDS_SEL_D 0x001E2C40          /* selection in an UNFOCUSED pane     */
#define FEEDS_DIV   0x00283448          /* divider / borders                  */
#define FEEDS_DOT   0x004488CC          /* unread accent dot (==C_ACCENT)     */
#define FEEDS_DIM   0x00707C90          /* read / secondary text              */
#define FEEDS_DIMR  0x008894A8          /* slightly brighter dim (dates)      */
/* draw.h provides: C_TEXT, C_SUBTLE, C_ACCENT, C_RED, C_TITLE_T, C_INPUT_BG,
 * C_INPUT_BD -- reused directly below. */

/* ── Focusable panes ──────────────────────────────────────────────────── */
enum { PANE_FEEDS = 0, PANE_ARTS = 1, PANE_READER = 2, PANE_COUNT = 3 };

/* ── Application state ─────────────────────────────────────────────────── */
typedef struct {
    int             lfd;
    lumen_window_t *lwin;
    surface_t       surf;
    int             fb_w, fb_h;
    int             p1w, p2w;       /* actual pane widths (scaled to fb_w)   */
    int             dirty, done;

    feed_db_t      *db;             /* malloc'd once (~0.6 MB)               */

    int  focus;                     /* PANE_FEEDS / PANE_ARTS / PANE_READER  */
    int  feed_sel;                  /* selected subscription index           */
    int  feed_top;                  /* first visible feed row (scroll)       */
    int  art_sel;                   /* selected article index (in feed_sel)  */
    int  art_top;                   /* first visible article row (scroll)    */
    int  reader_scroll;             /* reader body scroll, in pixels         */
    int  reader_max;                /* max reader scroll (content height-vp) */

    int  refreshing;                /* 1 while a blocking fetch is in flight */
    char refresh_title[STR_TITLE];  /* what we're refreshing right now       */

    int  adding;                    /* 1 = top bar is the "add feed" input   */
    char add_buf[STR_URL];
    int  add_len;
    char flash[160];                /* transient status message (e.g. errors)*/
} app_t;

static app_t A;
static volatile sig_atomic_t s_term;
static void sigterm_handler(int s) { (void)s; s_term = 1; }

/* ── Text helpers (TTF if available, bitmap fallback) ─────────────────── */
static void text(int sz, int x, int y, const char *s, uint32_t color)
{
    if (g_font_ui) font_draw_text(&A.surf, g_font_ui, sz, x, y, s, color);
    else           draw_text_t(&A.surf, x, y, s, color);
}
static int text_w(int sz, const char *s)
{
    if (g_font_ui) return font_text_width(g_font_ui, sz, s);
    return (int)strlen(s) * FONT_W;
}

/* Copy s into out, truncated with "..." so it fits within max_w pixels.
 * Pure measurement loop (font_text_width) -- no float, byte-at-a-time. */
static void ellipsize(char *out, size_t cap, int sz, const char *s, int max_w)
{
    if (cap == 0) return;
    if (text_w(sz, s) <= max_w) {       /* fits whole */
        snprintf(out, cap, "%s", s);
        return;
    }
    const char *ell = "\xe2\x80\xa6";   /* UTF-8 ... */
    int ell_w = text_w(sz, ell);
    size_t n = 0;
    char tmp[512];
    /* Grow the prefix one char at a time until prefix+ellipsis would overflow. */
    for (size_t i = 0; s[i] && n < sizeof(tmp) - 1 && n < cap - 1; i++) {
        tmp[n] = s[i];
        tmp[n + 1] = '\0';
        if (text_w(sz, tmp) + ell_w > max_w) {
            tmp[n] = '\0';              /* drop the char that pushed us over */
            break;
        }
        n++;
    }
    snprintf(out, cap, "%s%s", tmp, ell);
}

/* ── Unread bookkeeping ──────────────────────────────────────────────── */
static int feed_unread(const feed_t *f)
{
    int n = 0;
    for (int i = 0; i < f->n_items; i++)
        if (!f->items[i].read) n++;
    return n;
}
static int total_unread(void)
{
    int n = 0;
    for (int i = 0; i < A.db->n_feeds; i++)
        n += feed_unread(&A.db->feeds[i]);
    return n;
}

/* Currently-selected feed / article, or NULL. */
static feed_t *cur_feed(void)
{
    if (A.feed_sel < 0 || A.feed_sel >= A.db->n_feeds) return NULL;
    return &A.db->feeds[A.feed_sel];
}
static feed_item_t *cur_item(void)
{
    feed_t *f = cur_feed();
    if (!f || A.art_sel < 0 || A.art_sel >= f->n_items) return NULL;
    return &f->items[A.art_sel];
}

/* ── Pane rects ────────────────────────────────────────────────────────
 * Pane widths are computed once per window in layout_panes(): the reader
 * (pane 3) always keeps at least P3_MIN px, shrinking pane 1 then pane 2 on
 * narrow framebuffers. p1w/p2w are stored in state; the reader takes the rest. */
static void layout_panes(void)
{
    A.p1w = P1_W;
    A.p2w = P2_W;
    int min_total = A.p1w + DIV_W + A.p2w + DIV_W + P3_MIN;
    if (A.fb_w < min_total) {
        int over = min_total - A.fb_w;
        /* steal from pane 2 first, then pane 1, keeping floors */
        int take2 = over; if (take2 > A.p2w - 180) take2 = A.p2w - 180;
        if (take2 < 0) take2 = 0;
        A.p2w -= take2; over -= take2;
        if (over > 0) {
            int take1 = over; if (take1 > A.p1w - 140) take1 = A.p1w - 140;
            if (take1 < 0) take1 = 0;
            A.p1w -= take1;
        }
    }
}
static int p1_x(void)   { return 0; }
static int p2_x(void)   { return A.p1w + DIV_W; }
static int p3_x(void)   { return A.p1w + DIV_W + A.p2w + DIV_W; }
static int p3_w(void)   { int w = A.fb_w - p3_x(); return w < 1 ? 1 : w; }
static int pane_h(void) { return A.fb_h - PANE_Y; }
/* List area (below the pane header). */
static int list_y(void)        { return PANE_Y + PANE_HDR_H; }
static int list_h(void)        { return A.fb_h - list_y(); }
static int feed_rows_vis(void) { return list_h() / FEED_ROW_H; }
static int art_rows_vis(void)  { return list_h() / ART_ROW_H; }
/* Reader viewport (below its header, above the bottom edge). */
static int reader_vp_h(void)   { return A.fb_h - list_y(); }

/* Keep the selected list row inside the visible window. */
static void clamp_feed_scroll(void)
{
    int vr = feed_rows_vis(); if (vr < 1) vr = 1;
    if (A.feed_sel < A.feed_top)          A.feed_top = A.feed_sel;
    if (A.feed_sel >= A.feed_top + vr)    A.feed_top = A.feed_sel - vr + 1;
    if (A.feed_top > A.db->n_feeds - vr)  A.feed_top = A.db->n_feeds - vr;
    if (A.feed_top < 0)                   A.feed_top = 0;
}
static void clamp_art_scroll(void)
{
    feed_t *f = cur_feed();
    int n = f ? f->n_items : 0;
    int vr = art_rows_vis(); if (vr < 1) vr = 1;
    if (A.art_sel < A.art_top)          A.art_top = A.art_sel;
    if (A.art_sel >= A.art_top + vr)    A.art_top = A.art_sel - vr + 1;
    if (A.art_top > n - vr)             A.art_top = n - vr;
    if (A.art_top < 0)                  A.art_top = 0;
}

/* ── Reader word-wrap ─────────────────────────────────────────────────────
 * Greedy pixel word-wrap of `body` into lines[] (each line ≤ STR_BODY).
 * Accumulate words while font_text_width(line + " " + word) fits wrap_w; on
 * overflow flush the line and start fresh with the word. A single word wider
 * than the pane is broken character-by-character so nothing is ever clipped
 * horizontally. Returns the line count. Integer-only -- text_w is the only
 * measurement, and it's the bitmap/TTF helper above (no float anywhere). */
#define WRAP_MAX 256

/* Emit `word` (length wn) into lines[], breaking it across as many lines as
 * needed so each piece fits wrap_w. Used when a single word can't fit on one
 * line. Updates *nl. */
static void wrap_break_word(const char *word, int wn, int sz, int wrap_w,
                            char lines[WRAP_MAX][STR_BODY], int *nl)
{
    int wi = 0;
    while (wi < wn && *nl < WRAP_MAX) {
        char piece[STR_BODY];
        int k = 0;
        while (wi < wn && k < STR_BODY - 1) {
            piece[k] = word[wi];
            piece[k + 1] = '\0';
            if (text_w(sz, piece) > wrap_w && k > 0) { piece[k] = '\0'; break; }
            k++; wi++;
        }
        snprintf(lines[(*nl)++], STR_BODY, "%s", piece);
    }
}

static int wrap_body(const char *body, int sz, int wrap_w,
                     char lines[WRAP_MAX][STR_BODY])
{
    int nl = 0;
    char line[STR_BODY];
    int  ll = 0;                    /* current line length in bytes          */
    line[0] = '\0';

    const char *p = body;
    while (*p && nl < WRAP_MAX) {
        if (*p == '\n') {                          /* explicit hard break    */
            snprintf(lines[nl++], STR_BODY, "%s", line);
            ll = 0; line[0] = '\0';
            p++;
            continue;
        }
        while (*p == ' ' || *p == '\t') p++;       /* skip leading spaces    */
        const char *wb = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
        int word_n = (int)(p - wb);
        if (word_n == 0) continue;                 /* trailing whitespace    */

        /* candidate = current line (+ a space when non-empty) + word */
        char cand[STR_BODY];
        int cn = 0;
        for (int i = 0; i < ll && cn < STR_BODY - 1; i++) cand[cn++] = line[i];
        if (ll > 0 && cn < STR_BODY - 1) cand[cn++] = ' ';
        for (int i = 0; i < word_n && cn < STR_BODY - 1; i++) cand[cn++] = wb[i];
        cand[cn] = '\0';

        if (text_w(sz, cand) <= wrap_w) {          /* fits -- extend line     */
            memcpy(line, cand, (size_t)cn + 1);
            ll = cn;
            continue;
        }
        /* Doesn't fit. Flush the current line if it has content. */
        if (ll > 0) {
            snprintf(lines[nl++], STR_BODY, "%s", line);
            ll = 0; line[0] = '\0';
        }
        /* Now place the word on a fresh line; break it if still too wide. */
        char wbuf[STR_BODY];
        int wn = word_n < STR_BODY - 1 ? word_n : STR_BODY - 1;
        memcpy(wbuf, wb, (size_t)wn);
        wbuf[wn] = '\0';
        if (text_w(sz, wbuf) <= wrap_w) {
            memcpy(line, wbuf, (size_t)wn + 1);
            ll = wn;
        } else {
            wrap_break_word(wbuf, wn, sz, wrap_w, lines, &nl);
        }
    }
    if (ll > 0 && nl < WRAP_MAX)
        snprintf(lines[nl++], STR_BODY, "%s", line);
    return nl;
}

/* ── Rendering ───────────────────────────────────────────────────────── */
static void render_topbar(void)
{
    surface_t *s = &A.surf;
    draw_fill_rect(s, 0, 0, A.fb_w, TOPBAR_H, FEEDS_BAR);
    draw_fill_rect(s, 0, TOPBAR_H - 1, A.fb_w, 1, FEEDS_DIV);

    int ty = (TOPBAR_H - FS_TITLE) / 2;
    text(FS_TITLE, PAD, ty, "Feeds", C_ACCENT);

    if (A.adding) {
        /* Inline URL input occupies the rest of the bar. */
        int lx = PAD + text_w(FS_TITLE, "Feeds") + 18;
        const char *lbl = "Add feed URL: ";
        text(FS_HINT, lx, (TOPBAR_H - FS_HINT) / 2 - 1, lbl, FEEDS_DIMR);
        int ix = lx + text_w(FS_HINT, lbl);
        char shown[STR_URL + 8];
        snprintf(shown, sizeof(shown), "%s", A.add_buf);
        /* tail-truncate so the caret stays visible */
        int avail = A.fb_w - ix - PAD - 8;
        const char *p = shown;
        while (*p && text_w(FS_HINT, p) > avail) p++;
        text(FS_HINT, ix, (TOPBAR_H - FS_HINT) / 2 - 1, p, C_TITLE_T);
        int cx = ix + text_w(FS_HINT, p);
        draw_fill_rect(s, cx + 1, (TOPBAR_H - FS_HINT) / 2 - 1, 2, FS_HINT + 2,
                       C_ACCENT);
        return;
    }

    /* Right-aligned status line. */
    char status[200];
    uint32_t scol = FEEDS_DIMR;
    if (A.refreshing) {
        char t[STR_TITLE];
        ellipsize(t, sizeof(t), FS_HINT, A.refresh_title, 360);
        snprintf(status, sizeof(status), "Refreshing %s\xe2\x80\xa6", t);
        scol = C_ACCENT;
    } else if (A.flash[0]) {
        snprintf(status, sizeof(status), "%s", A.flash);
        scol = C_RED;
    } else {
        int u = total_unread();
        snprintf(status, sizeof(status),
                 "%d unread \xc2\xb7 r refresh \xc2\xb7 a add \xc2\xb7 d del \xc2\xb7 q quit", u);
    }
    int sw = text_w(FS_HINT, status);
    text(FS_HINT, A.fb_w - PAD - sw, (TOPBAR_H - FS_HINT) / 2 - 1, status, scol);
}

static void pane_header(int x, int w, const char *label, int focused)
{
    surface_t *s = &A.surf;
    draw_fill_rect(s, x, PANE_Y, w, PANE_HDR_H, FEEDS_HDRBG);
    draw_fill_rect(s, x, PANE_Y + PANE_HDR_H - 1, w, 1, FEEDS_DIV);
    text(FS_HDR, x + PAD, PANE_Y + (PANE_HDR_H - FS_HDR) / 2 - 1, label,
         focused ? C_ACCENT : C_SUBTLE);
}

static void render_feeds_pane(void)
{
    surface_t *s = &A.surf;
    int x = p1_x();
    int focused = (A.focus == PANE_FEEDS);
    draw_fill_rect(s, x, PANE_Y, A.p1w, pane_h(), FEEDS_PANE);
    pane_header(x, A.p1w, "Feeds", focused);

    int ly = list_y(), vr = feed_rows_vis();
    for (int r = 0; r < vr; r++) {
        int idx = A.feed_top + r;
        if (idx >= A.db->n_feeds) break;
        feed_t *f = &A.db->feeds[idx];
        int y = ly + r * FEED_ROW_H;

        if (idx == A.feed_sel)
            draw_fill_rect(s, x, y, A.p1w, FEED_ROW_H,
                           focused ? FEEDS_SEL : FEEDS_SEL_D);

        /* "*" marks the feed currently being refreshed. */
        int tx = x + PAD;
        if (A.refreshing &&
            strcmp(f->title[0] ? f->title : f->url, A.refresh_title) == 0) {
            text(FS_FEED, tx, y + (FEED_ROW_H - FS_FEED) / 2, "*", C_ACCENT);
            tx += text_w(FS_FEED, "* ");
        }

        const char *name = f->title[0] ? f->title : f->url;
        int unread = feed_unread(f);
        char cnt[16] = "";
        int cnt_w = 0;
        if (unread > 0) {
            snprintf(cnt, sizeof(cnt), "%d", unread);
            cnt_w = text_w(FS_DATE, cnt);
        }
        int avail = (x + A.p1w - PAD) - tx - (cnt_w ? cnt_w + 10 : 0);
        char shown[STR_TITLE];
        ellipsize(shown, sizeof(shown), FS_FEED, name, avail);
        uint32_t fg = (idx == A.feed_sel) ? C_TITLE_T
                    : (unread > 0 ? C_TEXT : FEEDS_DIM);
        text(FS_FEED, tx, y + (FEED_ROW_H - FS_FEED) / 2, shown, fg);

        if (cnt_w) {
            int cx = x + A.p1w - PAD - cnt_w;
            /* small pill behind the count */
            draw_rounded_rect(s, cx - 6, y + (FEED_ROW_H - 16) / 2,
                              cnt_w + 12, 16, 8, FEEDS_DOT);
            text(FS_DATE, cx, y + (FEED_ROW_H - FS_DATE) / 2, cnt, C_TITLE_T);
        }
    }
    draw_fill_rect(s, x + A.p1w, PANE_Y, DIV_W, pane_h(), FEEDS_DIV);
}

static void render_articles_pane(void)
{
    surface_t *s = &A.surf;
    int x = p2_x();
    int focused = (A.focus == PANE_ARTS);
    draw_fill_rect(s, x, PANE_Y, A.p2w, pane_h(), FEEDS_PANE);
    pane_header(x, A.p2w, "Articles", focused);

    feed_t *f = cur_feed();
    int ly = list_y();
    if (!f) {
        text(FS_ART, x + PAD, ly + PAD, "(no feed selected)", FEEDS_DIM);
        draw_fill_rect(s, x + A.p2w, PANE_Y, DIV_W, pane_h(), FEEDS_DIV);
        return;
    }
    if (f->error[0]) {
        char shown[STR_ERR + 8];
        snprintf(shown, sizeof(shown), "! %s", f->error);
        char e2[STR_ERR + 8];
        ellipsize(e2, sizeof(e2), FS_ART, shown, A.p2w - 2 * PAD);
        text(FS_ART, x + PAD, ly + PAD, e2, C_RED);
        draw_fill_rect(s, x + A.p2w, PANE_Y, DIV_W, pane_h(), FEEDS_DIV);
        return;
    }
    if (!f->loaded) {
        text(FS_ART, x + PAD, ly + PAD, "(press r to refresh)", FEEDS_DIM);
        draw_fill_rect(s, x + A.p2w, PANE_Y, DIV_W, pane_h(), FEEDS_DIV);
        return;
    }
    if (f->n_items == 0) {
        text(FS_ART, x + PAD, ly + PAD, "(no articles)", FEEDS_DIM);
        draw_fill_rect(s, x + A.p2w, PANE_Y, DIV_W, pane_h(), FEEDS_DIV);
        return;
    }

    int vr = art_rows_vis();
    for (int r = 0; r < vr; r++) {
        int idx = A.art_top + r;
        if (idx >= f->n_items) break;
        feed_item_t *it = &f->items[idx];
        int y = ly + r * ART_ROW_H;

        if (idx == A.art_sel)
            draw_fill_rect(s, x, y, A.p2w, ART_ROW_H,
                           focused ? FEEDS_SEL : FEEDS_SEL_D);

        int tx = x + PAD;
        /* unread accent dot */
        if (!it->read) {
            draw_circle_filled(s, tx + 3, y + ART_ROW_H / 2 - 6, 3, FEEDS_DOT);
        }
        tx += 14;

        const char *title = it->title[0] ? it->title : "(untitled)";
        char shown[STR_TITLE];
        ellipsize(shown, sizeof(shown), FS_ART, title,
                  (x + A.p2w - PAD) - tx);
        uint32_t fg = (idx == A.art_sel) ? C_TITLE_T
                    : (it->read ? FEEDS_DIM : C_TEXT);
        text(FS_ART, tx, y + 6, shown, fg);

        if (it->date[0]) {
            char d2[STR_DATE];
            ellipsize(d2, sizeof(d2), FS_DATE, it->date,
                      (x + A.p2w - PAD) - tx);
            text(FS_DATE, tx, y + 6 + FS_ART + 2, d2, FEEDS_DIMR);
        }
    }

    /* thin scrollbar when the list overflows */
    if (f->n_items > vr) {
        int track_h = list_h();
        int th = track_h * vr / f->n_items;
        if (th < 24) th = 24;
        int range = track_h - th;
        int maxtop = f->n_items - vr;
        int ty = ly + (maxtop > 0 ? range * A.art_top / maxtop : 0);
        draw_fill_rect(s, x + A.p2w - SBAR_W, ly, SBAR_W, track_h, 0x000C121C);
        draw_rounded_rect(s, x + A.p2w - SBAR_W + 1, ty, SBAR_W - 2, th, 3,
                          FEEDS_DIV);
    }

    draw_fill_rect(s, x + A.p2w, PANE_Y, DIV_W, pane_h(), FEEDS_DIV);
}

/* Wrap the current item's body into lines[] and return the total reader
 * content height in pixels (title + meta + divider + body). Sets *out_nl to
 * the number of body lines. Returns 0 if no item is selected. The geometry
 * here MUST mirror render_reader_pane's vertical advances exactly. */
static int reader_content_h(char lines[WRAP_MAX][STR_BODY], int *out_nl)
{
    feed_item_t *it = cur_item();
    *out_nl = 0;
    if (!it) return 0;

    int wrap_w = p3_w() - 2 * PAD - SBAR_W;
    int y = 0;                          /* relative content y (px)            */

    /* title (FS_BIG) -- count its wrapped lines */
    static char tlines[WRAP_MAX][STR_BODY];  /* static: 16KB user stack can't hold 256KB */
    int tnl = wrap_body(it->title[0] ? it->title : "(untitled)",
                        FS_BIG, wrap_w, tlines);
    y += tnl * (FS_BIG + 6);
    y += 6;
    if (it->date[0] || it->link[0]) y += FS_DATE + 8;   /* meta line          */
    y += 10;                                            /* divider            */

    int nl = wrap_body(it->summary, FS_BODY, wrap_w, lines);
    *out_nl = nl;
    y += nl * LINE_H;
    return y + PAD;
}

static void render_reader_pane(void)
{
    surface_t *s = &A.surf;
    int x = p3_x(), w = p3_w();
    int focused = (A.focus == PANE_READER);
    draw_fill_rect(s, x, PANE_Y, w, pane_h(), FEEDS_PANE);
    pane_header(x, w, "Reader", focused);

    feed_item_t *it = cur_item();
    int ly = list_y();
    if (!it) {
        text(FS_BODY, x + PAD, ly + PAD,
             "Select an article to read it here.", FEEDS_DIM);
        A.reader_max = 0;
        return;
    }

    int wrap_w = w - 2 * PAD - SBAR_W;
    static char lines[WRAP_MAX][STR_BODY];   /* static: 16KB user stack can't hold 256KB */
    int nl = 0;
    int content_h = reader_content_h(lines, &nl);

    int vp_h = reader_vp_h();
    A.reader_max = content_h - vp_h;
    if (A.reader_max < 0) A.reader_max = 0;
    if (A.reader_scroll > A.reader_max) A.reader_scroll = A.reader_max;
    if (A.reader_scroll < 0) A.reader_scroll = 0;

    int top = ly - A.reader_scroll;     /* y of content origin on screen      */
    int clip_lo = ly, clip_hi = A.fb_h;

    /* title (FS_BIG), wrapped */
    static char tlines[WRAP_MAX][STR_BODY];  /* static: 16KB user stack can't hold 256KB */
    int tnl = wrap_body(it->title[0] ? it->title : "(untitled)",
                        FS_BIG, wrap_w, tlines);
    int y = top;
    for (int i = 0; i < tnl; i++) {
        if (y + FS_BIG >= clip_lo && y < clip_hi)
            text(FS_BIG, x + PAD, y, tlines[i], C_TITLE_T);
        y += FS_BIG + 6;
    }
    y += 6;

    /* date · link */
    if (it->date[0] || it->link[0]) {
        char meta[STR_DATE + STR_URL + 8];
        if (it->date[0] && it->link[0])
            snprintf(meta, sizeof(meta), "%s \xc2\xb7 %s", it->date, it->link);
        else
            snprintf(meta, sizeof(meta), "%s", it->date[0] ? it->date : it->link);
        char m2[256];
        ellipsize(m2, sizeof(m2), FS_DATE, meta, wrap_w);
        if (y + FS_DATE >= clip_lo && y < clip_hi)
            text(FS_DATE, x + PAD, y, m2, FEEDS_DIMR);
        y += FS_DATE + 8;
    }

    /* divider */
    if (y >= clip_lo && y < clip_hi)
        draw_fill_rect(s, x + PAD, y, wrap_w, 1, FEEDS_DIV);
    y += 10;

    /* body */
    for (int i = 0; i < nl; i++) {
        if (y + FS_BODY >= clip_lo && y < clip_hi)
            text(FS_BODY, x + PAD, y, lines[i], C_TEXT);
        y += LINE_H;
    }

    /* scrollbar / "more" hint when the body overflows */
    if (A.reader_max > 0) {
        int track_h = vp_h;
        int th = track_h * vp_h / (vp_h + A.reader_max);
        if (th < 30) th = 30;
        int range = track_h - th;
        int ty = ly + (A.reader_max > 0 ? range * A.reader_scroll / A.reader_max : 0);
        draw_fill_rect(s, x + w - SBAR_W, ly, SBAR_W, track_h, 0x000C121C);
        draw_rounded_rect(s, x + w - SBAR_W + 1, ty, SBAR_W - 2, th, 3,
                          focused ? FEEDS_SEL : FEEDS_DIV);
        if (A.reader_scroll < A.reader_max) {
            const char *more = "more \xe2\x96\xbe";   /* "more  v" */
            int mw = text_w(FS_DATE, more);
            text(FS_DATE, x + w - SBAR_W - mw - 8, A.fb_h - FS_DATE - 6,
                 more, FEEDS_DIMR);
        }
    }
}

static void render(void)
{
    if (!A.dirty) return;
    A.dirty = 0;
    surface_t *s = &A.surf;

    draw_fill_rect(s, 0, 0, A.fb_w, A.fb_h, FEEDS_BG);
    render_feeds_pane();
    render_articles_pane();
    render_reader_pane();
    render_topbar();

    lumen_window_present(A.lwin);
}

/* Render immediately (used during the blocking refresh so progress shows). */
static void render_now(void)
{
    A.dirty = 1;
    render();
}

/* ── Refresh (blocking -- fork/exec curl via feed_fetch) ──────────────── */
static void refresh_feed(int i)
{
    if (i < 0 || i >= A.db->n_feeds) return;
    feed_t *f = &A.db->feeds[i];

    A.refreshing = 1;
    snprintf(A.refresh_title, sizeof(A.refresh_title), "%s",
             f->title[0] ? f->title : f->url);
    A.flash[0] = '\0';
    render_now();                       /* show "Refreshing ..." + the "*"      */

    char *buf = NULL;
    size_t n = 0;
    char err[STR_ERR];
    err[0] = '\0';
    int rc = feed_fetch(f->url, &buf, &n, err, sizeof(err));
    if (rc == 0 && buf) {
        feed_parse(buf, n, f);
        free(buf);
        f->loaded = 1;
        f->error[0] = '\0';
    } else {
        snprintf(f->error, sizeof(f->error), "%s",
                 err[0] ? err : "fetch failed");
    }
    /* The store owns persistent read-state; re-apply it after the re-parse
     * so already-read items don't reappear as unread (unread is then derived
     * on demand via feed_unread()). */
    feed_db_load_read(A.db);

    A.refreshing = 0;
    A.refresh_title[0] = '\0';

    /* Keep the article selection sane after a re-parse. */
    if (i == A.feed_sel) {
        if (A.art_sel >= f->n_items) A.art_sel = f->n_items - 1;
        if (A.art_sel < 0) A.art_sel = 0;
        clamp_art_scroll();
    }
    render_now();
}

static void refresh_all(void)
{
    for (int i = 0; i < A.db->n_feeds && !s_term; i++)
        refresh_feed(i);
}

/* ── Article open (mark read) ────────────────────────────────────────── */
static void open_article(void)
{
    feed_item_t *it = cur_item();
    if (!it) return;
    if (!it->read && it->link[0]) {
        feed_mark_read(it->link);
        it->read = 1;
    } else if (!it->read) {
        it->read = 1;                   /* no link to persist, but mark it    */
    }
    A.reader_scroll = 0;
    A.focus = PANE_READER;
    A.dirty = 1;
}

/* Move focus to Articles for the selected feed, refreshing if needed. */
static void enter_feed(void)
{
    feed_t *f = cur_feed();
    if (!f) return;
    if (!f->loaded && !f->error[0])
        refresh_feed(A.feed_sel);
    A.art_sel = 0;
    A.art_top = 0;
    A.reader_scroll = 0;
    A.focus = PANE_ARTS;
    A.dirty = 1;
}

/* ── Add-feed mode ───────────────────────────────────────────────────── */
static void add_begin(void)
{
    A.adding = 1;
    A.add_len = 0;
    A.add_buf[0] = '\0';
    A.flash[0] = '\0';
    A.dirty = 1;
}
static void add_cancel(void)
{
    A.adding = 0;
    A.add_buf[0] = '\0';
    A.add_len = 0;
    A.dirty = 1;
}
static void add_commit(void)
{
    if (A.add_len == 0) { add_cancel(); return; }
    char url[STR_URL];
    snprintf(url, sizeof(url), "%s", A.add_buf);
    A.adding = 0;
    A.add_buf[0] = '\0';
    A.add_len = 0;

    int rc = feed_db_add(A.db, url);    /* persists on success                */
    if (rc < 0) {
        snprintf(A.flash, sizeof(A.flash), "could not add feed (full or dupe)");
        A.dirty = 1;
        return;
    }
    /* Select and refresh the newly-added feed (added last). */
    A.feed_sel = A.db->n_feeds - 1;
    clamp_feed_scroll();
    refresh_feed(A.feed_sel);
}
static void add_key(char c)
{
    if (c == KEY_ESC)            { add_cancel(); return; }
    if (c == '\r' || c == '\n')  { add_commit(); return; }
    if (c == '\b' || c == 0x7f) {
        if (A.add_len > 0) { A.add_buf[--A.add_len] = '\0'; A.dirty = 1; }
        return;
    }
    if (c >= 0x20 && c < 0x7f && A.add_len < (int)sizeof(A.add_buf) - 1) {
        A.add_buf[A.add_len++] = c;
        A.add_buf[A.add_len] = '\0';
        A.dirty = 1;
    }
}

/* ── Feed delete ─────────────────────────────────────────────────────── */
static void delete_feed(void)
{
    if (A.db->n_feeds == 0) return;
    feed_db_remove(A.db, A.feed_sel);   /* persists                           */
    if (A.feed_sel >= A.db->n_feeds) A.feed_sel = A.db->n_feeds - 1;
    if (A.feed_sel < 0) A.feed_sel = 0;
    A.art_sel = 0;
    A.art_top = 0;
    A.reader_scroll = 0;
    clamp_feed_scroll();
    A.dirty = 1;
}

/* ── Focus cycling ───────────────────────────────────────────────────── */
static void cycle_focus(int dir)
{
    A.focus = (A.focus + dir + PANE_COUNT) % PANE_COUNT;
    A.dirty = 1;
}

/* ── List / reader scrolling helpers ─────────────────────────────────── */
static void feed_move(int delta)
{
    int n = A.db->n_feeds;
    if (n == 0) return;
    A.feed_sel += delta;
    if (A.feed_sel < 0) A.feed_sel = 0;
    if (A.feed_sel >= n) A.feed_sel = n - 1;
    /* changing the feed resets the article/reader view to that feed */
    A.art_sel = 0;
    A.art_top = 0;
    A.reader_scroll = 0;
    clamp_feed_scroll();
    A.dirty = 1;
}
static void art_move(int delta)
{
    feed_t *f = cur_feed();
    if (!f || f->n_items == 0) return;
    A.art_sel += delta;
    if (A.art_sel < 0) A.art_sel = 0;
    if (A.art_sel >= f->n_items) A.art_sel = f->n_items - 1;
    A.reader_scroll = 0;
    clamp_art_scroll();
    A.dirty = 1;
}
static void reader_scroll_by(int px)
{
    A.reader_scroll += px;
    if (A.reader_scroll < 0) A.reader_scroll = 0;
    if (A.reader_scroll > A.reader_max) A.reader_scroll = A.reader_max;
    A.dirty = 1;
}

/* ── Keyboard ────────────────────────────────────────────────────────── */
static void handle_key(char c)
{
    if (A.adding) { add_key(c); return; }

    switch (c) {
    case KEY_ESC:
    case 'q': case 'Q':
        A.done = 1;
        return;

    case KEY_TAB:
        /* No reliable Shift detection from the proxy path; Tab cycles
         * forward. (Shift-Tab folds to the same byte here.) */
        cycle_focus(+1);
        return;

    case 'a':
        add_begin();
        return;

    case 'd':
        delete_feed();          /* deletes the selected subscription          */
        return;

    case 'r':
        refresh_feed(A.feed_sel);
        return;
    case 'R':
        refresh_all();
        return;

    case KEY_UP:   case 'k':
        if (A.focus == PANE_FEEDS)       feed_move(-1);
        else if (A.focus == PANE_ARTS)   art_move(-1);
        else                             reader_scroll_by(-LINE_H);
        return;
    case KEY_DOWN: case 'j':
        if (A.focus == PANE_FEEDS)       feed_move(+1);
        else if (A.focus == PANE_ARTS)   art_move(+1);
        else                             reader_scroll_by(+LINE_H);
        return;

    case KEY_LEFT:
        cycle_focus(-1);
        return;
    case KEY_RIGHT:
        cycle_focus(+1);
        return;

    case '\r': case '\n':
        if (A.focus == PANE_FEEDS)      enter_feed();
        else if (A.focus == PANE_ARTS)  open_article();
        return;

    /* PageUp/PageDown are E0 scancodes the PS/2 path doesn't deliver yet
     * (Phase 48); the mouse wheel covers paging the reader/lists today. */
    default:
        break;
    }
}

/* ── Mouse ───────────────────────────────────────────────────────────── */
static void handle_mouse(const lumen_event_t *ev)
{
    int x = ev->mouse.x, y = ev->mouse.y;

    if (ev->mouse.evtype == LUMEN_MOUSE_WHEEL) {
        int dir = ev->mouse.scroll > 0 ? -1 : +1;   /* +up scrolls toward top */
        if (x >= p3_x()) {
            A.focus = PANE_READER;
            reader_scroll_by(dir * LINE_H * 3);
        } else if (x >= p2_x()) {
            A.focus = PANE_ARTS;
            art_move(dir);
        } else {
            A.focus = PANE_FEEDS;
            feed_move(dir);
        }
        return;
    }

    if (ev->mouse.evtype != LUMEN_MOUSE_DOWN) return;
    if (!(ev->mouse.buttons & 1)) return;
    if (y < list_y()) return;                       /* top bar / headers      */

    if (x < p2_x()) {
        /* Feeds pane: select + load that feed, focus Articles. */
        int row = (y - list_y()) / FEED_ROW_H;
        int idx = A.feed_top + row;
        if (idx >= 0 && idx < A.db->n_feeds) {
            A.feed_sel = idx;
            clamp_feed_scroll();
            enter_feed();
        } else {
            A.focus = PANE_FEEDS;
            A.dirty = 1;
        }
    } else if (x < p3_x()) {
        /* Articles pane: open the clicked article (marks read). */
        feed_t *f = cur_feed();
        if (f && f->loaded) {
            int row = (y - list_y()) / ART_ROW_H;
            int idx = A.art_top + row;
            if (idx >= 0 && idx < f->n_items) {
                A.art_sel = idx;
                clamp_art_scroll();
                open_article();
            } else {
                A.focus = PANE_ARTS;
                A.dirty = 1;
            }
        } else {
            A.focus = PANE_ARTS;
            A.dirty = 1;
        }
    } else {
        A.focus = PANE_READER;
        A.dirty = 1;
    }
}

/* ── Main ────────────────────────────────────────────────────────────── */
int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    A.db = malloc(sizeof(feed_db_t));
    if (!A.db) {
        dprintf(2, "[feeds] out of memory (db alloc)\n");
        return 1;
    }
    memset(A.db, 0, sizeof(*A.db));
    feed_db_load(A.db);                 /* seeds defaults if no $HOME/.feeds   */

    /* Connect to Lumen (retry only on ECONNREFUSED, like the other apps). */
    A.lfd = lumen_connect_retry();
    if (A.lfd < 0) {
        dprintf(2, "[feeds] lumen_connect failed (%d)\n", A.lfd);
        free(A.db);
        return 1;
    }

    /* Clamp the window to the framebuffer if it's small. */
    int win_w = WIN_W, win_h = WIN_H;
    {
        const char *fw = getenv("LUMEN_FB_W");
        const char *fh = getenv("LUMEN_FB_H");
        if (fw && atoi(fw) > 0 && win_w > atoi(fw) - 32) win_w = atoi(fw) - 32;
        if (fh && atoi(fh) > 0 && win_h > atoi(fh) - 64) win_h = atoi(fh) - 64;
        if (win_w < 640) win_w = 640;
        if (win_h < 400) win_h = 400;
    }

    A.lwin = lumen_window_create(A.lfd, "Feeds", win_w, win_h);
    if (!A.lwin) {
        dprintf(2, "[feeds] lumen_window_create failed\n");
        close(A.lfd);
        free(A.db);
        return 1;
    }
    A.fb_w = A.lwin->w;
    A.fb_h = A.lwin->h;
    A.surf = (surface_t){
        .buf = (uint32_t *)A.lwin->backbuf,
        .w = A.fb_w, .h = A.fb_h, .pitch = A.lwin->stride,
    };
    layout_panes();                     /* size the three panes to fb_w        */

    font_init();

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigterm_handler;
    sigaction(SIGTERM, &sa, NULL);

    A.focus    = PANE_FEEDS;
    A.feed_sel = 0;
    A.art_sel  = 0;

    /* First paint, then auto-refresh everything so there's content to read. */
    A.dirty = 1;
    render();
    dprintf(2, "[feeds] connected %dx%d\n", A.lwin->w, A.lwin->h);

    refresh_all();

    /* Land on the first feed's articles if it loaded. */
    if (A.db->n_feeds > 0 && A.db->feeds[0].loaded &&
        A.db->feeds[0].n_items > 0) {
        A.focus = PANE_ARTS;
    }
    A.dirty = 1;
    render();

    /* ── Event loop ──────────────────────────────────────────────────────
     * Lumen folds arrow keys to the synthetic single bytes 0xF1-0xF4 for
     * proxy windows and passes bare Esc through as one 0x1B byte (see the
     * GUI architecture notes); raw VT escape sequences don't reach us, so a
     * single-byte keymap is sufficient -- same as 2048 / filemanager. */
    while (!s_term && !A.done) {
        lumen_event_t ev;
        int r = lumen_wait_event(A.lfd, &ev, 50);
        if (r < 0) break;
        if (r == 1) {
            if (ev.type == LUMEN_EV_CLOSE_REQUEST) break;
            if (ev.type == LUMEN_EV_KEY && ev.key.pressed)
                handle_key((char)ev.key.keycode);
            if (ev.type == LUMEN_EV_MOUSE)
                handle_mouse(&ev);
        }
        render();
    }

    lumen_window_destroy(A.lwin);
    close(A.lfd);
    free(A.db);
    dprintf(2, "[feeds] exit\n");
    return 0;
}
