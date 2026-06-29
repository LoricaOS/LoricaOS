/*
 * tar.c — self-contained extractor for uncompressed POSIX ustar archives.
 *
 * Format (per entry):
 *   - 512-byte header block, fields at fixed offsets:
 *       name[100]    @0
 *       mode[8]      @100   (octal ASCII, space/NUL terminated)
 *       size[12]     @124   (octal ASCII, space/NUL terminated)
 *       typeflag[1]  @156   ('0'/'\0' regular, '5' dir, '2' symlink,
 *                            'L'/'x'/'g'/... extension records — skipped)
 *       magic[6]     @257   ("ustar\0" or "ustar " — we accept a "ustar" prefix)
 *       prefix[155]  @345
 *   - File data follows the header, padded up to the next 512-byte multiple.
 *   - Full path = prefix + "/" + name when prefix is non-empty, else name.
 *   - Iteration ends at an all-zero name field (or two zero blocks / EOF).
 *
 * Security: entry paths must be relative; absolute paths, ".." components,
 * and anything that would escape dest_root are rejected and abort the whole
 * extraction. Every field read is bounded against buf+len; a truncated final
 * block is detected rather than over-read.
 */

#include "tar.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

#define TAR_BLOCK   512
#define TAR_NAME    100
#define TAR_PREFIX  155
/* prefix(155) + "/" + name(100) + NUL */
#define TAR_PATHMAX (TAR_PREFIX + 1 + TAR_NAME + 1)

#define SET_ERR(p, s) do { if (p) { *(p) = (s); } } while (0)

/* Parse an octal ASCII field of at most field_len bytes.
 * Skips leading spaces, reads octal digits until a space, NUL, or the end of
 * the field. Returns the value via *out; returns 0 on success, -1 if a
 * non-octal, non-terminator byte is seen. */
static int oct_parse(const unsigned char *field, size_t field_len,
                     unsigned long long *out)
{
    size_t i = 0;
    unsigned long long v = 0;

    while (i < field_len && field[i] == ' ') {
        i++;
    }
    for (; i < field_len; i++) {
        unsigned char c = field[i];
        if (c == ' ' || c == '\0') {
            break;
        }
        if (c < '0' || c > '7') {
            return -1;
        }
        v = (v << 3) | (unsigned long long)(c - '0');
    }
    *out = v;
    return 0;
}

/* Is a 512-byte block entirely zero? */
static int block_is_zero(const unsigned char *b)
{
    int i;
    for (i = 0; i < TAR_BLOCK; i++) {
        if (b[i] != 0) {
            return 0;
        }
    }
    return 1;
}

/* Strip a single leading "./" (GNU tar emits "./manifest" etc.). Done in
 * place on a writable copy. */
static const char *strip_dot_slash(const char *p)
{
    if (p[0] == '.' && p[1] == '/') {
        return p + 2;
    }
    return p;
}

/* Reject absolute paths and any ".." path component. Returns 0 if safe,
 * -1 if the path is unsafe. Empty path is treated as unsafe. */
static int path_is_safe(const char *p)
{
    size_t i = 0;

    if (p[0] == '\0') {
        return -1;
    }
    if (p[0] == '/') {
        return -1;
    }
    /* Scan component by component looking for exactly "..". */
    while (p[i] != '\0') {
        /* p[i] is the first char of a component here. */
        if (p[i] == '.' && p[i + 1] == '.' &&
            (p[i + 2] == '/' || p[i + 2] == '\0')) {
            return -1;
        }
        /* advance to next component */
        while (p[i] != '\0' && p[i] != '/') {
            i++;
        }
        while (p[i] == '/') {
            i++;
        }
    }
    return 0;
}

/* Build the full entry path (prefix + "/" + name, or name) into out[outsz].
 * Both name and prefix are bounded fields that may be unterminated; we copy
 * at most their field length and NUL-terminate. Returns 0 on success, -1 if
 * the result would not fit. */
static int build_path(const unsigned char *hdr, char *out, size_t outsz)
{
    char name[TAR_NAME + 1];
    char prefix[TAR_PREFIX + 1];
    size_t nl, pl;

    memcpy(name, hdr + 0, TAR_NAME);
    name[TAR_NAME] = '\0';
    nl = strnlen(name, TAR_NAME);
    name[nl] = '\0';

    memcpy(prefix, hdr + 345, TAR_PREFIX);
    prefix[TAR_PREFIX] = '\0';
    pl = strnlen(prefix, TAR_PREFIX);
    prefix[pl] = '\0';

    if (pl > 0) {
        if (pl + 1 + nl + 1 > outsz) {
            return -1;
        }
        memcpy(out, prefix, pl);
        out[pl] = '/';
        memcpy(out + pl + 1, name, nl);
        out[pl + 1 + nl] = '\0';
    } else {
        if (nl + 1 > outsz) {
            return -1;
        }
        memcpy(out, name, nl);
        out[nl] = '\0';
    }
    return 0;
}

