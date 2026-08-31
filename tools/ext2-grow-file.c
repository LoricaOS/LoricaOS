/* Host-side wrapper for testing the installer's real ext2 growth engine. */
#define _XOPEN_SOURCE 700
#include "../user/lib/libinstall/ext2_grow.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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
        if (n <= 0)
            return -1;
        done += (size_t)n;
    }
    return 0;
}

int main(int argc, char **argv)
{
    struct stat st;
    const char *err = NULL;
    uint64_t grown = 0;
    if (argc != 2) {
        fprintf(stderr, "usage: %s <ext2-image>\n", argv[0]);
        return 2;
    }
    int fd = open(argv[1], O_RDWR);
    if (fd < 0 || fstat(fd, &st) < 0) {
        perror(argv[1]);
        return 1;
    }
    if (ext2_grow(file_io, &fd, (uint64_t)st.st_size / 512u,
                  &grown, &err) < 0) {
        fprintf(stderr, "ext2-grow-file: %s\n", err ? err : "failed");
        close(fd);
        return 1;
    }
    if (fsync(fd) < 0) {
        perror("fsync");
        close(fd);
        return 1;
    }
    close(fd);
    printf("grew filesystem to %llu MiB\n",
           (unsigned long long)(grown / 2048u));
    return 0;
}
