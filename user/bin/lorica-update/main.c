/* lorica-update — signed package upgrade + power-safe Aegis kernel slot swap. */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef HOST_SELFTEST
#include <sys/wait.h>
#include "libinstall.h"
#include "syscalls.h"
#endif

#define STAGED_KERNEL "/etc/aegis/update/aegis.elf"
#define UPDATE_PENDING "/var/lib/lorica-update/pending"
#define ESP_OFFSET    (1ULL * 1024 * 1024)
#define ESP_SECTORS   16384U
#define KERNEL_SLOT   (3U * 1024 * 1024)
#define MAX_DEVS      16

typedef int (*disk_io_fn)(void *, uint64_t, void *, size_t, int);

static uint16_t le16(const unsigned char *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t le32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

typedef struct {
    disk_io_fn io;
    void *ctx;
    uint32_t bytes_per_sector, sectors_per_cluster;
    uint32_t fat_sector, root_sector, root_sectors, data_sector;
    uint32_t cluster_count;
} fat16_t;

typedef struct {
    uint16_t first_cluster;
    uint32_t size;
} fat_file_t;

static int fat_open(fat16_t *fs, disk_io_fn io, void *ctx)
{
    unsigned char b[512];
    uint32_t reserved, fats, fat_sectors, root_entries, total, data;

    memset(fs, 0, sizeof(*fs));
    fs->io = io;
    fs->ctx = ctx;
    if (io(ctx, ESP_OFFSET, b, sizeof(b), 0) != 0 ||
        b[510] != 0x55 || b[511] != 0xaa)
        return -1;
    fs->bytes_per_sector = le16(b + 11);
    fs->sectors_per_cluster = b[13];
    reserved = le16(b + 14);
    fats = b[16];
    root_entries = le16(b + 17);
    total = le16(b + 19) ? le16(b + 19) : le32(b + 32);
    fat_sectors = le16(b + 22);
    if (fs->bytes_per_sector != 512 || fs->sectors_per_cluster == 0 ||
        fats == 0 || fat_sectors == 0 || total != ESP_SECTORS)
        return -1;
    fs->fat_sector = reserved;
    fs->root_sectors = (root_entries * 32U + 511U) / 512U;
    fs->root_sector = reserved + fats * fat_sectors;
    fs->data_sector = fs->root_sector + fs->root_sectors;
    if (fs->data_sector >= total)
        return -1;
    data = total - fs->data_sector;
    fs->cluster_count = data / fs->sectors_per_cluster;
    return (fs->cluster_count >= 4085 && fs->cluster_count < 65525) ? 0 : -1;
}

static uint64_t sector_off(const fat16_t *fs, uint32_t sector)
{
    return ESP_OFFSET + (uint64_t)sector * fs->bytes_per_sector;
}

static int fat_next(const fat16_t *fs, uint16_t cluster, uint16_t *next)
{
    unsigned char e[2];
    uint64_t off = sector_off(fs, fs->fat_sector) + (uint64_t)cluster * 2;
    if (fs->io(fs->ctx, off, e, 2, 0) != 0)
        return -1;
    *next = le16(e);
    return 0;
}

static uint64_t cluster_off(const fat16_t *fs, uint16_t cluster)
{
    uint32_t sector = fs->data_sector +
        ((uint32_t)cluster - 2U) * fs->sectors_per_cluster;
    return sector_off(fs, sector);
}

static int dir_find(const fat16_t *fs, uint16_t dir_cluster,
                    const char name[11], fat_file_t *out)
{
    unsigned char block[512];
    uint32_t blocks, i, seen = 0;
    uint16_t cluster = dir_cluster, next;

    if (dir_cluster == 0) {
        blocks = fs->root_sectors;
    } else {
        blocks = fs->sectors_per_cluster;
        if (cluster < 2 || cluster >= fs->cluster_count + 2)
            return -1;
    }
    for (;;) {
        for (i = 0; i < blocks; i++) {
            uint64_t off = dir_cluster == 0
                ? sector_off(fs, fs->root_sector + i)
                : cluster_off(fs, cluster) + (uint64_t)i * 512;
            unsigned j;
            if (fs->io(fs->ctx, off, block, sizeof(block), 0) != 0)
                return -1;
            for (j = 0; j < sizeof(block); j += 32) {
                unsigned char *e = block + j;
                if (e[0] == 0x00)
                    return 0;
                if (e[0] == 0xe5 || e[11] == 0x0f)
                    continue;
                if (memcmp(e, name, 11) == 0) {
                    out->first_cluster = le16(e + 26);
                    out->size = le32(e + 28);
                    return 1;
                }
            }
        }
        if (dir_cluster == 0)
            return 0;
        if (++seen > fs->cluster_count || fat_next(fs, cluster, &next) != 0)
            return -1;
        if (next >= 0xfff8)
            return 0;
        if (next < 2 || next >= fs->cluster_count + 2)
            return -1;
        cluster = next;
    }
}

static int file_io(const fat16_t *fs, const fat_file_t *f,
                   unsigned char *buf, int write)
{
    uint32_t cluster_bytes = fs->sectors_per_cluster * 512U;
    uint32_t done = 0, seen = 0;
    uint16_t cluster = f->first_cluster, next;

    while (done < f->size) {
        uint32_t n = f->size - done;
        if (cluster < 2 || cluster >= fs->cluster_count + 2 ||
            ++seen > fs->cluster_count)
            return -1;
        if (n > cluster_bytes)
            n = cluster_bytes;
        if (fs->io(fs->ctx, cluster_off(fs, cluster), buf + done, n, write) != 0)
            return -1;
        done += n;
        if (done == f->size)
            return 0;
        if (fat_next(fs, cluster, &next) != 0 || next >= 0xfff8)
            return -1;
        cluster = next;
    }
    return 0;
}

static int mark_chain(const fat16_t *fs, const fat_file_t *f,
                      unsigned char *used)
{
    uint32_t bytes = fs->sectors_per_cluster * 512U;
    uint32_t need = (f->size + bytes - 1) / bytes, i;
    uint16_t cluster = f->first_cluster, next;
    for (i = 0; i < need; i++) {
        if (cluster < 2 || cluster >= fs->cluster_count + 2 || used[cluster])
            return -1;
        used[cluster] = 1;
        if (i + 1 < need) {
            if (fat_next(fs, cluster, &next) != 0 || next >= 0xfff8)
                return -1;
            cluster = next;
        }
    }
    return 0;
}

static int find_slots(fat16_t *fs, fat_file_t *current, fat_file_t *old)
{
    static const char boot_name[11] = "BOOT       ";
    static const char kernel_name[11] = "AEGIS   ELF";
    static const char old_name[11] = "AEGIS   OLD";
    fat_file_t boot;

    if (dir_find(fs, 0, boot_name, &boot) != 1 ||
        dir_find(fs, boot.first_cluster, kernel_name, current) != 1 ||
        dir_find(fs, boot.first_cluster, old_name, old) != 1)
        return -1;
    if (current->size != KERNEL_SLOT || old->size != KERNEL_SLOT)
        return -1;
    {
        unsigned char *used = calloc(fs->cluster_count + 2U, 1);
        int ok = used && mark_chain(fs, current, used) == 0 &&
                 mark_chain(fs, old, used) == 0;
        free(used);
        return ok ? 0 : -1;
    }
}

#ifndef HOST_SELFTEST
typedef struct { char name[16]; uint32_t block_size; } raw_disk_t;

static int raw_io(void *opaque, uint64_t off, void *buf, size_t len, int write)
{
    raw_disk_t *d = opaque;
    unsigned char block[4096];
    unsigned char *p = buf;
    while (len) {
        uint64_t lba = off / d->block_size;
        size_t at = (size_t)(off % d->block_size);
        size_t n = d->block_size - at;
        if (n > len) n = len;
        if ((at || n != d->block_size || !write) &&
            li_blkdev_io(d->name, lba, 1, block, 0) < 0)
            return -1;
        if (write) {
            memcpy(block + at, p, n);
            if (li_blkdev_io(d->name, lba, 1, block, 1) < 0)
                return -1;
        } else {
            memcpy(p, block + at, n);
        }
        off += n; p += n; len -= n;
    }
    return 0;
}

static int run(char *const argv[])
{
    pid_t pid = fork();
    int status;
    if (pid == 0) {
        execv(argv[0], argv);
        _exit(127);
    }
    if (pid < 0 || waitpid(pid, &status, 0) < 0)
        return -1;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

static int choose_disk(raw_disk_t *disk, const char *requested)
{
    install_blkdev_t devs[MAX_DEVS];
    int n = install_list_blkdevs(devs, MAX_DEVS), i, found = 0;
    if (n < 0)
        return -2;
    for (i = 0; i < n; i++) {
        char *p = strrchr(devs[i].name, 'p');
        if (strncmp(devs[i].name, "ramdisk", 7) == 0 ||
            (p && p[1] >= '0' && p[1] <= '9') ||
            (requested && strcmp(requested, devs[i].name) != 0) ||
            !install_disk_has_aegis(devs[i].name))
            continue;
        if (devs[i].block_size != 512 && devs[i].block_size != 4096)
            continue;
        memcpy(disk->name, devs[i].name, sizeof(disk->name));
        disk->name[sizeof(disk->name) - 1] = '\0';
        disk->block_size = devs[i].block_size;
        found++;
    }
    return found == 1 ? 0 : -1;
}

static int read_staged(unsigned char **out)
{
    int fd = open(STAGED_KERNEL, O_RDONLY);
    struct stat st;
    unsigned char *buf;
    size_t got = 0;
    if (fd < 0 || fstat(fd, &st) != 0 || st.st_size < 64 ||
        st.st_size > KERNEL_SLOT) {
        if (fd >= 0) close(fd);
        return -1;
    }
    buf = calloc(1, KERNEL_SLOT);
    if (!buf) { close(fd); return -1; }
    while (got < (size_t)st.st_size) {
        ssize_t n = read(fd, buf + got, (size_t)st.st_size - got);
        if (n <= 0) { free(buf); close(fd); return -1; }
        got += (size_t)n;
    }
    close(fd);
    if (memcmp(buf, "\177ELF", 4) != 0 || buf[4] != 2 ||
#if defined(__x86_64__)
        le16(buf + 18) != 62 ||
#elif defined(__aarch64__)
        le16(buf + 18) != 183 ||
#endif
        buf[5] != 1) {
        free(buf);
        return -1;
    }
    *out = buf;
    return 0;
}

int main(int argc, char **argv)
{
    raw_disk_t disk;
    fat16_t fs;
    fat_file_t current_slot, old_slot;
    unsigned char *staged = NULL, *current = NULL, *verify = NULL;
    char *sync_argv[] = { "/bin/herald", "sync", NULL };
    char *upgrade_argv[] = { "/bin/herald", "upgrade", NULL };
    char *finish_argv[] = { "/bin/herald", "recover", "complete", NULL };
    int rc = 1, pending;

    if (argc > 2) {
        fprintf(stderr, "usage: lorica-update [disk]\n");
        return 2;
    }
    if (choose_disk(&disk, argc == 2 ? argv[1] : NULL) == -2) {
        printf("lorica-update: administrator authentication required\n");
        if (install_elevate(NULL) != 0) {
            fprintf(stderr, "lorica-update: authentication failed\n");
            return 1;
        }
    }
    if (choose_disk(&disk, argc == 2 ? argv[1] : NULL) != 0) {
        fprintf(stderr, "lorica-update: specify the single installed raw disk\n");
        return 1;
    }
    pending = access(UPDATE_PENDING, F_OK) == 0;
    if (!pending &&
        (run(sync_argv) != 0 || run(upgrade_argv) != 0)) {
        fprintf(stderr, "lorica-update: package upgrade failed; kernel unchanged\n");
        return 1;
    }
    pending = access(UPDATE_PENDING, F_OK) == 0;
    if (read_staged(&staged) != 0) {
        fprintf(stderr, "lorica-update: signed release staged no valid kernel\n");
        return 1;
    }
    current = malloc(KERNEL_SLOT);
    verify = malloc(KERNEL_SLOT);
    if (!current || !verify || fat_open(&fs, raw_io, &disk) != 0 ||
        find_slots(&fs, &current_slot, &old_slot) != 0 ||
        file_io(&fs, &current_slot, current, 0) != 0) {
        fprintf(stderr, "lorica-update: invalid installed ESP or kernel slots\n");
        goto out;
    }
    if (memcmp(current, staged, KERNEL_SLOT) == 0) {
        rc = !pending || run(finish_argv) == 0 ? 0 : 1;
        if (rc == 0) printf("lorica-update: packages and kernel are up to date\n");
        goto out;
    }
    if (file_io(&fs, &old_slot, current, 1) != 0 ||
        file_io(&fs, &old_slot, verify, 0) != 0 ||
        memcmp(current, verify, KERNEL_SLOT) != 0) {
        fprintf(stderr, "lorica-update: could not preserve previous kernel; primary unchanged\n");
        goto out;
    }
    if (file_io(&fs, &current_slot, staged, 1) != 0 ||
        file_io(&fs, &current_slot, verify, 0) != 0 ||
        memcmp(staged, verify, KERNEL_SLOT) != 0) {
        fprintf(stderr, "lorica-update: new kernel write failed; select Previous kernel at boot\n");
        goto out;
    }
    sync();
    if (pending && run(finish_argv) != 0) {
        fprintf(stderr, "lorica-update: kernel installed, but update cleanup "
                        "failed; rerun lorica-update\n");
        goto out;
    }
    printf("lorica-update: update complete; reboot to use the new kernel\n");
    rc = 0;
out:
    free(staged); free(current); free(verify);
    return rc;
}
#else
static int image_io(void *opaque, uint64_t off, void *buf, size_t len, int write)
{
    int fd = *(int *)opaque;
    ssize_t n = write ? pwrite(fd, buf, len, (off_t)off)
                      : pread(fd, buf, len, (off_t)off);
    return n == (ssize_t)len ? 0 : -1;
}

int main(int argc, char **argv)
{
    int fd;
    fat16_t fs;
    fat_file_t current, old;
    unsigned char *a = NULL, *b = NULL, *check = NULL;
    int rc = 1;
    if (argc != 2 || (fd = open(argv[1], O_RDWR)) < 0 ||
        fat_open(&fs, image_io, &fd) != 0 ||
        find_slots(&fs, &current, &old) != 0) {
        fprintf(stderr, "lorica-update selftest: invalid ESP image\n");
        return 1;
    }
    a = malloc(KERNEL_SLOT); b = calloc(1, KERNEL_SLOT);
    check = malloc(KERNEL_SLOT);
    if (!a || !b || !check || file_io(&fs, &current, a, 0) != 0 ||
        memcmp(a, "\177ELF", 4) != 0)
        goto out;
    memcpy(b, "\177ELF", 4); b[4] = 2; b[63] = 0xa5;
    if (file_io(&fs, &old, a, 1) != 0 || file_io(&fs, &old, check, 0) != 0 ||
        memcmp(a, check, KERNEL_SLOT) != 0 ||
        file_io(&fs, &current, b, 1) != 0 ||
        file_io(&fs, &current, check, 0) != 0 ||
        memcmp(b, check, KERNEL_SLOT) != 0)
        goto out;
    rc = 0;
out:
    close(fd);
    free(a); free(b); free(check);
    if (rc)
        fprintf(stderr, "lorica-update selftest: slot swap failed\n");
    else
        printf("lorica-update selftest: FAT16 slot swap OK\n");
    return rc;
}
#endif
