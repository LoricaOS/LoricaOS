/* feeds_store.c — subscriptions + read-state persistence for the Aegis
 * "Feeds" RSS reader. USERSPACE (musl libc).
 *
 * On-disk formats:
 *   $HOME/.feeds       one subscription per line: "<url>\t<title>\n"
 *                      ('#'-prefixed and blank lines ignored; title optional)
 *   $HOME/.feeds-read  one read-article LINK per line: "<link>\n"
 *
 * Atomic save: feed_db_save writes to "$HOME/.feeds.tmp" then rename()s it
 * over "$HOME/.feeds" — a crash mid-write can never leave a truncated/empty
 * subscriptions file. If the temp open or write fails it is unlinked and we
 * report an error (the old file is left intact).
 *
 * Read-state dedup: feed_mark_read just APPENDs the link (no dedup); duplicate
 * lines are harmless because feed_db_load_read applies them idempotently
 * (it only ever SETS read=1). A cap (READLINKS_MAX) bounds how many lines we
 * read back so a pathological file can't make load loop unbounded.
 *
 * Safety: every path is built with a length-guarded snprintf; every file read
 * uses a bounded line buffer; open() failures are treated as "no file"; a
 * malformed .feeds line is skipped, never fatal. No float.
 */
#include "feeds.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

/* Max read-state links we will process from $HOME/.feeds-read in one load.
 * Bounds work, not correctness — extra links beyond this are simply ignored. */
#define READLINKS_MAX 2000

/* Longest single line we will read from either file before truncating it.
 * Big enough to hold a url + tab + title comfortably. */
#define FEEDS_LINE_MAX (STR_URL + STR_TITLE + 16)

/* ── path helpers ──────────────────────────────────────────────────────── */

/* Resolve $HOME (fall back to "/root" when unset/empty). */
static const char *home_dir(void)
{
    const char *h = getenv("HOME");
    if (!h || h[0] == '\0')
        return "/root";
    return h;
}

/* Build "$HOME/<name>" into out[outsz]; returns 0 on success, <0 if it would
 * overflow (snprintf truncation). */
static int build_path(char *out, size_t outsz, const char *name)
{
    int n = snprintf(out, outsz, "%s/%s", home_dir(), name);
    if (n < 0 || (size_t)n >= outsz)
        return -1;
    return 0;
}

/* ── small string helpers ──────────────────────────────────────────────── */

/* True if s starts with the http(s):// scheme — our "looks like a URL" test. */
static int is_url(const char *s)
{
    return s && (strncmp(s, "http://", 7) == 0 ||
                 strncmp(s, "https://", 8) == 0);
}

/* Trim trailing \r\n and surrounding ASCII spaces/tabs in place. */
static void trim(char *s)
{
    size_t len = strlen(s);
    /* strip trailing CR/LF and whitespace */
    while (len > 0) {
        char c = s[len - 1];
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t')
            s[--len] = '\0';
        else
            break;
    }
    /* strip leading whitespace by shifting */
    size_t start = 0;
    while (s[start] == ' ' || s[start] == '\t')
        start++;
    if (start > 0)
        memmove(s, s + start, strlen(s + start) + 1);
}

