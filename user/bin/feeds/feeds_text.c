/* feeds_text.c — HTML/XML fragment -> safe ASCII plain text.
 *
 * The TEXT module of the Aegis "Feeds" reader. USERSPACE (musl libc).
 *
 * Single forward pass over in[0..in_len), writing to out[0..outsz) with a
 * running write index `w` that is checked before EVERY byte. The output is
 * always NUL-terminated (when outsz>=1) and never written past out[outsz-1].
 *
 * Defensive by design: `in` is treated as NOT NUL-terminated; only in[0..in_len)
 * is ever read. No allocation, no recursion, no floats, no unbounded loops.
 */
#include "feeds.h"

#include <stddef.h>
#include <string.h>

/* Maximum bytes we scan ahead looking for an entity's terminating ';'.
 * "&" + up to ~10 name/number chars + ";" — keep the lookahead bounded. */
#define ENT_MAX_SCAN 12

/* ─────────────────────────────────────────────────────────────────────────
 * Output sink.  `w` is the next free index; valid writes are out[0..outsz-2]
 * (out[outsz-1] is reserved for the terminating NUL).  emit_byte() refuses to
 * write when the buffer is full, so the caller never has to special-case it.
 * ───────────────────────────────────────────────────────────────────────── */
typedef struct {
    char  *out;
    size_t outsz;
    size_t w;        /* next write index                                  */
    int    pend_sp;  /* a space is pending (collapsed whitespace)         */
    int    started;  /* at least one non-space byte already emitted       */
} sink_t;

/* Emit one already-vetted byte (never a whitespace byte — those go through
 * sink_space()).  Bound check leaves room for the final NUL. */
static void emit_byte(sink_t *s, char c)
{
    /* Flush a pending collapsed space first, but only between words. */
    if (s->pend_sp) {
        s->pend_sp = 0;
        if (s->started && s->w + 1 < s->outsz) {  /* room for sp + NUL    */
            s->out[s->w++] = ' ';
        }
    }
    if (s->w + 1 < s->outsz) {                     /* room for c + NUL     */
        s->out[s->w++] = c;
        s->started = 1;
    }
}

/* Emit a run-collapsing space.  Actual ' ' is deferred until the next real
 * byte, which trims trailing whitespace for free. */
static void sink_space(sink_t *s)
{
    if (s->started)
        s->pend_sp = 1;
    /* leading whitespace: dropped (pend_sp stays 0 until started) */
}

/* Emit a NUL-terminated ASCII string through emit_byte (used for "--", "...",
 * "TM", "deg").  All bytes are printable ASCII by construction. */
static void emit_str(sink_t *s, const char *str)
{
    while (*str)
        emit_byte(s, *str++);
}

/* ─────────────────────────────────────────────────────────────────────────
 * Entity handling
 * ───────────────────────────────────────────────────────────────────────── */

/* Map a (validated) Unicode/ASCII code point to safe ASCII output.
 * Printable ASCII passes through; a handful of common punctuation code points
 * are folded to ASCII; everything else becomes '?'. */
static void emit_codepoint(sink_t *s, unsigned long cp)
{
    if (cp >= 32 && cp <= 126) {           /* printable ASCII             */
        emit_byte(s, (char)cp);
        return;
    }
    switch (cp) {
    case 0x2018: case 0x2019: emit_byte(s, '\''); return; /* curly single */
    case 0x201C: case 0x201D: emit_byte(s, '"');  return; /* curly double */
    case 0x2013: case 0x2014: emit_byte(s, '-');  return; /* en/em dash   */
    case 0x2026: emit_str(s, "...");              return; /* ellipsis     */
    case 0x00A0: sink_space(s);                   return; /* nbsp         */
    default:     emit_byte(s, '?');               return; /* unknown      */
    }
}

/* Named entities, with the ASCII expansion each maps to. */
struct named_ent { const char *name; const char *repl; };
static const struct named_ent NAMED[] = {
    { "amp",    "&"   },
    { "lt",     "<"   },
    { "gt",     ">"   },
    { "quot",   "\""  },
    { "apos",   "'"   },
    { "nbsp",   " "   },
    { "copy",   "c"   },
    { "mdash",  "--"  },
    { "ndash",  "-"   },
    { "hellip", "..." },
    { "rsquo",  "'"   },
    { "lsquo",  "'"   },
    { "ldquo",  "\""  },
    { "rdquo",  "\""  },
    { "trade",  "TM"  },
    { "reg",    "R"   },
    { "deg",    "deg" },
    { "middot", "."   },
};
#define NAMED_N ((int)(sizeof(NAMED) / sizeof(NAMED[0])))

