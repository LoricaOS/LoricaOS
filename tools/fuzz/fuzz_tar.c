/* Fuzz herald's ustar extractor.
 *
 * tar.c is compiled with -Dopen=fz_open -Dmkdir=fz_mkdir -Dwrite=fz_write
 * -Dclose=fz_close, so extraction performs NO real filesystem I/O. That makes
 * every iteration pure and fast, and — more importantly — it lets the harness
 * ASSERT THE CONTAINMENT PROPERTY directly: every path tar.c would create must
 * live under dest_root and must not contain a traversal component. A finding
 * here is a directory-escape, not just a memory error.
 *
 * Both entry points are exercised: tar_find_mem (pure) and tar_extract_mem,
 * with and without an allowed_prefixes allow-list. */
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>

#include "tar.h"

#define DEST "/fuzzroot"

/* ---- interposed syscalls -------------------------------------------- */

static void check_contained(const char *path)
{
    size_t rl = strlen(DEST);
    size_t n;

    /* mkdir_parents() walks every '/' in the full path, so it legitimately
     * mkdirs dest_root itself before descending. Anything else must be under
     * dest_root. (DEST is deliberately a single component so this is the only
     * such case.) */
    if (strcmp(path, DEST) == 0)
        return;

    assert(strncmp(path, DEST "/", rl + 1) == 0 &&
           "tar.c would create a path OUTSIDE dest_root");

    /* ... and must not climb back out. */
    assert(strstr(path, "/../") == NULL && "traversal component in target");
    n = strlen(path);
    assert(!(n >= 3 && strcmp(path + n - 3, "/..") == 0) &&
           "trailing traversal component in target");

    /* An empty final component would mean we NUL-terminated wrong. */
    assert(path[rl + 1] != '\0' && "empty entry path under dest_root");
}

int fz_open(const char *path, int flags, ...)
{
    (void)flags;
    check_contained(path);
    return 4242;                 /* a fake fd; never touched by libc */
}

int fz_mkdir(const char *path, unsigned int mode)
{
    (void)mode;
    check_contained(path);
    return 0;
}

long fz_write(int fd, const void *buf, unsigned long n)
{
    assert(fd == 4242);
    (void)buf;
    return (long)n;              /* always a full write */
}

int fz_close(int fd)
{
    assert(fd == 4242);
    return 0;
}

/* ---- harness --------------------------------------------------------- */

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n)
{
    const char *err;
    const unsigned char *data;
    size_t sz;
    uint8_t mode;
    /* Heap copy so ASan flags any read past the archive end. */
    uint8_t *buf;

    if (n < 1)
        return 0;
    mode = d[0];
    n--; d++;

    buf = malloc(n ? n : 1);
    if (!buf)
        return 0;
    memcpy(buf, d, n);

    /* find mode: the manifest lookup herald does before extracting. */
    err = NULL;
    data = NULL;
    sz = 0;
    if (tar_find_mem(buf, n, "manifest", &data, &sz) == 1) {
        /* A found entry must point inside the archive, wholly. */
        assert(data >= buf && data <= buf + n);
        assert(sz <= (size_t)((buf + n) - data) &&
               "tar_find_mem returned a size running past the archive");
        /* Touch it the way the caller (manifest_parse) would. */
        if (sz)
            assert(memchr(data, 0, sz) != NULL || 1);
    }

    /* extract mode, with and without the install-prefix allow-list. */
    err = NULL;
    if (mode & 1) {
        static const char *const prefixes[] = {
            "apps/fuzz/", "lib/engine/", NULL
        };
        tar_extract_mem(buf, n, DEST, prefixes, &err);
    } else {
        tar_extract_mem(buf, n, DEST, NULL, &err);
    }

    free(buf);
    return 0;
}
