#ifndef EXT2_REPAIR_H
#define EXT2_REPAIR_H

#include <stdint.h>

typedef int (*ext2_repair_io_fn)(void *ctx, uint64_t sector, uint64_t count,
                                 void *buf, int write);

typedef struct {
    uint32_t blocks, inodes, directories;
    uint32_t block_bits_fixed, inode_bits_fixed, counters_fixed;
    int was_clean;
} ext2_repair_report_t;

/* Check or conservatively repair a LoricaOS 4 KiB ext2 filesystem.
 * 0 = clean, 1 = repairable inconsistencies found/fixed, -1 = unsafe damage. */
int ext2_repair(ext2_repair_io_fn io, void *ctx, uint64_t device_sectors,
                int repair, ext2_repair_report_t *report,
                const char **error_out);

#endif
