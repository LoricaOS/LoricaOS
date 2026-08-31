/* Offline ext2 growth for the freshly copied LoricaOS root filesystem.
 *
 * The live image is intentionally one partial 4 KiB block group so it stays
 * small in RAM and on the ISO. The installed partition is much larger. This
 * grows that copy by constructing ordinary ext2 block groups, including sparse
 * backup superblocks and the resize inode's reserved-GDT references. It is not
 * a general resize2fs replacement: unsupported geometry fails closed. */
#include "ext2_grow.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define SECTOR_SIZE             512u
#define EXT2_BLOCK_SIZE         4096u
#define SECTORS_PER_BLOCK       (EXT2_BLOCK_SIZE / SECTOR_SIZE)
#define EXT2_MAGIC              0xEF53u
#define EXT2_VALID_FS           0x0001u
#define EXT2_FEATURE_RESIZE_INO 0x0010u
#define EXT2_FEATURE_SPARSE_SB  0x0001u
#define EXT2_MAX_GROUPS         128u
#define EXT2_DESC_SIZE          32u
#define ZERO_CHUNK              65536u

static unsigned char s_zero[ZERO_CHUNK];
static unsigned char s_check[ZERO_CHUNK];

static uint16_t rd16(const unsigned char *p, size_t off)
{
    return (uint16_t)(p[off] | ((uint16_t)p[off + 1] << 8));
}

static uint32_t rd32(const unsigned char *p, size_t off)
{
    return (uint32_t)p[off] | ((uint32_t)p[off + 1] << 8) |
           ((uint32_t)p[off + 2] << 16) | ((uint32_t)p[off + 3] << 24);
}

static void wr16(unsigned char *p, size_t off, uint16_t v)
{
    p[off] = (unsigned char)v;
    p[off + 1] = (unsigned char)(v >> 8);
}

static void wr32(unsigned char *p, size_t off, uint32_t v)
{
    p[off] = (unsigned char)v;
    p[off + 1] = (unsigned char)(v >> 8);
    p[off + 2] = (unsigned char)(v >> 16);
    p[off + 3] = (unsigned char)(v >> 24);
}

static int rw(ext2_grow_io_fn io, void *ctx, uint64_t sector,
              uint64_t count, void *buf, int write)
{
    return io(ctx, sector, count, buf, write) < 0 ? -1 : 0;
}

static int write_checked(ext2_grow_io_fn io, void *ctx, uint64_t sector,
                         uint64_t count, const void *buf)
{
    size_t bytes = (size_t)(count * SECTOR_SIZE);
    if (bytes > sizeof(s_check) ||
        rw(io, ctx, sector, count, (void *)buf, 1) < 0 ||
        rw(io, ctx, sector, count, s_check, 0) < 0 ||
        memcmp(buf, s_check, bytes) != 0)
        return -1;
    return 0;
}

static int write_block(ext2_grow_io_fn io, void *ctx, uint32_t block,
                       const void *buf)
{
    return write_checked(io, ctx, (uint64_t)block * SECTORS_PER_BLOCK,
                         SECTORS_PER_BLOCK, buf);
}

static int read_block(ext2_grow_io_fn io, void *ctx, uint32_t block, void *buf)
{
    return rw(io, ctx, (uint64_t)block * SECTORS_PER_BLOCK,
              SECTORS_PER_BLOCK, buf, 0);
}

static int zero_blocks(ext2_grow_io_fn io, void *ctx,
                       uint32_t first, uint32_t count)
{
    uint32_t done = 0;
    while (done < count) {
        uint32_t blocks = count - done;
        if (blocks > ZERO_CHUNK / EXT2_BLOCK_SIZE)
            blocks = ZERO_CHUNK / EXT2_BLOCK_SIZE;
        if (write_checked(io, ctx,
                          (uint64_t)(first + done) * SECTORS_PER_BLOCK,
                          (uint64_t)blocks * SECTORS_PER_BLOCK, s_zero) < 0)
            return -1;
        done += blocks;
    }
    return 0;
}

