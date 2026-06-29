/* run.c — install_run_all orchestrator (libinstall) */
#include "libinstall.h"
#include <string.h>
#include <unistd.h>

static void report_err(install_progress_t *p, const char *msg)
{
    if (p && p->on_error)
        p->on_error(msg, p->ctx);
}

int install_run_all(const char *devname, uint64_t disk_blocks,
                    uint32_t block_size,
                    const char *root_hash,
                    const char *username,
                    const char *user_hash,
                    install_progress_t *p)
{
    if (!devname || !root_hash) {
        report_err(p, "invalid arguments to install_run_all");
        return -1;
    }

    /* 1. Boot config for the installed system (no-op under Limine — the
     *    bootloader + limine.conf travel in the ESP image, see config.c). */
    if (p && p->on_step)
        p->on_step("Writing boot config", p->ctx);
    if (install_write_boot_cfg() < 0) {
        report_err(p, "write boot config failed");
        return -1;
    }
    if (p && p->on_progress) p->on_progress(100, p->ctx);

    /* 2. Strip test binaries */
    if (p && p->on_step)
        p->on_step("Stripping test binaries", p->ctx);
    install_strip_test_binaries();
    if (p && p->on_progress) p->on_progress(100, p->ctx);

    /* 3. Write credentials */
    if (p && p->on_step)
        p->on_step("Writing user accounts", p->ctx);
    if (install_write_credentials(root_hash, username, user_hash) < 0) {
        report_err(p, "write /etc/passwd failed");
        return -1;
    }
    if (p && p->on_progress) p->on_progress(100, p->ctx);

    /* 4. GPT */
    if (install_write_gpt(devname, disk_blocks, block_size, p) < 0)
        return -1;

    /* 5. Rescan */
    if (p && p->on_step)
        p->on_step("Rescanning partitions", p->ctx);
    int nparts = install_rescan_gpt(devname);
    if (nparts <= 0) {
        report_err(p, "partition rescan failed");
        return -1;
    }

    /* 6. Find root partition — first partition on devname whose name
     *    starts with `${devname}p`. */
    install_blkdev_t devs[8];
    int n = install_list_blkdevs(devs, 8);
    char root_part[16] = "";
    uint64_t root_blocks = 0;
    size_t devname_len = strlen(devname);
    int i;
    for (i = 0; i < n; i++) {
        if (strncmp(devs[i].name, devname, devname_len) == 0 &&
            devs[i].name[devname_len] == 'p') {
            size_t copylen = strlen(devs[i].name);
            if (copylen >= sizeof(root_part)) copylen = sizeof(root_part) - 1;
            memcpy(root_part, devs[i].name, copylen);
            root_part[copylen] = '\0';
            root_blocks = devs[i].block_count;
            break;
        }
    }
    if (root_part[0] == '\0') {
        report_err(p, "root partition not found after rescan");
        return -1;
    }
    if (p && p->on_progress) p->on_progress(100, p->ctx);

    /* 6b. Flush the live filesystem to ramdisk0 BEFORE the raw block copy.
     *     install_copy_rootfs reads ramdisk0 with raw block I/O, but the
     *     credential write (step 3) and test-binary strip (step 2) mutate
     *     the mounted rootfs through ext2's write-back block cache. Without
     *     this sync those dirty blocks are still in cache, so the raw
     *     ramdisk0->disk copy ships the *pristine* /etc/shadow
     *     (root/forevervigilant) and silently discards the user's chosen
     *     credentials. ext2_sync() flushes all dirty ext2 blocks to ramdisk0. */
    if (p && p->on_step)
        p->on_step("Flushing filesystem", p->ctx);
    sync();
    if (p && p->on_progress) p->on_progress(100, p->ctx);

    /* 7. Copy rootfs */
    if (install_copy_rootfs(root_part, root_blocks, block_size, p) < 0)
        return -1;

    /* 8. Copy ESP */
    if (install_copy_esp(devname, block_size, p) < 0)
        return -1;

    /* 9. Sync */
    if (p && p->on_step)
        p->on_step("Syncing disk", p->ctx);
    sync();
    if (p && p->on_progress) p->on_progress(100, p->ctx);

    return 0;
}
