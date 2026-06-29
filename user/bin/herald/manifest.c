/* manifest.c — herald ini-manifest parser (see manifest.h).
 *
 * Parses a key=value ini manifest from an in-memory buffer (NOT a FILE).
 * Modeled on user/lib/glyph/apps.c, but scans line-by-line over buf/len:
 * find '\n', copy the line into a bounded stack buffer, split on first '='.
 */
#include <string.h>

#include "manifest.h"

/* Copy at most dstsz-1 bytes of [src, src+n) into dst, then NUL-terminate.
 * Always bounded; never overflows dst. */
static void
bounded_copy(char *dst, size_t dstsz, const char *src, size_t n)
{
    if (dstsz == 0)
        return;
    if (n > dstsz - 1)
        n = dstsz - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* Trim trailing CR and spaces/tabs from a NUL-terminated string in place. */
static void
rtrim(char *s)
{
    size_t len = strlen(s);
    while (len > 0) {
        char c = s[len - 1];
        if (c == '\r' || c == ' ' || c == '\t')
            s[--len] = '\0';
        else
            break;
    }
}

int
manifest_parse(const void *buf, size_t len, herald_manifest_t *out)
{
    if (!buf || !out)
        return -1;

    memset(out, 0, sizeof(*out));

    const char *p = (const char *)buf;
    size_t off = 0;

    while (off < len) {
        /* Locate the end of this line (up to '\n' or end of buffer). */
        size_t start = off;
        size_t end = off;
        while (end < len && p[end] != '\n')
            end++;

        size_t line_len = end - start;   /* excludes the '\n' */

        /* Advance off past the newline (or to end of buffer). */
        off = (end < len) ? end + 1 : end;

        /* Copy the line into a bounded stack buffer. Over-long lines are
         * truncated safely (we never read or write past the buffer). */
        char line[512];
        bounded_copy(line, sizeof(line), p + start, line_len);

        /* Drop trailing CR / whitespace (handles CRLF input). */
        rtrim(line);

        /* Skip blank lines and comments. */
        if (line[0] == '\0' || line[0] == '#')
            continue;

        /* Split on the first '='. Lines without '=' are skipped. */
        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = '\0';
        char *key = line;
        char *val = eq + 1;

        /* Trim trailing whitespace off the key (e.g. "id  ="). */
        rtrim(key);

        /* Skip leading whitespace on the key. */
        while (*key == ' ' || *key == '\t')
            key++;

        /* Reject control characters in the value. A TAB would corrupt the
         * TAB-separated installed-package db; other control bytes have no
         * legitimate use and could inject into the db or a cap-policy file.
         * (Embedded newlines are already impossible — we split on '\n'.) */
        {
            const char *c;
            for (c = val; *c; c++) {
                if ((unsigned char)*c < 0x20 || (unsigned char)*c == 0x7f)
                    return -1;
            }
        }

        if (strcmp(key, "id") == 0) {
            if (strchr(val, '/'))      /* id must be a bare dir name */
                return -1;
            bounded_copy(out->id, sizeof(out->id), val, strlen(val));
        } else if (strcmp(key, "name") == 0) {
            bounded_copy(out->name, sizeof(out->name), val, strlen(val));
        } else if (strcmp(key, "version") == 0) {
            bounded_copy(out->version, sizeof(out->version), val, strlen(val));
        } else if (strcmp(key, "exec") == 0) {
            if (strchr(val, '/'))      /* exec is a filename, not a path */
                return -1;
            bounded_copy(out->exec, sizeof(out->exec), val, strlen(val));
        } else if (strcmp(key, "caps") == 0) {
            bounded_copy(out->caps, sizeof(out->caps), val, strlen(val));
        } else if (strncmp(key, "caps.", 5) == 0) {
            /* Per-binary cap policy: caps.<binary>=<caps>. The binary is an
             * exec basename (no '/'), the value is space-separated cap names. */
            const char *bin = key + 5;
            if (bin[0] && !strchr(bin, '/') &&
                out->nbincaps < HERALD_MAX_BINCAPS) {
                herald_bincap_t *bc = &out->bincaps[out->nbincaps];
                bounded_copy(bc->binary, sizeof(bc->binary), bin, strlen(bin));
                bounded_copy(bc->caps, sizeof(bc->caps), val, strlen(val));
                out->nbincaps++;
            }
        } else if (strcmp(key, "depends") == 0) {
            bounded_copy(out->depends, sizeof(out->depends), val, strlen(val));
        } else if (strcmp(key, "paths") == 0) {
            bounded_copy(out->paths, sizeof(out->paths), val, strlen(val));
        } else if (strcmp(key, "class") == 0) {
            /* class=system marks a first-party, signature-trusted system
             * package (see manifest.h). Any other value (incl. the implicit
             * default) is an ordinary third-party app. */
            out->is_system = (strcmp(val, "system") == 0);
        }
        /* Unknown keys: ignore. */
    }

    /* id/name/version are always required. exec is required only for app
     * packages — a system package installs a tree, not a single launcher, so
     * it has no meaningful exec. */
    if (!out->id[0] || !out->name[0] || !out->version[0])
        return -1;
    if (!out->is_system && !out->exec[0])
        return -1;

    return 0;
}