/* Attempt to decode an entity starting at in[i] (which is '&').
 * On success, emit the expansion and return the index *after* the ';'.
 * On failure, emit a literal '&' and return i+1.
 *
 * `in_len` bounds all reads; we scan at most ENT_MAX_SCAN bytes for ';'. */
static size_t decode_entity(sink_t *s, const char *in, size_t in_len, size_t i)
{
    size_t j;
    size_t semi = 0;       /* index of terminating ';' (0 == not found)    */
    int    found = 0;

    /* Find ';' within the bounded window, staying inside in_len. */
    for (j = i + 1; j < in_len && j <= i + ENT_MAX_SCAN; j++) {
        if (in[j] == ';') { semi = j; found = 1; break; }
        /* '&' or '<' before ';' means this isn't a well-formed entity. */
        if (in[j] == '&' || in[j] == '<') break;
    }
    if (!found) {
        emit_byte(s, '&');
        return i + 1;
    }

    /* Numeric: &#NNN;  or  &#xHH; / &#XHH;  */
    if (in[i + 1] == '#') {
        unsigned long cp = 0;
        size_t        p  = i + 2;
        int           any = 0;
        int           hex = 0;

        if (p < semi && (in[p] == 'x' || in[p] == 'X')) { hex = 1; p++; }

        for (; p < semi; p++) {
            char c = in[p];
            unsigned digit;
            if (c >= '0' && c <= '9')          digit = (unsigned)(c - '0');
            else if (hex && c >= 'a' && c <= 'f') digit = (unsigned)(c - 'a' + 10);
            else if (hex && c >= 'A' && c <= 'F') digit = (unsigned)(c - 'A' + 10);
            else { any = 0; break; }            /* bad digit -> not valid   */
            cp = cp * (hex ? 16UL : 10UL) + digit;
            any = 1;
            if (cp > 0x10FFFFUL) cp = 0xFFFFFFFFUL; /* clamp; will map to '?'*/
        }

        if (!any) {                             /* "&#;" or junk -> literal */
            emit_byte(s, '&');
            return i + 1;
        }
        emit_codepoint(s, cp);
        return semi + 1;
    }

    /* Named entity: case-sensitive match against the table. */
    {
        size_t nlen = semi - (i + 1);
        int    k;
        for (k = 0; k < NAMED_N; k++) {
            size_t want = strlen(NAMED[k].name);
            if (want == nlen && memcmp(in + i + 1, NAMED[k].name, want) == 0) {
                emit_str(s, NAMED[k].repl);
                return semi + 1;
            }
        }
    }

    /* Unknown entity name -> emit a literal '&', resume after it. */
    emit_byte(s, '&');
    return i + 1;
}

/* ─────────────────────────────────────────────────────────────────────────
 * Tag handling
 * ───────────────────────────────────────────────────────────────────────── */

/* Case-insensitive compare of in[i .. i+n) against a lowercase ASCII literal.
 * Bounded by in_len; returns 1 only if all n bytes are present and match. */
static int tag_is(const char *in, size_t in_len, size_t i, const char *lit)
{
    size_t n = strlen(lit);
    size_t k;
    if (i + n > in_len) return 0;
    for (k = 0; k < n; k++) {
        char c = in[i + k];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != lit[k]) return 0;
    }
    return 1;
}

/* True if the tag name starting at `name` (right after '<' or "</") is a
 * block/line boundary tag that should collapse to a single space.
 * `name` points at the first name char; bounded by in_len. */
static int is_space_tag(const char *in, size_t in_len, size_t name)
{
    static const char *const TAGS[] = {
        "br", "p", "li", "div", "tr",
        "h1", "h2", "h3", "h4", "h5", "h6",
    };
    int t;
    int n = (int)(sizeof(TAGS) / sizeof(TAGS[0]));
    for (t = 0; t < n; t++) {
        size_t tl = strlen(TAGS[t]);
        if (tag_is(in, in_len, name, TAGS[t])) {
            /* Next char must end the tag name (space, '>', '/', or EOI) so
             * "<p>" matches but "<pre>" / "<param>" do not. */
            size_t after = name + tl;
            if (after >= in_len) return 1;
            char c = in[after];
            if (c == '>' || c == '/' || c == ' ' || c == '\t' ||
                c == '\n' || c == '\r')
                return 1;
        }
    }
    return 0;
}