static int is_power(uint32_t n, uint32_t base)
{
    if (n < 1)
        return 0;
    while (n % base == 0)
        n /= base;
    return n == 1;
}

static int has_sparse_super(uint32_t group)
{
    return group == 0 || group == 1 || is_power(group, 3) ||
           is_power(group, 5) || is_power(group, 7);
}

static void bitmap_clear_range(unsigned char *map, uint32_t first, uint32_t end)
{
    uint32_t i;
    for (i = first; i < end; i++)
        map[i >> 3] &= (unsigned char)~(1u << (i & 7));
}

static void bgd_set(unsigned char *gdt, uint32_t group,
                    uint32_t block_bitmap, uint32_t inode_bitmap,
                    uint32_t inode_table, uint16_t free_blocks,
                    uint16_t free_inodes)
{
    size_t o = (size_t)group * EXT2_DESC_SIZE;
    memset(gdt + o, 0, EXT2_DESC_SIZE);
    wr32(gdt, o + 0, block_bitmap);
    wr32(gdt, o + 4, inode_bitmap);
    wr32(gdt, o + 8, inode_table);
    wr16(gdt, o + 12, free_blocks);
    wr16(gdt, o + 14, free_inodes);
}

static int fail(const char **out, const char *msg)
{
    if (out)
        *out = msg;
    return -1;
}

