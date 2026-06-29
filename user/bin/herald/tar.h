#ifndef HERALD_TAR_H
#define HERALD_TAR_H
#include <stddef.h>

/* Extract an uncompressed POSIX ustar archive (buf/len) under dest_root
 * (e.g. "/"). Entry paths are relative; reject absolute or containing "..".
 * If allowed_prefixes is non-NULL (a NULL-terminated array of prefix strings),
 * every entry must be exactly "manifest", begin with one of the prefixes (e.g.
 * "apps/<id>/", "lib/ladybird/"), or be an ancestor directory of one; any other
 * entry aborts the whole extraction. Pass NULL to allow any (safe) relative path.
 * Creates parent dirs (0755). Regular files: 0755 if tar mode has any exec
 * bit, else 0644. Returns 0, or negative on error; *err (nullable) gets a
 * static string. */
int tar_extract_mem(const void *buf, size_t len, const char *dest_root,
                    const char *const *allowed_prefixes, const char **err);

/* Locate a single entry by exact name; on success sets *data (pointer INTO
 * buf, no copy) and *size. Returns 1 if found, 0 if not, negative on malformed
 * archive. Used to read `manifest` before extracting. */
int tar_find_mem(const void *buf, size_t len, const char *name,
                 const unsigned char **data, size_t *size);
#endif