/* Skip a tag (or comment, or CDATA, or script/style body) starting at in[i],
 * where in[i] == '<'.  Returns the index of the first byte AFTER the construct.
 * May emit a single space for block/line boundary tags.  CDATA inner bytes are
 * NOT consumed here — we return the index of the inner content so the main loop
 * re-cleans them (tags inside CDATA still get stripped). */
static size_t handle_tag(sink_t *s, const char *in, size_t in_len, size_t i)
{
    size_t j;

    /* HTML comment: <!-- ... --> — skip entirely.  Scan for "-->", with all
     * three reads bounded by in_len (j+2 <= in_len-1). */
    if (tag_is(in, in_len, i, "<!--")) {
        for (j = i + 4; j + 2 < in_len; j++) {
            if (in[j] == '-' && in[j + 1] == '-' && in[j + 2] == '>')
                return j + 3;
        }
        return in_len;  /* unterminated comment */
    }

    /* CDATA: <![CDATA[ ... ]]>  — emit the INNER bytes as ordinary text.
     * We just step past the "<![CDATA[" marker and let the main loop process
     * the inner content (so tags inside CDATA are still stripped, entities
     * still decoded).  The closing "]]>" is recognised and dropped by the
     * main loop's ']]>' check.  No recursion, single pass. */
    if (tag_is(in, in_len, i, "<![cdata[")) {
        return i + 9;   /* resume at inner content                         */
    }

    /* <script ...> ... </script> and <style ...> ... </style>: drop body. */
    if (tag_is(in, in_len, i, "<script") || tag_is(in, in_len, i, "<style")) {
        const char *close = tag_is(in, in_len, i, "<script")
                                ? "</script"
                                : "</style";
        size_t clen = strlen(close);
        /* Find the closing tag, then skip to its '>' (or EOI). */
        for (j = i + 1; j < in_len; j++) {
            if (tag_is(in, in_len, j, close)) {
                /* skip to and past the '>' of the close tag */
                size_t k = j + clen;
                while (k < in_len && in[k] != '>') k++;
                return (k < in_len) ? k + 1 : in_len;
            }
        }
        return in_len;  /* unterminated script/style */
    }

    /* Plain tag.  Decide if it is a block/line boundary (-> single space). */
    {
        size_t name = i + 1;
        int    boundary;
        if (name < in_len && in[name] == '/')   /* closing tag </...>      */
            name++;
        boundary = (name < in_len) ? is_space_tag(in, in_len, name) : 0;

        /* Skip to and past the matching '>' (or to in_len if unterminated). */
        j = i + 1;
        while (j < in_len && in[j] != '>')
            j++;
        if (j < in_len)        /* landed on '>' */
            j++;
        /* else j == in_len: unterminated tag, consume the rest. */

        if (boundary)
            sink_space(s);
        return j;
    }
}

/* ─────────────────────────────────────────────────────────────────────────
 * Public entry point
 * ───────────────────────────────────────────────────────────────────────── */
void html_to_text(const char *in, size_t in_len, char *out, size_t outsz)
{
    sink_t s;
    size_t i = 0;

    if (outsz == 0)
        return;                 /* nowhere to even put a NUL               */
    if (out == NULL)
        return;

    s.out     = out;
    s.outsz   = outsz;
    s.w       = 0;
    s.pend_sp = 0;
    s.started = 0;

    if (in == NULL || in_len == 0) {
        out[0] = '\0';
        return;
    }

    while (i < in_len) {
        unsigned char c = (unsigned char)in[i];

        /* Stop early once the output is full (room only for the NUL left).
         * Keeps us from scanning the whole input for nothing. */
        if (s.w + 1 >= s.outsz)
            break;

        if (c == '<') {
            i = handle_tag(&s, in, in_len, i);
            continue;
        }

        /* CDATA terminator ']]>' — drop it without emitting. */
        if (c == ']' && i + 2 < in_len &&
            in[i + 1] == ']' && in[i + 2] == '>') {
            i += 3;
            continue;
        }

        if (c == '&') {
            i = decode_entity(&s, in, in_len, i);
            continue;
        }

        /* Whitespace -> collapse to a single (deferred) space. */
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
            c == '\f' || c == '\v') {
            sink_space(&s);
            i++;
            continue;
        }

        /* Non-ASCII / control bytes: drop (keep output ASCII-only & safe). */
        if (c >= 128 || c < 32) {
            i++;
            continue;
        }

        /* Ordinary printable ASCII. */
        emit_byte(&s, (char)c);
        i++;
    }

    /* Any pending space is intentionally NOT flushed (trailing-trim). */
    out[s.w] = '\0';
}
