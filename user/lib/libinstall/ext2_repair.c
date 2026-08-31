/* Conservative offline ext2 repair for LoricaOS recovery.
 *
 * Never frees an allocated inode or block: a crash can leak space, but guessing
 * that an allocation is dead risks destroying data. Referenced resources are
 * marked allocated, free counters are rebuilt, and VALID_FS is restored only
 * after geometry, directories, block ranges, and duplicate ownership pass. */
#include "ext2_repair.h"
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define BS 4096u
#define SPB 8u
#define MAGIC 0xef53u
#define VALID_FS 1u
#define ERROR_FS 2u
#define INCOMPAT_FILETYPE 2u
#define RO_COMPAT_SUPPORTED 3u /* sparse_super | large_file */
#define MAX_GROUPS 128u
#define DESC_SIZE 32u
#define S_IFMT 0170000u
#define S_IFREG 0100000u
#define S_IFDIR 0040000u
#define S_IFLNK 0120000u

typedef struct {
    ext2_repair_io_fn io;
    void *ctx;
    uint64_t sectors;
    unsigned char sb[1024], gdt[BS], block[BS], verify[BS];
    unsigned char *block_refs, *inode_alloc, *scanned;
    uint32_t blocks, inodes, bpg, ipg, groups, inode_size, first_ino;
    uint32_t inode_table_blocks;
    uint32_t directories, missing_blocks, missing_inodes, counter_mismatches;
    uint32_t dirs_per_group[MAX_GROUPS];
    const char *error;
} repair_t;

static uint16_t r16(const unsigned char *p, size_t o)
{ return (uint16_t)(p[o] | ((uint16_t)p[o + 1] << 8)); }
static uint32_t r32(const unsigned char *p, size_t o)
{ return (uint32_t)p[o] | ((uint32_t)p[o + 1] << 8) |
         ((uint32_t)p[o + 2] << 16) | ((uint32_t)p[o + 3] << 24); }
static void w16(unsigned char *p, size_t o, uint16_t v)
{ p[o] = (unsigned char)v; p[o + 1] = (unsigned char)(v >> 8); }
static void w32(unsigned char *p, size_t o, uint32_t v)
{ p[o] = (unsigned char)v; p[o + 1] = (unsigned char)(v >> 8);
  p[o + 2] = (unsigned char)(v >> 16); p[o + 3] = (unsigned char)(v >> 24); }
static int bit(const unsigned char *m, uint32_t n)
{ return (m[n >> 3] >> (n & 7)) & 1; }
static void setbit(unsigned char *m, uint32_t n)
{ m[n >> 3] |= (unsigned char)(1u << (n & 7)); }

static int fail(repair_t *r, const char *s)
{ r->error = s; return -1; }

static int rw(repair_t *r, uint64_t sector, uint64_t count,
              void *buf, int write)
{
    if (sector > r->sectors || count > r->sectors - sector ||
        r->io(r->ctx, sector, count, buf, write) < 0)
        return fail(r, write ? "filesystem write failed" : "filesystem read failed");
    return 0;
}

static int read_block(repair_t *r, uint32_t b, unsigned char *out)
{
    if (b >= r->blocks) return fail(r, "block pointer outside filesystem");
    return rw(r, (uint64_t)b * SPB, SPB, out, 0);
}

static int write_block(repair_t *r, uint32_t b, const unsigned char *in)
{
    if (rw(r, (uint64_t)b * SPB, SPB, (void *)in, 1) < 0 ||
        rw(r, (uint64_t)b * SPB, SPB, r->verify, 0) < 0 ||
        memcmp(in, r->verify, BS) != 0)
        return fail(r, "filesystem write verification failed");
    return 0;
}

static int mark_block(repair_t *r, uint32_t b)
{
    if (b >= r->blocks) return fail(r, "inode references block outside filesystem");
    if (bit(r->block_refs, b)) return fail(r, "duplicate block ownership detected");
    setbit(r->block_refs, b);
    return 0;
}

static uint32_t group_start(repair_t *r, uint32_t g) { return g * r->bpg; }
static uint32_t group_end(repair_t *r, uint32_t g)
{
    uint32_t end = group_start(r, g) + r->bpg;
    return end < r->blocks ? end : r->blocks;
}

