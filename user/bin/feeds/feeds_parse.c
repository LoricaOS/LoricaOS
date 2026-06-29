/* feeds_parse.c — hand-written RSS 2.0 / RSS 1.0(RDF) / Atom parser.
 *
 * This module eats arbitrary bytes off the internet. It MUST never crash,
 * overrun a buffer, or loop forever. The whole design is built around three
 * invariants:
 *
 *   1. Every pointer stays inside [buf, end). No helper ever dereferences past
 *      `end`; every loop condition tests the bound BEFORE the byte.
 *   2. Every copy into a fixed field is bounded by that field's size and is
 *      explicitly NUL-terminated.
 *   3. Every scan loop advances by at least one byte on every iteration (or
 *      breaks), and the item loop both advances and is hard-capped at
 *      ITEMS_MAX, so no input can make the parser spin forever.
 *
 * There is no regex and no XML library here (none exist in this environment).
 * Tags are matched by their LOCAL name — the part after any ':' namespace
 * prefix — so "<atom:title>", "<title>" and "<dc:date>" all match "title" /
 * "date". CDATA, attributes and self-closing tags are handled explicitly.
 *
 * No global mutable state: the parser is fully reentrant.
 */
#include "feeds.h"

/* ── tiny byte helpers (the input is NOT NUL-terminated; never use libc str*) */

static int p_is_space(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
           c == '\v';
}

static unsigned char p_lower(unsigned char c)
{
    if (c >= 'A' && c <= 'Z')
        return (unsigned char)(c - 'A' + 'a');
    return c;
}

/* Length of a NUL-terminated literal (compile-time-ish; bounded by use). */
static size_t p_litlen(const char *s)
{
    size_t n = 0;
    while (s[n] != '\0')
        n++;
    return n;
}

/* Case-insensitive compare of [p, p+n) against the literal s.
 * True only if exactly n bytes match AND s is exactly n bytes long. */
static int p_ieq_n(const char *p, size_t n, const char *s)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (s[i] == '\0')
            return 0;
        if (p_lower((unsigned char)p[i]) != p_lower((unsigned char)s[i]))
            return 0;
    }
    return s[n] == '\0';
}

/* True if a NUL-terminated string `z` equals literal `s`, case-insensitive. */
static int p_ieq_z(const char *z, const char *s)
{
    size_t i = 0;
    for (;;) {
        unsigned char a = p_lower((unsigned char)z[i]);
        unsigned char b = p_lower((unsigned char)s[i]);
        if (a != b)
            return 0;
        if (a == '\0')
            return 1;
        i++;
    }
}

/* Does [p, end) begin with literal s (case-insensitive)? Bounded by end. */
static int p_starts_with(const char *p, const char *end, const char *s)
{
    size_t i = 0;
    while (s[i] != '\0') {
        if (p + i >= end)
            return 0;
        if (p_lower((unsigned char)p[i]) != p_lower((unsigned char)s[i]))
            return 0;
        i++;
    }
    return 1;
}

