/* syscalls.h — private: Aegis syscall wrappers used by libinstall.
 * Not part of the public libinstall.h API. */
#ifndef LIBINSTALL_SYSCALLS_H
#define LIBINSTALL_SYSCALLS_H

#include "libinstall.h"
#include <sys/syscall.h>
#include <unistd.h>

static inline long li_blkdev_list(install_blkdev_t *buf, unsigned long bufsize)
{
    return syscall(510, buf, bufsize);
}

static inline long li_blkdev_io(const char *name, unsigned long long lba,
                                unsigned long long count, void *buf, int wr)
{
    return syscall(511, name, lba, count, buf, (unsigned long)wr);
}

static inline long li_gpt_rescan(const char *name)
{
    return syscall(512, name);
}

#endif /* LIBINSTALL_SYSCALLS_H */