static int mark_metadata(repair_t *r)
{
    uint32_t g;
    for (g = 0; g < r->groups; g++) {
        size_t o = (size_t)g * DESC_SIZE;
        uint32_t start = group_start(r, g), end = group_end(r, g);
        uint32_t bb = r32(r->gdt, o), ib = r32(r->gdt, o + 4);
        uint32_t it = r32(r->gdt, o + 8), b;
        uint32_t prefix_end = bb;
        if (ib < prefix_end) prefix_end = ib;
        if (it < prefix_end) prefix_end = it;
        if (bb < start || bb >= end || ib < start || ib >= end ||
            it < start || it > end || r->inode_table_blocks > end - it)
            return fail(r, "group descriptor points outside its block group");
        for (b = start; b < prefix_end; b++)
            if (mark_block(r, b) < 0) return -1;
        if (mark_block(r, bb) < 0 || mark_block(r, ib) < 0) return -1;
        for (b = 0; b < r->inode_table_blocks; b++)
            if (mark_block(r, it + b) < 0) return -1;
    }
    return 0;
}

static int read_inode(repair_t *r, uint32_t ino, unsigned char out[128])
{
    uint32_t g, idx, table, block, off;
    size_t go;
    if (ino == 0 || ino > r->inodes) return fail(r, "directory references invalid inode");
    g = (ino - 1) / r->ipg; idx = (ino - 1) % r->ipg;
    go = (size_t)g * DESC_SIZE;
    table = r32(r->gdt, go + 8);
    block = table + (idx * r->inode_size) / BS;
    off = (idx * r->inode_size) % BS;
    if (off + 128 > BS || read_block(r, block, r->block) < 0) return -1;
    memcpy(out, r->block + off, 128);
    return 0;
}

static int parse_dir_block(repair_t *r, const unsigned char *b)
{
    uint32_t off = 0;
    while (off < BS) {
        if (BS - off < 8) return fail(r, "truncated directory entry");
        uint32_t ino = r32(b, off);
        uint16_t rec = r16(b, off + 4);
        uint8_t namesz = b[off + 6];
        unsigned char inode[128];
        if (rec < 8 || (rec & 3) || rec > BS - off || namesz > rec - 8)
            return fail(r, "malformed directory entry");
        if (ino) {
            if (ino > r->inodes) return fail(r, "directory inode is out of range");
            if (!bit(r->inode_alloc, ino - 1)) {
                if (read_inode(r, ino, inode) < 0 ||
                    (r16(inode, 0) & S_IFMT) == 0)
                    return fail(r, "directory references an uninitialized inode");
                setbit(r->inode_alloc, ino - 1);
                r->missing_inodes++;
            }
        }
        off += rec;
    }
    return 0;
}

static int data_block(repair_t *r, uint32_t b, int is_dir,
                      uint32_t *dir_bytes)
{
    if (!b) {
        if (is_dir && *dir_bytes) return fail(r, "directory contains a sparse block");
        return 0;
    }
    if (mark_block(r, b) < 0) return -1;
    if (is_dir && *dir_bytes) {
        if (read_block(r, b, r->block) < 0 || parse_dir_block(r, r->block) < 0)
            return -1;
        *dir_bytes = *dir_bytes > BS ? *dir_bytes - BS : 0;
    }
    return 0;
}

static int indirect(repair_t *r, uint32_t b, int depth, int is_dir,
                    uint32_t *dir_bytes)
{
    unsigned char pointers[BS];
    uint32_t i;
    if (!b) return 0;
    if (depth < 1 || depth > 2 || mark_block(r, b) < 0 ||
        read_block(r, b, pointers) < 0) return -1;
    for (i = 0; i < BS / 4; i++) {
        uint32_t p = r32(pointers, i * 4);
        if (!p) continue;
        if (depth == 1) {
            if (data_block(r, p, is_dir, dir_bytes) < 0) return -1;
        } else if (indirect(r, p, depth - 1, is_dir, dir_bytes) < 0) {
            return -1;
        }
    }
    return 0;
}

