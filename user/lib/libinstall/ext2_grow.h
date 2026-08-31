#ifndef LIBINSTALL_EXT2_GROW_H
#define LIBINSTALL_EXT2_GROW_H

#include <stdint.h>

typedef int (*ext2_grow_io_fn)(void *ctx, uint64_t sector, uint64_t count,
                               void *buf, int write);

/* Grow the single-group 4 KiB ext2 image copied by the installer to fit the
 * target partition, capped at the one-block group-descriptor table Aegis can
 * currently mount (128 groups / 16 GiB). The primary superblock is committed
 * last, so an interrupted attempt still describes the original filesystem.
 * Returns 0 on success, -1 with a static diagnostic in *error_out on failure. */
int ext2_grow(ext2_grow_io_fn io, void *ctx, uint64_t target_sectors,
              uint64_t *grown_sectors, const char **error_out);

#endif