/* Bounded copy of a NUL-terminated source into a fixed dest. */
static void p_copy_z(char *dst, size_t dstsz, const char *src)
{
    size_t i = 0;
    if (dstsz == 0)
        return;
    while (src[i] != '\0' && i + 1 < dstsz) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

/* ── tag-name local span ────────────────────────────────────────────────── *
 * `p` points at the first byte of a tag name (just past '<' or "</"). Compute
 * the LOCAL name (after any ':') as (*local, *local_len) and return a pointer
 * to the first byte AFTER the whole name (whitespace/'>'/'/'/end). Bounded.
 */
static const char *tag_name_span(const char *p, const char *end,
                                 const char **local, size_t *local_len)
{
    const char *name = p;
    const char *colon = NULL;
    const char *q = p;
    while (q < end) {
        unsigned char c = (unsigned char)*q;
        if (p_is_space(c) || c == '>' || c == '/' || c == '?')
            break;
        if (c == ':')
            colon = q;
        q++;
    }
    if (colon && colon + 1 <= q) {
        *local = colon + 1;
        *local_len = (size_t)(q - (colon + 1));
    } else {
        *local = name;
        *local_len = (size_t)(q - name);
    }
    return q;
}

/* Find the closing '>' of a tag whose body starts at `after` (just past the
 * name). Tracks quotes so a '>' inside an attribute value doesn't end it
 * early. Returns a pointer to the '>' or to `end` if unterminated. Sets
 * *self_closing if the tag ended in "/>". */
static const char *tag_find_gt(const char *after, const char *end,
                               int *self_closing)
{
    const char *q = after;
    char quote = 0;
    while (q < end) {
        char c = *q;
        if (quote) {
            if (c == quote)
                quote = 0;
        } else if (c == '"' || c == '\'') {
            quote = c;
        } else if (c == '>') {
            break;
        }
        q++;
    }
    if (self_closing)
        *self_closing = (q > after && q < end && q[-1] == '/') ? 1 : 0;
    return q; /* points at '>' or end */
}

/* ── find next opening tag with a given local name ──────────────────────── *
 * Searches [p, end). Skips comments, CDATA, processing instructions, DOCTYPE
 * and closing tags so a '<' inside any of those can't confuse the match.
 * Returns a pointer to the '<' of the matching open tag, or NULL.
 * On success: *tag_end = byte just past the tag's '>'; *self_closing set.
 */
static const char *find_open_tag(const char *p, const char *end,
                                 const char *name,
                                 const char **tag_end, int *self_closing)
{
    while (p < end) {
        if (*p != '<') {
            p++;
            continue;
        }
        /* p is at '<' — classify what follows. */
        if (p_starts_with(p, end, "<!--")) {
            const char *q = p + 4;
            while (q + 3 <= end &&
                   !(q[0] == '-' && q[1] == '-' && q[2] == '>'))
                q++;
            p = (q + 3 <= end) ? q + 3 : end;
            continue;
        }
        if (p_starts_with(p, end, "<![CDATA[")) {
            const char *q = p + 9;
            while (q + 3 <= end &&
                   !(q[0] == ']' && q[1] == ']' && q[2] == '>'))
                q++;
            p = (q + 3 <= end) ? q + 3 : end;
            continue;
        }
        if (p + 2 <= end && p[1] == '!') {
            /* DOCTYPE / other declaration: skip to '>' */
            const char *q = p + 2;
            while (q < end && *q != '>')
                q++;
            p = (q < end) ? q + 1 : end;
            continue;
        }
        if (p + 2 <= end && p[1] == '?') {
            const char *q = p + 2;
            while (q + 2 <= end && !(q[0] == '?' && q[1] == '>'))
                q++;
            p = (q + 2 <= end) ? q + 2 : end;
            continue;
        }
        if (p + 2 <= end && p[1] == '/') {
            /* closing tag — skip the whole </...> */
            const char *q = p + 2;
            while (q < end && *q != '>')
                q++;
            p = (q < end) ? q + 1 : end;
            continue;
        }
        /* opening (or self-closing) tag: match its local name */
        {
            const char *local;
            size_t llen;
            const char *after = tag_name_span(p + 1, end, &local, &llen);
            int selfc = 0;
            const char *gt = tag_find_gt(after, end, &selfc);
            if (p_ieq_n(local, llen, name)) {
                if (tag_end)
                    *tag_end = (gt < end) ? gt + 1 : end;
                if (self_closing)
                    *self_closing = selfc;
                return p;
            }
            p = (gt < end) ? gt + 1 : end; /* not ours — skip past '>' */
            continue;
        }
    }
    return NULL;
}

/* Find the matching closing tag </name> (local-name match) in [p, end).
 * Skips comments/CDATA so a literal "</item>" inside CDATA is ignored.
 * Returns a pointer to the '<' of the close tag, or NULL. */
static const char *find_close_tag(const char *p, const char *end,
                                  const char *name)
{
    while (p < end) {
        if (*p != '<') {
            p++;
            continue;
        }
        if (p_starts_with(p, end, "<![CDATA[")) {
            const char *q = p + 9;
            while (q + 3 <= end &&
                   !(q[0] == ']' && q[1] == ']' && q[2] == '>'))
                q++;
            p = (q + 3 <= end) ? q + 3 : end;
            continue;
        }
        if (p_starts_with(p, end, "<!--")) {
            const char *q = p + 4;
            while (q + 3 <= end &&
                   !(q[0] == '-' && q[1] == '-' && q[2] == '>'))
                q++;
            p = (q + 3 <= end) ? q + 3 : end;
            continue;
        }
        if (p + 2 <= end && p[1] == '/') {
            const char *local;
            size_t llen;
            const char *after = tag_name_span(p + 2, end, &local, &llen);
            if (p_ieq_n(local, llen, name))
                return p;
            while (after < end && *after != '>')
                after++;
            p = (after < end) ? after + 1 : end;
            continue;
        }
        p++; /* some other '<...' — advance one byte */
    }
    return NULL;
}

/* ── decode helpers ─────────────────────────────────────────────────────── */

/* Trim [v, v+vlen), decode the handful of entities that appear in URLs/dates
 * (&amp; &apos; &quot; &lt; &gt;), and copy into out[outsz], NUL-terminated.
 * Used for link/date fields (NOT run through html_to_text). */
static void decode_amp_into(const char *v, size_t vlen, char *out, size_t outsz)
{
    const char *s = v;
    const char *e = v + vlen;
    size_t o = 0;
    const char *p;

    if (outsz == 0)
        return;
    while (s < e && p_is_space((unsigned char)*s))
        s++;
    while (e > s && p_is_space((unsigned char)e[-1]))
        e--;

    p = s;
    while (p < e && o + 1 < outsz) {
        if (*p == '&') {
            if (p + 5 <= e && p[1] == 'a' && p[2] == 'm' && p[3] == 'p' &&
                p[4] == ';') {
                out[o++] = '&';
                p += 5;
                continue;
            }
            if (p + 6 <= e && p[1] == 'a' && p[2] == 'p' && p[3] == 'o' &&
                p[4] == 's' && p[5] == ';') {
                out[o++] = '\'';
                p += 6;
                continue;
            }
            if (p + 6 <= e && p[1] == 'q' && p[2] == 'u' && p[3] == 'o' &&
                p[4] == 't' && p[5] == ';') {
                out[o++] = '"';
                p += 6;
                continue;
            }
            if (p + 4 <= e && p[1] == 'l' && p[2] == 't' && p[3] == ';') {
                out[o++] = '<';
                p += 4;
                continue;
            }
            if (p + 4 <= e && p[1] == 'g' && p[2] == 't' && p[3] == ';') {
                out[o++] = '>';
                p += 4;
                continue;
            }
        }
        out[o++] = *p++;
    }
    out[o] = '\0';
}

/* ── attribute value extraction ─────────────────────────────────────────── *
 * In a tag's text [tag_start, tag_end) (tag_start at '<', tag_end just past
 * '>'), find attribute `name` (case-insensitive) and copy its value into
 * out[outsz] (trimmed, &amp;-decoded, NUL-terminated). Returns 1 if found.
 */
static int attr_value(const char *tag_start, const char *tag_end,
                      const char *name, char *out, size_t outsz)
{
    const char *p = tag_start;
    size_t nlen = p_litlen(name);

    if (outsz == 0)
        return 0;
    out[0] = '\0';

    /* skip '<' and the element name itself */
    if (p < tag_end && *p == '<')
        p++;
    {
        const char *local;
        size_t llen;
        p = tag_name_span(p, tag_end, &local, &llen);
    }

    while (p < tag_end) {
        const char *aname;
        size_t anlen;
        const char *vstart;
        const char *vend;

        while (p < tag_end && (p_is_space((unsigned char)*p) || *p == '/'))
            p++;
        if (p >= tag_end || *p == '>')
            break;

        aname = p;
        while (p < tag_end && !p_is_space((unsigned char)*p) && *p != '=' &&
               *p != '>' && *p != '/')
            p++;
        anlen = (size_t)(p - aname);

        while (p < tag_end && p_is_space((unsigned char)*p))
            p++;

        if (p < tag_end && *p == '=') {
            p++;
            while (p < tag_end && p_is_space((unsigned char)*p))
                p++;
            if (p < tag_end && (*p == '"' || *p == '\'')) {
                char q = *p;
                p++;
                vstart = p;
                while (p < tag_end && *p != q)
                    p++;
                vend = p;
                if (p < tag_end)
                    p++; /* skip closing quote */
            } else {
                vstart = p;
                while (p < tag_end && !p_is_space((unsigned char)*p) &&
                       *p != '>' && *p != '/')
                    p++;
                vend = p;
            }
            if (anlen == nlen && p_ieq_n(aname, anlen, name)) {
                decode_amp_into(vstart, (size_t)(vend - vstart), out, outsz);
                return 1;
            }
        }
        /* valueless attribute: just loop; p already advanced past its name */
        if (anlen == 0)
            p++; /* guarantee forward progress on malformed input */
    }
    return 0;
}

/* ── element inner text ─────────────────────────────────────────────────── *
 * Locate the inner span of an element. `tag_end` is just past the open tag's
 * '>'; find the matching close tag for `name` and return (*istart,*ilen) for
 * the bytes between. If no close tag exists, the span runs to `end`.
 */
static void element_inner(const char *tag_end, const char *end,
                          const char *name,
                          const char **istart, size_t *ilen)
{
    const char *close = find_close_tag(tag_end, end, name);
    const char *iend = close ? close : end;
    const char *is = tag_end;
    if (is > iend)
        is = iend; /* defensive */
    *istart = is;
    *ilen = (size_t)(iend - is);
}

/* If the (whitespace-trimmed) span (*s, *len) is a single wrapping
 * <![CDATA[ ... ]]>, narrow it to the inner bytes. Otherwise leave as-is
 * (html_to_text tolerates inline CDATA markers as plain text). */
static void unwrap_cdata(const char **s, size_t *len)
{
    const char *p = *s;
    const char *e = *s + *len;
    const char *t = p;
    while (t < e && p_is_space((unsigned char)*t))
        t++;
    if (e - t >= 9 && p_starts_with(t, e, "<![CDATA[")) {
        const char *inner = t + 9;
        const char *q = inner;
        while (q + 3 <= e && !(q[0] == ']' && q[1] == ']' && q[2] == '>'))
            q++;
        if (q + 3 <= e) {
            *s = inner;
            *len = (size_t)(q - inner);
        }
    }
}

/* Extract element `name`'s text from [scan, end) as cleaned plain text (via
 * html_to_text). Returns 1 if the element was found (out filled, maybe ""),
 * 0 if not found (out untouched). */
static int tag_inner_text(const char *scan, const char *end, const char *name,
                          char *out, size_t outsz)
{
    const char *tag_end;
    int selfc;
    const char *is;
    size_t ilen;
    const char *open = find_open_tag(scan, end, name, &tag_end, &selfc);
    if (!open)
        return 0;
    if (selfc) {
        html_to_text("", 0, out, outsz);
        return 1;
    }
    element_inner(tag_end, end, name, &is, &ilen);
    unwrap_cdata(&is, &ilen);
    html_to_text(is, ilen, out, outsz);
    return 1;
}

/* Extract element `name`'s text from [scan, end) as a trimmed, &amp;-decoded
 * raw string (link/date fields — NO html_to_text). Returns 1 if found. */
static int tag_inner_raw(const char *scan, const char *end, const char *name,
                         char *out, size_t outsz)
{
    const char *tag_end;
    int selfc;
    const char *is;
    size_t ilen;
    const char *open = find_open_tag(scan, end, name, &tag_end, &selfc);
    if (!open)
        return 0;
    if (selfc) {
        if (outsz)
            out[0] = '\0';
        return 1;
    }
    element_inner(tag_end, end, name, &is, &ilen);
    unwrap_cdata(&is, &ilen);
    decode_amp_into(is, ilen, out, outsz);
    return 1;
}

/* ── Atom <link> selection ──────────────────────────────────────────────── *
 * Atom links live in attributes: <link rel="..." href="..." />. Prefer
 * rel="alternate" or a link with NO rel; skip rel="self"/"enclosure"/etc.
 * Scans every <link> in [scan, end) and writes the best href into out[outsz].
 * Bounded: at most a fixed number of <link> tags are examined per entry.
 */
static void atom_pick_link(const char *scan, const char *end,
                           char *out, size_t outsz)
{
    char fallback[STR_URL];
    const char *p = scan;
    int guard = 0;

    if (outsz == 0)
        return;
    out[0] = '\0';
    fallback[0] = '\0';

    while (p < end && guard < 64) {
        const char *tag_end;
        int selfc;
        char rel[64];
        char href[STR_URL];
        int has_rel, has_href;
        const char *open = find_open_tag(p, end, "link", &tag_end, &selfc);
        if (!open)
            break;
        guard++;
        p = tag_end; /* forward progress for the next iteration */

        has_rel = attr_value(open, tag_end, "rel", rel, sizeof rel);
        has_href = attr_value(open, tag_end, "href", href, sizeof href);
        if (!has_href || href[0] == '\0')
            continue;

        if (!has_rel || rel[0] == '\0' || p_ieq_z(rel, "alternate")) {
            /* preferred: no rel, or rel="alternate" */
            p_copy_z(out, outsz, href);
            return;
        }
        if (p_ieq_z(rel, "self") || p_ieq_z(rel, "enclosure") ||
            p_ieq_z(rel, "edit") || p_ieq_z(rel, "replies"))
            continue; /* explicitly not a reading link */
        if (fallback[0] == '\0')
            p_copy_z(fallback, sizeof fallback, href);
    }
    if (out[0] == '\0' && fallback[0] != '\0')
        p_copy_z(out, outsz, fallback);
}

/* ── format detection ───────────────────────────────────────────────────── *
 * Look at the first chunk of the document for a tell-tale root tag.
 *   returns 1 = RSS/RDF, 2 = Atom, 0 = neither.
 * We scan a bounded prefix so a multi-megabyte body can't slow detection.
 */
static int detect_format(const char *buf, const char *end)
{
    const char *p = buf;
    /* Only inspect up to the first ~4 KiB — root tags appear at the top. */
    const char *limit = buf + 4096;
    if (limit > end)
        limit = end;

    for (; p < limit; p++) {
        if (*p != '<')
            continue;
        if (p_starts_with(p, limit, "<rss"))
            return 1;
        if (p_starts_with(p, limit, "<rdf:rdf") || p_starts_with(p, limit, "<rdf "))
            return 1;
        if (p_starts_with(p, limit, "<feed"))
            return 2;
    }
    /* Fallback: a <feed ...> may sit past 4 KiB behind a long DOCTYPE; do one
     * cheap full-range check for the unambiguous Atom/RSS roots. */
    if (find_open_tag(buf, end, "feed", NULL, NULL))
        return 2;
    if (find_open_tag(buf, end, "rss", NULL, NULL) ||
        find_open_tag(buf, end, "rdf", NULL, NULL))
        return 1;
    return 0;
}

/* ── per-item / per-entry extraction ────────────────────────────────────── */

/* Fill one feed_item_t from the element subrange [istart, iend). `atom` picks
 * the Atom vs RSS field mapping. Never touches it->read. */
static void fill_item(feed_item_t *it, const char *istart, const char *iend,
                      int atom)
{
    /* zero the text fields we own (NOT read) */
    it->title[0] = '\0';
    it->link[0] = '\0';
    it->date[0] = '\0';
    it->summary[0] = '\0';

    /* title (both formats: <title>) */
    tag_inner_text(istart, iend, "title", it->title, sizeof it->title);
    if (it->title[0] == '\0')
        p_copy_z(it->title, sizeof it->title, "(untitled)");

    if (atom) {
        /* Atom link: attribute-based, with rel preference. */
        atom_pick_link(istart, iend, it->link, sizeof it->link);

        /* date: <updated> preferred, else <published>. */
        if (!tag_inner_raw(istart, iend, "updated", it->date,
                           sizeof it->date) ||
            it->date[0] == '\0')
            tag_inner_raw(istart, iend, "published", it->date,
                          sizeof it->date);

        /* summary: <content> preferred, else <summary>. */
        if (!tag_inner_text(istart, iend, "content", it->summary,
                            sizeof it->summary) ||
            it->summary[0] == '\0')
            tag_inner_text(istart, iend, "summary", it->summary,
                           sizeof it->summary);
    } else {
        /* RSS link: usually <link> text. If absent/empty, try an href attr
         * (some feeds use <link href="..."/> Atom-style inside RSS) or
         * rdf:about on the item — handled by the caller for RDF items. */
        if (!tag_inner_raw(istart, iend, "link", it->link, sizeof it->link) ||
            it->link[0] == '\0') {
            const char *tag_end;
            int selfc;
            const char *lk =
                find_open_tag(istart, iend, "link", &tag_end, &selfc);
            if (lk)
                attr_value(lk, tag_end, "href", it->link, sizeof it->link);
        }

        /* date: <pubDate> (RSS2) else <date> (dc:date, local name "date"). */
        if (!tag_inner_raw(istart, iend, "pubDate", it->date,
                           sizeof it->date) ||
            it->date[0] == '\0')
            tag_inner_raw(istart, iend, "date", it->date, sizeof it->date);

        /* summary: <content:encoded> (local "encoded") else <description>. */
        if (!tag_inner_text(istart, iend, "encoded", it->summary,
                            sizeof it->summary) ||
            it->summary[0] == '\0')
            tag_inner_text(istart, iend, "description", it->summary,
                           sizeof it->summary);
    }
}

/* ── public entry point ─────────────────────────────────────────────────── */

int feed_parse(const char *buf, size_t len, feed_t *f)
{
    const char *end;
    const char *scan;
    int fmt;
    int n = 0;

    if (!buf || !f || len == 0)
        return -1;
    end = buf + len;

    /* Reset only the fields this module owns. Leave f->url / f->loaded /
     * f->error / read-state to the caller. */
    f->title[0] = '\0';
    f->n_items = 0;

    fmt = detect_format(buf, end);
    if (fmt == 0)
        return -1; /* not a recognizable feed */

    if (fmt == 2) {
        /* ── ATOM ─────────────────────────────────────────────────────── */
        const char *first_entry;
        const char *tag_end;
        int selfc;

        /* Feed title = <title> child of <feed>, before the first <entry>. We
         * bound the title search to the prefix preceding the first entry. */
        first_entry = find_open_tag(buf, end, "entry", &tag_end, &selfc);
        {
            const char *title_end = first_entry ? first_entry : end;
            tag_inner_text(buf, title_end, "title", f->title,
                           sizeof f->title);
        }
        if (f->title[0] == '\0')
            p_copy_z(f->title, sizeof f->title, "(untitled feed)");

        /* Walk each <entry>...</entry>. */
        scan = buf;
        while (n < ITEMS_MAX) {
            const char *open =
                find_open_tag(scan, end, "entry", &tag_end, &selfc);
            const char *close;
            if (!open)
                break;
            if (selfc) {
                /* empty <entry/> — nothing to extract; advance and continue */
                scan = tag_end;
                continue;
            }
            close = find_close_tag(tag_end, end, "entry");
            {
                const char *iend = close ? close : end;
                fill_item(&f->items[n], tag_end, iend, /*atom=*/1);
                n++;
                /* advance past this entry; if no close tag, we're done */
                if (!close)
                    break;
                scan = close;
                /* skip the close tag's '>' to guarantee progress */
                while (scan < end && *scan != '>')
                    scan++;
                if (scan < end)
                    scan++;
            }
        }
    } else {
        /* ── RSS 2.0 / RSS 1.0 (RDF) ─────────────────────────────────────── */
        const char *chan_end;
        const char *first_item;
        const char *tag_end;
        int selfc;

        /* Channel title: prefer a <title> inside <channel> and before the
         * first <item>. We bound the search by the first <item>, and also by
         * the <channel> close if present, whichever is tighter. */
        first_item = find_open_tag(buf, end, "item", &tag_end, &selfc);
        chan_end = first_item ? first_item : end;
        {
            /* If a <channel> open exists, start the title search there. */
            const char *ctag_end;
            int cselfc;
            const char *chan =
                find_open_tag(buf, end, "channel", &ctag_end, &cselfc);
            const char *tstart = (chan && ctag_end <= chan_end) ? ctag_end : buf;
            tag_inner_text(tstart, chan_end, "title", f->title,
                           sizeof f->title);
        }
        if (f->title[0] == '\0')
            p_copy_z(f->title, sizeof f->title, "(untitled feed)");

        /* Walk each <item>. RSS2 items have a close tag; RDF items may be
         * self-closing-ish but typically have </item> too. We bound each
         * item's field search by its own close tag (or the next <item>). */
        scan = buf;
        while (n < ITEMS_MAX) {
            const char *open =
                find_open_tag(scan, end, "item", &tag_end, &selfc);
            const char *close;
            const char *next_open;
            const char *iend;
            const char *next_tag_end;
            int next_selfc;
            if (!open)
                break;

            close = find_close_tag(tag_end, end, "item");
            next_open =
                find_open_tag(tag_end, end, "item", &next_tag_end, &next_selfc);

            /* item body ends at the earliest of: its close tag, the next item
             * open, or end. (Self-closing RDF items have no inner text, but
             * their fields can hang as following siblings — bounding by the
             * next <item> keeps each item's fields its own.) */
            iend = end;
            if (close && close < iend)
                iend = close;
            if (next_open && next_open < iend)
                iend = next_open;

            fill_item(&f->items[n], tag_end, iend, /*atom=*/0);

            /* RDF: link may be rdf:about on the <item> open tag. */
            if (f->items[n].link[0] == '\0')
                attr_value(open, tag_end, "about", f->items[n].link,
                           sizeof f->items[n].link);

            n++;

            /* advance scan to just past this item's region */
            if (close) {
                scan = close;
                while (scan < end && *scan != '>')
                    scan++;
                if (scan < end)
                    scan++;
            } else if (next_open) {
                scan = next_open;
            } else {
                break;
            }
        }
    }

    f->n_items = n;
    return n;
}