static int scan_inode(repair_t *r, uint32_t ino)
{
    unsigned char in[128];
    uint16_t mode;
    uint32_t i, size, dir_bytes = 0;
    if (read_inode(r, ino, in) < 0) return -1;
    mode = r16(in, 0) & S_IFMT;
    if (mode == 0) {
        if (ino < r->first_ino && ino != 2) return 0;
        return fail(r, "allocated inode has no file type");
    }
    if (ino < r->first_ino && ino != 2) return 0; /* reserved/resize inodes */
    if (mode != S_IFREG && mode != S_IFDIR && mode != S_IFLNK) return 0;
    if (mode == S_IFLNK && r32(in, 28) == 0) return 0; /* fast symlink */
    size = r32(in, 4);
    if (mode == S_IFDIR) {
        dir_bytes = size;
        r->directories++;
        r->dirs_per_group[(ino - 1) / r->ipg]++;
    }
    for (i = 0; i < 12; i++)
        if (data_block(r, r32(in, 40 + i * 4), mode == S_IFDIR, &dir_bytes) < 0)
            return -1;
    if (indirect(r, r32(in, 88), 1, mode == S_IFDIR, &dir_bytes) < 0 ||
        indirect(r, r32(in, 92), 2, mode == S_IFDIR, &dir_bytes) < 0)
        return -1;
    if (r32(in, 96) != 0) return fail(r, "triple-indirect files require full e2fsck");
    if (mode == S_IFDIR && dir_bytes != 0)
        return fail(r, "directory size exceeds its data blocks");
    return 0;
}

static int load_inode_maps(repair_t *r)
{
    uint32_t g;
    for (g = 0; g < r->groups; g++) {
        uint32_t ib = r32(r->gdt, (size_t)g * DESC_SIZE + 4);
        uint32_t count = r->inodes - g * r->ipg;
        uint32_t i;
        if (count > r->ipg) count = r->ipg;
        if (read_block(r, ib, r->block) < 0) return -1;
        for (i = 0; i < count; i++)
            if (bit(r->block, i)) setbit(r->inode_alloc, g * r->ipg + i);
    }
    return 0;
}

static int scan_all(repair_t *r)
{
    int progress;
    do {
        uint32_t i;
        progress = 0;
        for (i = 0; i < r->inodes; i++) {
            if (!bit(r->inode_alloc, i) || bit(r->scanned, i)) continue;
            setbit(r->scanned, i); progress = 1;
            if (scan_inode(r, i + 1) < 0) return -1;
        }
    } while (progress);
    return 0;
}

static int reconcile(repair_t *r, int repair, uint32_t *free_b_out,
                     uint32_t *free_i_out)
{
    uint32_t g, total_fb = 0, total_fi = 0;
    for (g = 0; g < r->groups; g++) {
        size_t o = (size_t)g * DESC_SIZE;
        uint32_t bb = r32(r->gdt, o), ib = r32(r->gdt, o + 4);
        uint32_t start = group_start(r, g), end = group_end(r, g);
        uint32_t valid_b = end - start, valid_i = r->inodes - g * r->ipg;
        uint32_t i, fb = 0, fi = 0; int changed = 0;
        if (valid_i > r->ipg) valid_i = r->ipg;
        if (read_block(r, bb, r->block) < 0) return -1;
        for (i = 0; i < valid_b; i++) {
            if (bit(r->block_refs, start + i) && !bit(r->block, i)) {
                setbit(r->block, i); r->missing_blocks++; changed = 1;
            }
            if (!bit(r->block, i)) fb++;
        }
        if (changed && repair && write_block(r, bb, r->block) < 0) return -1;
        changed = 0;
        if (read_block(r, ib, r->block) < 0) return -1;
        for (i = 0; i < valid_i; i++) {
            if (bit(r->inode_alloc, g * r->ipg + i) && !bit(r->block, i)) {
                setbit(r->block, i); changed = 1;
            }
            if (!bit(r->block, i)) fi++;
        }
        if (changed && repair && write_block(r, ib, r->block) < 0) return -1;
        if (r16(r->gdt, o + 12) != fb || r16(r->gdt, o + 14) != fi ||
            r16(r->gdt, o + 16) != r->dirs_per_group[g]) {
            r->counter_mismatches++;
            if (repair) {
                w16(r->gdt, o + 12, (uint16_t)fb);
                w16(r->gdt, o + 14, (uint16_t)fi);
                w16(r->gdt, o + 16, (uint16_t)r->dirs_per_group[g]);
            }
        }
        total_fb += fb; total_fi += fi;
    }
    *free_b_out = total_fb; *free_i_out = total_fi;
    return 0;
}

