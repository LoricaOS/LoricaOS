/* Host wrapper around the recovery environment's real repair engine. */
#define _XOPEN_SOURCE 700
#include "../user/lib/libinstall/ext2_repair.h"
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

static int file_io(void *ctx, uint64_t sector, uint64_t count,
                   void *buf, int write)
{
    int fd = *(int *)ctx;
    size_t bytes = (size_t)(count * 512u), done = 0;
    off_t off = (off_t)(sector * 512u);
    while (done < bytes) {
        ssize_t n = write
            ? pwrite(fd, (const unsigned char *)buf + done, bytes - done, off + done)
            : pread(fd, (unsigned char *)buf + done, bytes - done, off + done);
        if (n <= 0) return -1;
        done += (size_t)n;
    }
    return 0;
}

int main(int argc, char **argv)
{
    struct stat st; ext2_repair_report_t report; const char *error = NULL;
    int fd, rc, repair = argc == 3 && argv[1][0] == '-' && argv[1][1] == 'y';
    const char *path = repair ? argv[2] : argc == 2 ? argv[1] : NULL;
    if (!path) { fprintf(stderr, "usage: %s [-y] <ext2-image>\n", argv[0]); return 2; }
    fd = open(path, repair ? O_RDWR : O_RDONLY);
    if (fd < 0 || fstat(fd, &st) < 0) { perror(path); return 1; }
    rc = ext2_repair(file_io, &fd, (uint64_t)st.st_size / 512u,
                     repair, &report, &error);
    if (repair && rc >= 0) fsync(fd);
    close(fd);
    if (rc < 0) { fprintf(stderr, "ext2-repair: UNSAFE: %s\n",
                          error ? error : "failed"); return 1; }
    printf("ext2-repair: %s blocks=%u inodes=%u directories=%u fixes=%u/%u/%u\n",
           rc ? (repair ? "REPAIRED" : "NEEDS-REPAIR") : "CLEAN",
           report.blocks, report.inodes, report.directories,
           report.block_bits_fixed, report.inode_bits_fixed, report.counters_fixed);
    return rc && !repair ? 3 : 0;
}