int ext2_grow(ext2_grow_io_fn io, void *ctx, uint64_t target_sectors,
              uint64_t *grown_sectors, const char **error_out)
{
    unsigned char sb[1024], gdt[EXT2_BLOCK_SIZE], block[EXT2_BLOCK_SIZE];
    unsigned char bitmap[EXT2_BLOCK_SIZE];
    uint32_t old_blocks, blocks_per_group, inodes_per_group, inode_size;
    uint32_t old_groups, groups, target_blocks, inode_table_blocks;
    uint32_t reserved_gdt, compat, ro_compat, g, backup_count = 0;
    uint64_t free_blocks;

    if (error_out) *error_out = NULL;
    if (grown_sectors) *grown_sectors = 0;
    if (!io || target_sectors < SECTORS_PER_BLOCK ||
        rw(io, ctx, 2, 2, sb, 0) < 0)
        return fail(error_out, "cannot read ext2 superblock");

    if (rd16(sb, 56) != EXT2_MAGIC || rd32(sb, 24) != 2 ||
        rd32(sb, 20) != 0)
        return fail(error_out, "root filesystem is not supported 4 KiB ext2");

    old_blocks = rd32(sb, 4);
    blocks_per_group = rd32(sb, 32);
    inodes_per_group = rd32(sb, 40);
    inode_size = rd16(sb, 88);
    reserved_gdt = rd16(sb, 206);
    compat = rd32(sb, 92);
    ro_compat = rd32(sb, 100);
    if (old_blocks == 0 || blocks_per_group == 0 ||
        blocks_per_group > EXT2_BLOCK_SIZE * 8u ||
        inodes_per_group == 0 || inodes_per_group > 65535u ||
        inode_size < 128 || inode_size > EXT2_BLOCK_SIZE ||
        (inode_size & (inode_size - 1)) != 0 || reserved_gdt > 64 ||
        !(compat & EXT2_FEATURE_RESIZE_INO) ||
        !(ro_compat & EXT2_FEATURE_SPARSE_SB))
        return fail(error_out, "root filesystem has unsupported resize geometry");

    old_groups = (old_blocks + blocks_per_group - 1) / blocks_per_group;
    if (old_groups != 1)
        return fail(error_out, "root filesystem is not the expected single-group image");

    {
        uint64_t partition_blocks = target_sectors / SECTORS_PER_BLOCK;
        uint64_t max_blocks = (uint64_t)EXT2_MAX_GROUPS * blocks_per_group;
        if (partition_blocks > max_blocks)
            partition_blocks = max_blocks;
        if (partition_blocks > UINT32_MAX)
            partition_blocks = UINT32_MAX;
        target_blocks = (uint32_t)partition_blocks;
    }
    if (target_blocks <= old_blocks) {
        if (grown_sectors) *grown_sectors = (uint64_t)old_blocks * SECTORS_PER_BLOCK;
        return 0;
    }

    groups = (target_blocks + blocks_per_group - 1) / blocks_per_group;
    if (groups > EXT2_MAX_GROUPS || groups * EXT2_DESC_SIZE > EXT2_BLOCK_SIZE)
        return fail(error_out, "root filesystem exceeds Aegis group-table limit");
    inode_table_blocks = (inodes_per_group * inode_size + EXT2_BLOCK_SIZE - 1) /
                         EXT2_BLOCK_SIZE;

    /* A tiny final group cannot hold its own metadata; omit it rather than
     * creating a filesystem resize2fs/e2fsck would reject. */
    if (groups > 1) {
        uint32_t last_valid = target_blocks - (groups - 1) * blocks_per_group;
        uint32_t last_prefix = 2 + inode_table_blocks +
            (has_sparse_super(groups - 1) ? 2 + reserved_gdt : 0);
        if (last_valid <= last_prefix) {
            groups--;
            target_blocks = groups * blocks_per_group;
        }
    }

    if (read_block(io, ctx, 1, gdt) < 0)
        return fail(error_out, "cannot read ext2 group descriptors");

    /* Expand group 0 to its full bitmap span. Bits beyond the old filesystem
     * were allocated padding; they become ordinary free blocks. */
    {
        uint32_t bb = rd32(gdt, 0);
        uint32_t group0_end = groups == 1 ? target_blocks : blocks_per_group;
        uint32_t added = group0_end - old_blocks;
        if (bb == 0 || read_block(io, ctx, bb, bitmap) < 0)
            return fail(error_out, "cannot read ext2 block bitmap");
        bitmap_clear_range(bitmap, old_blocks, group0_end);
        if (write_block(io, ctx, bb, bitmap) < 0)
            return fail(error_out, "cannot expand ext2 group zero bitmap");
        wr16(gdt, 12, (uint16_t)(rd16(gdt, 12) + added));
        free_blocks = (uint64_t)rd32(sb, 12) + added;
    }

    /* Describe and initialize every new group. All metadata is written and
     * read back before the primary superblock advertises the larger size. */
    for (g = 1; g < groups; g++) {
        uint32_t start = g * blocks_per_group;
        uint32_t valid = target_blocks - start;
        uint32_t prefix, bb, ib, it, free;
        int backup = has_sparse_super(g);
        if (valid > blocks_per_group) valid = blocks_per_group;
        prefix = backup ? 2 + reserved_gdt : 0; /* super + one GDT + reserve */
        bb = start + prefix;
        ib = bb + 1;
        it = ib + 1;
        prefix += 2 + inode_table_blocks;
        if (valid <= prefix)
            return fail(error_out, "target ext2 group is too small for metadata");
        free = valid - prefix;
        bgd_set(gdt, g, bb, ib, it, (uint16_t)free,
                (uint16_t)inodes_per_group);
        free_blocks += free;

        memset(bitmap, 0xff, sizeof(bitmap));
        bitmap_clear_range(bitmap, prefix, valid);
        if (write_block(io, ctx, bb, bitmap) < 0)
            return fail(error_out, "cannot write ext2 block bitmap");

        memset(bitmap, 0xff, sizeof(bitmap));
        bitmap_clear_range(bitmap, 0, inodes_per_group);
        if (write_block(io, ctx, ib, bitmap) < 0)
            return fail(error_out, "cannot write ext2 inode bitmap");
        if (zero_blocks(io, ctx, it, inode_table_blocks) < 0)
            return fail(error_out, "cannot initialize ext2 inode table");
        if (backup) backup_count++;
    }

    wr32(sb, 0, groups * inodes_per_group);
    wr32(sb, 4, target_blocks);
    wr32(sb, 8, target_blocks / 20u); /* preserve mke2fs's 5% reserve policy */
    wr32(sb, 12, (uint32_t)free_blocks);
    wr32(sb, 16, rd32(sb, 16) + (groups - 1) * inodes_per_group);
    wr16(sb, 58, (uint16_t)(rd16(sb, 58) | EXT2_VALID_FS));

    /* Link every sparse-super reserved GDT block into resize inode 7, exactly
     * as resize2fs does. The inode's double-indirect block points at each
     * primary reserved GDT block; those blocks point at their backup copies. */
    if (reserved_gdt != 0) {
        uint32_t inode_table = rd32(gdt, 8);
        uint32_t inode_block = inode_table + ((7u - 1u) * inode_size) / EXT2_BLOCK_SIZE;
        uint32_t inode_off = ((7u - 1u) * inode_size) % EXT2_BLOCK_SIZE;
        uint32_t dind, r;
        unsigned char refs[EXT2_BLOCK_SIZE];
        if (read_block(io, ctx, inode_block, block) < 0)
            return fail(error_out, "cannot read ext2 resize inode");
        dind = rd32(block, inode_off + 40 + 13 * 4);
        if (dind == 0 || read_block(io, ctx, dind, bitmap) < 0)
            return fail(error_out, "invalid ext2 resize inode");
        for (r = 0; r < reserved_gdt; r++) {
            uint32_t primary = rd32(bitmap, (r + 1u) * 4u);
            uint32_t idx = 0;
            if (primary == 0 || read_block(io, ctx, primary, refs) < 0)
                return fail(error_out, "invalid ext2 reserved GDT block");
            memset(refs, 0, sizeof(refs));
            for (g = 1; g < groups; g++) {
                if (has_sparse_super(g)) {
                    uint32_t backup = g * blocks_per_group + 2 + r;
                    wr32(refs, (size_t)idx++ * 4u, backup);
                }
            }
            if (write_block(io, ctx, primary, refs) < 0)
                return fail(error_out, "cannot update ext2 resize inode map");
        }
        wr32(block, inode_off + 28,
             rd32(block, inode_off + 28) +
             backup_count * reserved_gdt * SECTORS_PER_BLOCK);
        if (write_block(io, ctx, inode_block, block) < 0)
            return fail(error_out, "cannot update ext2 resize inode");
    }

    /* Sparse backup copies. Reserved GDT blocks are owned by resize inode 7
     * and deliberately zero until a future descriptor-table expansion. */
    for (g = 1; g < groups; g++) {
        uint32_t start, r;
        if (!has_sparse_super(g)) continue;
        start = g * blocks_per_group;
        memset(block, 0, sizeof(block));
        memcpy(block, sb, sizeof(sb));
        wr16(block, 90, (uint16_t)g);
        if (write_block(io, ctx, start, block) < 0 ||
            write_block(io, ctx, start + 1, gdt) < 0)
            return fail(error_out, "cannot write ext2 backup metadata");
        for (r = 0; r < reserved_gdt; r++)
            if (zero_blocks(io, ctx, start + 2 + r, 1) < 0)
                return fail(error_out, "cannot write ext2 reserved GDT backup");
    }

    if (write_block(io, ctx, 1, gdt) < 0)
        return fail(error_out, "cannot commit ext2 group descriptors");
    /* Commit point: until this succeeds, the target still advertises the
     * original one-group filesystem and ignores all newly written metadata. */
    if (write_checked(io, ctx, 2, 2, sb) < 0)
        return fail(error_out, "cannot commit expanded ext2 superblock");

    if (grown_sectors)
        *grown_sectors = (uint64_t)target_blocks * SECTORS_PER_BLOCK;
    return 0;
}