int ext2_repair(ext2_repair_io_fn io, void *ctx, uint64_t sectors,
                int repair, ext2_repair_report_t *out, const char **error_out)
{
    repair_t r; uint32_t free_b = 0, free_i = 0; int needs;
    memset(&r, 0, sizeof(r)); r.io = io; r.ctx = ctx; r.sectors = sectors;
    if (error_out) *error_out = NULL;
    if (out) memset(out, 0, sizeof(*out));
    if (!io || rw(&r, 2, 2, r.sb, 0) < 0 || r16(r.sb, 56) != MAGIC ||
        r32(r.sb, 24) != 2 || r32(r.sb, 20) != 0 ||
        (r32(r.sb, 96) & ~INCOMPAT_FILETYPE) != 0 ||
        (r32(r.sb, 100) & ~RO_COMPAT_SUPPORTED) != 0 ||
        (r32(r.sb, 92) & 4u) != 0) {
        fail(&r, "not a supported LoricaOS 4 KiB ext2 filesystem"); goto bad;
    }
    r.inodes = r32(r.sb, 0); r.blocks = r32(r.sb, 4);
    r.bpg = r32(r.sb, 32); r.ipg = r32(r.sb, 40);
    r.inode_size = r16(r.sb, 88); r.first_ino = r32(r.sb, 84);
    if (!r.blocks || !r.inodes || !r.bpg || !r.ipg || r.bpg > BS * 8u ||
        r.ipg > BS * 8u || r.inode_size < 128 || r.inode_size > BS ||
        (r.inode_size & (r.inode_size - 1)) ||
        r.first_ino < 2 || r.first_ino > r.inodes ||
        (uint64_t)r.blocks * SPB > sectors) {
        fail(&r, "invalid ext2 geometry"); goto bad;
    }
    r.groups = (r.blocks + r.bpg - 1) / r.bpg;
    r.inode_table_blocks = (r.ipg * r.inode_size + BS - 1) / BS;
    if (!r.groups || r.groups > MAX_GROUPS || r.groups * DESC_SIZE > BS ||
        r.inodes > r.groups * r.ipg ||
        r.inodes <= (r.groups - 1) * r.ipg || read_block(&r, 1, r.gdt) < 0) {
        fail(&r, "ext2 group table is unsupported"); goto bad;
    }
    r.block_refs = calloc((r.blocks + 7u) / 8u, 1);
    r.inode_alloc = calloc((r.inodes + 7u) / 8u, 1);
    r.scanned = calloc((r.inodes + 7u) / 8u, 1);
    if (!r.block_refs || !r.inode_alloc || !r.scanned) {
        fail(&r, "not enough memory to check filesystem"); goto bad;
    }
    if (mark_metadata(&r) < 0 || load_inode_maps(&r) < 0 || scan_all(&r) < 0 ||
        reconcile(&r, 0, &free_b, &free_i) < 0) goto bad;
    if (r32(r.sb, 12) != free_b || r32(r.sb, 16) != free_i)
        r.counter_mismatches++;
    needs = !(r16(r.sb, 58) & VALID_FS) || (r16(r.sb, 58) & ERROR_FS) ||
            r.missing_blocks || r.missing_inodes || r.counter_mismatches;
    if (repair && needs) {
        /* Keep a crash during bitmap/GDT repair visible to the next boot. */
        w16(r.sb, 58, (uint16_t)(r16(r.sb, 58) & ~VALID_FS));
        if (rw(&r, 2, 2, r.sb, 1) < 0 || rw(&r, 2, 2, r.verify, 0) < 0 ||
            memcmp(r.sb, r.verify, 1024) != 0) {
            fail(&r, "could not mark filesystem repair in progress"); goto bad;
        }
        r.missing_blocks = 0; r.counter_mismatches = 0;
        if (reconcile(&r, 1, &free_b, &free_i) < 0) goto bad;
        if (r32(r.sb, 12) != free_b || r32(r.sb, 16) != free_i)
            r.counter_mismatches++;
        if (write_block(&r, 1, r.gdt) < 0) goto bad;
        w32(r.sb, 12, free_b); w32(r.sb, 16, free_i);
        w16(r.sb, 58, (uint16_t)((r16(r.sb, 58) | VALID_FS) & ~ERROR_FS));
        if (rw(&r, 2, 2, r.sb, 1) < 0 || rw(&r, 2, 2, r.verify, 0) < 0 ||
            memcmp(r.sb, r.verify, 1024) != 0) {
            fail(&r, "superblock write verification failed"); goto bad;
        }
    }
    if (out) {
        out->blocks = r.blocks - free_b; out->inodes = r.inodes - free_i;
        out->directories = r.directories; out->block_bits_fixed = r.missing_blocks;
        out->inode_bits_fixed = r.missing_inodes;
        out->counters_fixed = r.counter_mismatches;
        out->was_clean = !needs;
    }
    free(r.block_refs); free(r.inode_alloc); free(r.scanned);
    return needs ? 1 : 0;
bad:
    if (error_out) *error_out = r.error;
    free(r.block_refs); free(r.inode_alloc); free(r.scanned);
    return -1;
}