/* Returns 1 if `s` begins with `pre`. */
static int str_starts_with(const char *s, const char *pre)
{
    while (*pre) {
        if (*s++ != *pre++)
            return 0;
    }
    return 1;
}

/* Strip a single trailing '/' (directory entries carry one). In place. */
static void strip_trailing_slash(char *p)
{
    size_t n = strlen(p);
    if (n > 0 && p[n - 1] == '/') {
        p[n - 1] = '\0';
    }
}

/* mkdir each prefix of `path` up to but NOT including the final component.
 * EEXIST is ignored. Returns 0 on success, -1 on a real mkdir failure.
 * `path` is modified transiently (slashes are temporarily NUL'd) and
 * restored before return. */
static int mkdir_parents(char *path)
{
    size_t i;

    for (i = 1; path[i] != '\0'; i++) {
        if (path[i] != '/') {
            continue;
        }
        path[i] = '\0';
        if (path[0] != '\0') {
            if (mkdir(path, 0755) != 0 && errno != EEXIST) {
                path[i] = '/';
                return -1;
            }
        }
        path[i] = '/';
    }
    return 0;
}

/* mkdir the path itself plus all parents (used for directory entries). */
static int mkdir_p(char *path)
{
    if (mkdir_parents(path) != 0) {
        return -1;
    }
    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

/* Validate a ustar magic field ("ustar\0" or "ustar "). We accept any field
 * whose first five bytes are "ustar". Returns 1 if valid. */
static int magic_ok(const unsigned char *hdr)
{
    return memcmp(hdr + 257, "ustar", 5) == 0;
}

/*
 * Core iterator. Walks header blocks. For each non-skipped entry it invokes
 * the supplied behavior via the function parameters:
 *   - extract != 0: write regular files / make dirs under dest_root.
 *   - find_name != NULL: locate that entry; on match set fd and fsz, return 1.
 *
 * Returns:
 *   for extract mode: 0 on success, negative on error.
 *   for find mode:    1 found, 0 not found, negative on malformed archive.
 */
static int tar_walk(const void *buf, size_t len, const char *dest_root,
                    const char *const *allowed_prefixes,
                    const char **err, int extract,
                    const char *find_name,
                    const unsigned char **fd, size_t *fsz)
{
    const unsigned char *base = (const unsigned char *)buf;
    size_t off = 0;
    int zero_run = 0;

    while (off + TAR_BLOCK <= len) {
        const unsigned char *hdr = base + off;
        unsigned long long size, mode;
        size_t data_off, padded;
        char rawpath[TAR_PATHMAX];
        const char *path;
        char typeflag;

        if (block_is_zero(hdr)) {
            zero_run++;
            off += TAR_BLOCK;
            if (zero_run >= 2) {
                break;
            }
            continue;
        }
        zero_run = 0;

        /* A header whose name field is all-zero terminates iteration. */
        {
            int all_zero = 1;
            size_t k;
            for (k = 0; k < TAR_NAME; k++) {
                if (hdr[k] != 0) {
                    all_zero = 0;
                    break;
                }
            }
            if (all_zero) {
                break;
            }
        }

        if (!magic_ok(hdr)) {
            SET_ERR(err, "tar: bad ustar magic");
            return -1;
        }

        if (oct_parse(hdr + 124, 12, &size) != 0) {
            SET_ERR(err, "tar: bad size field");
            return -1;
        }
        if (oct_parse(hdr + 100, 8, &mode) != 0) {
            SET_ERR(err, "tar: bad mode field");
            return -1;
        }

        typeflag = (char)hdr[156];

        data_off = off + TAR_BLOCK;
        /* Pad data length up to the next 512-byte multiple. */
        padded = (size_t)((size + (TAR_BLOCK - 1)) / TAR_BLOCK) * TAR_BLOCK;

        /* Bound the data region against the buffer. Skip-types still need a
         * correct advance, so this check guards every type. */
        if (data_off > len || padded > len - data_off) {
            SET_ERR(err, "tar: truncated entry data");
            return -1;
        }

        if (build_path(hdr, rawpath, sizeof(rawpath)) != 0) {
            SET_ERR(err, "tar: entry path too long");
            return -1;
        }
        path = strip_dot_slash(rawpath);

        /* Types we advance past without acting on: extension records
         * ('L','x','g'), symlinks ('2' — MVP skip), hardlinks, devices, etc.
         * Only regular ('0'/'\0') and directory ('5') are handled. */
        if (typeflag == '0' || typeflag == '\0' || typeflag == '5') {
            /* Path safety applies to anything we would create or match. */
            if (path_is_safe(path) != 0) {
                SET_ERR(err, "tar: unsafe entry path");
                return -1;
            }

            if (find_name != NULL) {
                /* find mode: match regular files (the manifest is a file). */
                if (typeflag != '5' && strcmp(path, find_name) == 0) {
                    if (fd) {
                        *fd = base + data_off;
                    }
                    if (fsz) {
                        *fsz = (size_t)size;
                    }
                    return 1;
                }
            } else if (extract) {
                char full[TAR_PATHMAX + 256];
                size_t rl = strlen(dest_root);
                size_t need;

                /* Bundle-prefix restriction: only the manifest, entries under
                 * one of allowed_prefixes, and the ancestor directory entries of
                 * a prefix (e.g. "apps/", "apps/<id>/" — tar emits these and they
                 * only mkdir already-existing dirs) may be written. Anything else
                 * aborts, so a package can't write outside its declared footprint. */
                if (allowed_prefixes != NULL) {
                    int ok = (strcmp(path, "manifest") == 0);
                    const char *const *pp;
                    size_t pl = strlen(path);
                    for (pp = allowed_prefixes; !ok && *pp != NULL; pp++) {
                        if (str_starts_with(path, *pp))
                            ok = 1;
                        else if (pl > 0 && path[pl - 1] == '/' &&
                                 str_starts_with(*pp, path))
                            ok = 1;   /* ancestor directory of a prefix */
                    }
                    if (!ok) {
                        SET_ERR(err, "tar: entry outside declared install paths");
                        return -1;
                    }
                }

                /* dest_root + "/" + path + NUL */
                need = rl + 1 + strlen(path) + 1;
                if (need > sizeof(full)) {
                    SET_ERR(err, "tar: target path too long");
                    return -1;
                }
                memcpy(full, dest_root, rl);
                full[rl] = '/';
                strcpy(full + rl + 1, path);
                strip_trailing_slash(full);

                if (typeflag == '5') {
                    if (mkdir_p(full) != 0) {
                        SET_ERR(err, "tar: mkdir failed");
                        return -1;
                    }
                } else {
                    mode_t fmode = (mode & 0111) ? 0755 : 0644;
                    int wfd;
                    size_t written = 0;

                    if (mkdir_parents(full) != 0) {
                        SET_ERR(err, "tar: mkdir parent failed");
                        return -1;
                    }
                    wfd = open(full, O_WRONLY | O_CREAT | O_TRUNC, fmode);
                    if (wfd < 0) {
                        SET_ERR(err, "tar: open for write failed");
                        return -1;
                    }
                    while (written < (size_t)size) {
                        size_t chunk = (size_t)size - written;
                        ssize_t w = write(wfd,
                                          base + data_off + written,
                                          chunk);
                        if (w < 0) {
                            if (errno == EINTR) {
                                continue;
                            }
                            close(wfd);
                            SET_ERR(err, "tar: write failed");
                            return -1;
                        }
                        if (w == 0) {
                            close(wfd);
                            SET_ERR(err, "tar: short write");
                            return -1;
                        }
                        written += (size_t)w;
                    }
                    if (close(wfd) != 0) {
                        SET_ERR(err, "tar: close failed");
                        return -1;
                    }
                }
            }
        }
        /* else: skipped type — already advanced via padded below. */

        off = data_off + padded;
    }

    if (find_name != NULL) {
        return 0;   /* not found */
    }
    return 0;       /* extract success */
}

int tar_extract_mem(const void *buf, size_t len, const char *dest_root,
                    const char *const *allowed_prefixes, const char **err)
{
    SET_ERR(err, NULL);
    if (buf == NULL || dest_root == NULL) {
        SET_ERR(err, "tar: null argument");
        return -1;
    }
    return tar_walk(buf, len, dest_root, allowed_prefixes, err, 1,
                    NULL, NULL, NULL);
}

int tar_find_mem(const void *buf, size_t len, const char *name,
                 const unsigned char **data, size_t *size)
{
    const char *err = NULL;
    const char *match;

    if (buf == NULL || name == NULL) {
        return -1;
    }
    /* Match against the same normalized form we extract under: a leading
     * "./" is stripped from entries, so strip it from the query too. */
    match = strip_dot_slash(name);

    return tar_walk(buf, len, NULL, NULL, &err, 0, match, data, size);
}