/* Bounded copy: copy src into dst[dstsz], always NUL-terminated, truncating. */
static void copy_bounded(char *dst, size_t dstsz, const char *src)
{
    if (dstsz == 0)
        return;
    size_t i = 0;
    for (; src[i] != '\0' && i < dstsz - 1; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

/* ── line reader (no stdio FILE buffering surprises; bounded) ───────────── */

/* Read one line (up to bufsz-1 bytes) from fd into buf, NUL-terminated,
 * INCLUDING the trailing '\n' if present. Returns the number of bytes stored
 * (0 only at clean EOF with nothing read), or -1 on read error. If a line is
 * longer than the buffer it is truncated and the remainder of that line is
 * consumed (so the next call starts on the following line). */
static int read_line(int fd, char *buf, size_t bufsz)
{
    if (bufsz == 0)
        return 0;
    size_t i = 0;
    char c;
    int got = 0;
    for (;;) {
        ssize_t r = read(fd, &c, 1);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (r == 0)
            break;              /* EOF */
        got = 1;
        if (i < bufsz - 1)
            buf[i++] = c;
        /* else: drop overflow byte, keep consuming until newline */
        if (c == '\n')
            break;
    }
    buf[i] = '\0';
    return got ? (int)i : 0;
}

/* ── seed defaults ─────────────────────────────────────────────────────── */

struct seed { const char *url; const char *title; };

static const struct seed DEFAULT_FEEDS[] = {
    { "https://herald.byexec.com/feed.xml", "Aegis News"  },
    { "https://hnrss.org/frontpage",        "Hacker News" },
    { "https://lwn.net/headlines/rss",      "LWN"         },
};
#define DEFAULT_FEEDS_N ((int)(sizeof(DEFAULT_FEEDS) / sizeof(DEFAULT_FEEDS[0])))

/* Reset one feed slot to a clean loaded=0 state. */
static void feed_init(feed_t *f, const char *url, const char *title)
{
    memset(f, 0, sizeof(*f));
    copy_bounded(f->url, sizeof(f->url), url ? url : "");
    copy_bounded(f->title, sizeof(f->title), title ? title : "");
    f->n_items = 0;
    f->loaded = 0;
    f->error[0] = '\0';
}

/* ── public API ────────────────────────────────────────────────────────── */

int feed_db_load(feed_db_t *db)
{
    if (!db)
        return -1;

    memset(db, 0, sizeof(*db));
    db->n_feeds = 0;

    char path[STR_URL + 32];
    if (build_path(path, sizeof(path), ".feeds") < 0)
        return -1;

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        if (errno == ENOENT) {
            /* First run: seed defaults and persist them. */
            for (int i = 0; i < DEFAULT_FEEDS_N && db->n_feeds < FEEDS_MAX; i++) {
                feed_init(&db->feeds[db->n_feeds],
                          DEFAULT_FEEDS[i].url, DEFAULT_FEEDS[i].title);
                db->n_feeds++;
            }
            (void)feed_db_save(db);   /* best effort; missing-file isn't fatal */
            return 0;
        }
        /* Any other open error: real failure. */
        return -1;
    }

    char line[FEEDS_LINE_MAX];
    for (;;) {
        int n = read_line(fd, line, sizeof(line));
        if (n < 0) {            /* read error */
            close(fd);
            return -1;
        }
        if (n == 0)             /* EOF */
            break;
        if (db->n_feeds >= FEEDS_MAX)
            break;

        trim(line);
        if (line[0] == '\0' || line[0] == '#')
            continue;           /* blank or comment */

        /* Split on the FIRST tab into url + title. */
        char *tab = strchr(line, '\t');
        char *url, *title;
        if (tab) {
            *tab = '\0';
            url = line;
            title = tab + 1;
        } else {
            url = line;
            title = "";
        }

        /* Trim each half (tab-adjacent spaces, stray CR). */
        trim(url);
        /* title may legitimately contain interior spaces; only strip edges */
        {
            /* reuse trim on a mutable buffer */
            char tbuf[STR_TITLE];
            copy_bounded(tbuf, sizeof(tbuf), title);
            trim(tbuf);
            /* validate url before committing the slot */
            if (!is_url(url))
                continue;       /* skip non-URL lines */

            feed_init(&db->feeds[db->n_feeds], url, tbuf);
            db->n_feeds++;
        }
    }

    close(fd);
    return 0;
}

int feed_db_save(const feed_db_t *db)
{
    if (!db)
        return -1;

    char path[STR_URL + 32];
    char tmp[STR_URL + 32];
    if (build_path(path, sizeof(path), ".feeds") < 0)
        return -1;
    if (build_path(tmp, sizeof(tmp), ".feeds.tmp") < 0)
        return -1;

    int fd = open(tmp, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0)
        return -1;

    int rc = 0;
    for (int i = 0; i < db->n_feeds && i < FEEDS_MAX; i++) {
        const feed_t *f = &db->feeds[i];
        if (f->url[0] == '\0')
            continue;           /* skip empty-url slots */

        char rec[STR_URL + STR_TITLE + 4];
        int m = snprintf(rec, sizeof(rec), "%s\t%s\n", f->url, f->title);
        if (m < 0) {
            rc = -1;
            break;
        }
        if ((size_t)m >= sizeof(rec))
            m = (int)sizeof(rec) - 1;   /* truncated, but write what we have */

        /* bounded write loop (handles short writes / EINTR) */
        int off = 0;
        while (off < m) {
            ssize_t w = write(fd, rec + off, (size_t)(m - off));
            if (w < 0) {
                if (errno == EINTR)
                    continue;
                rc = -1;
                break;
            }
            off += (int)w;
        }
        if (rc < 0)
            break;
    }

    if (close(fd) < 0)
        rc = -1;

    if (rc < 0) {
        unlink(tmp);            /* leave the old .feeds intact */
        return -1;
    }

    /* Atomic replace. */
    if (rename(tmp, path) < 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

int feed_db_add(feed_db_t *db, const char *url)
{
    if (!db || !url)
        return -1;
    if (!is_url(url))
        return -1;                      /* empty or not http(s):// */
    if (strlen(url) >= STR_URL)
        return -1;                      /* won't fit */
    if (db->n_feeds >= FEEDS_MAX)
        return -1;                      /* full */

    /* duplicate check */
    for (int i = 0; i < db->n_feeds; i++) {
        if (strcmp(db->feeds[i].url, url) == 0)
            return -1;                  /* already subscribed */
    }

    feed_init(&db->feeds[db->n_feeds], url, "");
    db->n_feeds++;

    return feed_db_save(db);
}

int feed_db_remove(feed_db_t *db, int idx)
{
    if (!db)
        return -1;
    if (idx < 0 || idx >= db->n_feeds)
        return -1;                      /* bad index */

    int tail = db->n_feeds - 1 - idx;   /* slots after idx */
    if (tail > 0) {
        memmove(&db->feeds[idx], &db->feeds[idx + 1],
                (size_t)tail * sizeof(feed_t));
    }
    db->n_feeds--;
    /* clear the now-unused tail slot to avoid stale data lingering */
    memset(&db->feeds[db->n_feeds], 0, sizeof(feed_t));

    return feed_db_save(db);
}

/* ── read-state ────────────────────────────────────────────────────────── */

void feed_db_load_read(feed_db_t *db)
{
    if (!db)
        return;

    char path[STR_URL + 32];
    if (build_path(path, sizeof(path), ".feeds-read") < 0)
        return;

    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return;                         /* missing file => no-op */

    char line[STR_URL + 4];
    int seen = 0;
    for (;;) {
        if (seen >= READLINKS_MAX)
            break;
        int n = read_line(fd, line, sizeof(line));
        if (n <= 0)
            break;                      /* EOF or error => stop */
        seen++;

        trim(line);
        if (line[0] == '\0')
            continue;

        /* Mark every matching item across every feed as read. */
        for (int fi = 0; fi < db->n_feeds; fi++) {
            feed_t *f = &db->feeds[fi];
            for (int ii = 0; ii < f->n_items && ii < ITEMS_MAX; ii++) {
                if (strcmp(f->items[ii].link, line) == 0)
                    f->items[ii].read = 1;
            }
        }
    }

    close(fd);
}

void feed_mark_read(const char *link)
{
    if (!link || link[0] == '\0')
        return;                         /* nothing to record */

    char path[STR_URL + 32];
    if (build_path(path, sizeof(path), ".feeds-read") < 0)
        return;

    int fd = open(path, O_CREAT | O_APPEND | O_WRONLY, 0644);
    if (fd < 0)
        return;                         /* best-effort; failure is non-fatal */

    /* Build "<link>\n" in a bounded buffer (truncate an absurd link rather
     * than overflow). */
    char rec[STR_URL + 2];
    int m = snprintf(rec, sizeof(rec), "%s\n", link);
    if (m < 0) {
        close(fd);
        return;
    }
    if ((size_t)m >= sizeof(rec))
        m = (int)sizeof(rec) - 1;

    int off = 0;
    while (off < m) {
        ssize_t w = write(fd, rec + off, (size_t)(m - off));
        if (w < 0) {
            if (errno == EINTR)
                continue;
            break;                      /* give up; non-fatal */
        }
        off += (int)w;
    }

    close(fd);
}
