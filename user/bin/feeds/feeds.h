/* feeds.h — shared data model + module contract for the Aegis "Feeds" reader.
 *
 * ARCHITECTURE (one app, five modules, each a separate .c built in parallel):
 *   feeds_net.c    network fetch (fork/exec curl) ......... feed_fetch()
 *   feeds_parse.c  RSS 2.0 + Atom parsing ................. feed_parse()
 *   feeds_store.c  subscriptions + read-state persistence . feed_db_*()
 *   feeds_text.c   HTML -> plain text + entity decode ..... html_to_text()
 *   main.c         Lumen three-pane GUI (integrator)
 *
 * Every module includes ONLY this header for the shared types + the other
 * modules' prototypes. Do not add cross-module includes; code to these
 * signatures exactly. All strings are fixed-size, NUL-terminated buffers —
 * no dynamic strings inside the model (only feed_fetch returns malloc'd bytes).
 */
#ifndef FEEDS_H
#define FEEDS_H

#include <stddef.h>

/* ── sizing (kept modest so the whole db is ~0.6 MB, malloc'd once) ─────── */
#define FEEDS_MAX      16     /* max subscriptions                          */
#define ITEMS_MAX      24     /* max items kept per feed (newest first)     */
#define STR_TITLE     256
#define STR_URL       384
#define STR_DATE       48
#define STR_BODY     1024     /* article summary/body, post HTML->text      */
#define STR_ERR       128

/* One article/entry. */
typedef struct {
    char title[STR_TITLE];
    char link[STR_URL];
    char date[STR_DATE];      /* the date string as found (RFC822 or ISO8601)*/
    char summary[STR_BODY];   /* plain text (HTML already stripped)          */
    int  read;                /* 0 = unread, 1 = read                        */
} feed_item_t;

/* One subscribed feed and its current items. */
typedef struct {
    char        url[STR_URL];     /* subscription URL (persisted)            */
    char        title[STR_TITLE]; /* channel title (from feed; falls back to url) */
    feed_item_t items[ITEMS_MAX];
    int         n_items;
    int         loaded;           /* 1 once successfully fetched+parsed       */
    char        error[STR_ERR];   /* last fetch/parse error ("" = ok)        */
} feed_t;

/* The whole reader state (malloc this once; ~0.6 MB). */
typedef struct {
    feed_t feeds[FEEDS_MAX];
    int    n_feeds;
} feed_db_t;

/* ── feeds_net.c ────────────────────────────────────────────────────────
 * Fetch url into a freshly malloc'd, NUL-terminated buffer (caller free()s).
 * Implementation: fork/exec /bin/curl to a temp file then read it back (the
 * same trust/lib pattern as user/bin/herald/net.c — curl carries the
 * NET_SOCKET capability, this app does not). On error returns <0 and writes a
 * short reason into errbuf (may be NULL). out and len are set on success only.
 * Returns 0 on success, <0 on error. */
int feed_fetch(const char *url, char **out, size_t *len,
               char *errbuf, size_t errsz);

/* ── feeds_parse.c ──────────────────────────────────────────────────────
 * Detect and parse RSS 2.0 (<rss><channel><item>) or Atom
 * (<feed><entry>) from buf[len]. Fills f->title and up to ITEMS_MAX items
 * (newest-first as they appear), setting f->n_items. Per item: title, link,
 * date, and summary (RSS description / content:encoded, or Atom summary /
 * content). The parser MUST run summary/title text through html_to_text()
 * (feeds_text.c) so the model holds clean plain text. Does NOT touch
 * f->items[].read (the store/GUI owns read-state). Returns item count (>=0)
 * on success, <0 on a hard parse error (e.g. not XML / no recognizable feed).*/
int feed_parse(const char *buf, size_t len, feed_t *f);

/* ── feeds_text.c ───────────────────────────────────────────────────────
 * Convert an HTML/XML text fragment to plain text in out[outsz]: strip tags,
 * decode the common named + numeric entities (&amp; &lt; &gt; &quot; &#39;
 * &#NNN; &nbsp; &mdash; &hellip; ...), collapse runs of whitespace to single
 * spaces, and NUL-terminate (truncate to fit). Safe on already-plain text.
 * Used by the parser (item summaries/titles) and may be reused by the GUI. */
void html_to_text(const char *in, size_t in_len, char *out, size_t outsz);

/* ── feeds_store.c ──────────────────────────────────────────────────────
 * Subscription persistence. The on-disk format is one line per feed:
 *     <url>\t<title>\n
 * (title optional). Path: $HOME/.feeds, falling back to /root/.feeds when
 * HOME is unset. feed_db_load clears db then loads subscriptions (url+title
 * only; items are fetched at runtime). If the file is missing, load seeds a
 * couple of sensible default subscriptions and returns 0. */
int feed_db_load(feed_db_t *db);
int feed_db_save(const feed_db_t *db);
int feed_db_add(feed_db_t *db, const char *url);   /* 0 ok, <0 full/dupe/bad */
int feed_db_remove(feed_db_t *db, int idx);        /* 0 ok, <0 bad index     */

/* Read-state cache: persists which item LINKS have been read, across runs, in
 * $HOME/.feeds-read (one link per line). feed_db_load_read marks matching
 * items in the db as read; feed_mark_read records a link as read and persists.
 * Call feed_db_load_read after a feed's items are (re)parsed. */
void feed_db_load_read(feed_db_t *db);
void feed_mark_read(const char *link);

#endif /* FEEDS_H */
