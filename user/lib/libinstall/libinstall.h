/* libinstall.h — Aegis installer library (Phase 2 extraction)
 *
 * Pure install logic shared between the text-mode installer and the
 * upcoming Glyph-based GUI installer.  All UI concerns (prompts,
 * progress display) live in the caller; libinstall emits progress
 * via callbacks and returns 0 / -1 on each operation.
 */
#ifndef LIBINSTALL_H
#define LIBINSTALL_H

#include <stdint.h>

/* On-disk ESP layout, shared by gpt.c (partition table math) and copy.c
 * (raw image copy).  ESP_SIZE_BYTES MUST equal the size of esp.img exactly
 * (Makefile $(ESP_IMG) rule: dd count=8192 512B sectors = 4 MiB) — the
 * installer raw-copies the esp.img ramdisk onto the ESP partition. */
#define ESP_ALIGN_BYTES (1ULL * 1024 * 1024)   /* 1 MiB start alignment */
#define ESP_SIZE_BYTES  (4ULL * 1024 * 1024)   /* 4 MiB ESP             */

/* Progress callback struct.  All callbacks are optional (NULL is OK).
 *
 *   on_step     — called at the start of each named phase
 *                 ("Writing GPT", "Copying rootfs", ...).
 *   on_progress — called while a phase is running, 0..100.
 *   on_error    — called with a human-readable message if a step fails.
 *                 libinstall always returns -1 from the failing call;
 *                 the error callback is purely for UI display.
 *
 * `ctx` is an opaque caller pointer passed back to every callback. */
typedef struct {
    void (*on_step)(const char *label, void *ctx);
    void (*on_progress)(int pct, void *ctx);
    void (*on_error)(const char *msg, void *ctx);
    void *ctx;
} install_progress_t;

/* Block device enumeration.  Layout matches the kernel syscall-510
 * ABI exactly: {char[16], uint64, uint32, uint32 pad}. */
typedef struct {
    char     name[16];
    uint64_t block_count;
    uint32_t block_size;
    uint32_t _pad;
} install_blkdev_t;

int install_list_blkdevs(install_blkdev_t *out, int max);

/* Returns 1 if the given disk has at least one Aegis-typed partition
 * registered (i.e. an existing Aegis installation), 0 otherwise.
 * Detection: any blkdev whose name is `<devname>p<digit>` was
 * registered by the kernel's gpt_scan, which only registers
 * partitions matching the Aegis GUID prefix. */
int install_disk_has_aegis(const char *devname);

/* GPT — write protective MBR + primary GPT + backup GPT.
 * Creates 12 MB ESP (LBA 2048..26623 in 512B terms) and Aegis root
 * (rest of disk).
 * block_size must be 512 or 4096 (native LBA size of the device).
 * Returns 0 on success, -1 on I/O error (error callback fired). */
int install_write_gpt(const char *devname, uint64_t disk_blocks,
                      uint32_t block_size, install_progress_t *p);

/* Ask the kernel to re-enumerate partitions on `devname`.
 * Returns the number of partitions found (>0 on success). */
int install_rescan_gpt(const char *devname);

/* Copy ramdisk1 (the ESP image embedded as module 2 in the live
 * ISO) to the ESP partition on `devname`.
 * block_size: native LBA size of devname (512 or 4096). */
int install_copy_esp(const char *devname, uint32_t block_size,
                     install_progress_t *p);

/* Copy ramdisk0 (the ext2 rootfs image embedded as module 1) to
 * `dst_dev` (the Aegis root partition found via install_rescan_gpt).
 * block_size: native LBA size of dst_dev. Emits on_progress every 10%. */
int install_copy_rootfs(const char *dst_dev, uint64_t dst_blocks,
                        uint32_t block_size, install_progress_t *p);

/* Installed-system boot config step. No-op under Limine: the bootloader and
 * limine.conf are carried in the ESP image (install_copy_esp), which boots the
 * kernel from the ext2 root by GPT GUID — nothing is written to the ext2 fs.
 * Retained as an ordered phase for progress UX / ABI stability. */
int install_write_boot_cfg(void);

/* Delete test binaries (thread_test, mmap_test, etc.) from the
 * currently mounted rootfs.  Must run before install_copy_rootfs. */
void install_strip_test_binaries(void);

/* Hash a password using crypt(3) with a fresh SHA-512 salt.
 * `out` must be at least 128 bytes.  Returns 0 on success. */
int install_hash_password(const char *password, char *out, int outsz);

/* Validate a username: [a-z_][a-z0-9_-]* up to 31 chars. Rejects ':' and
 * newlines (which would corrupt the colon/line-delimited account files).
 * Returns 1 if valid, 0 otherwise. */
int install_username_valid(const char *username);

/* Write /etc/passwd, /etc/shadow, /etc/group on the currently
 * mounted rootfs.  `username` and `user_hash` may be NULL to skip
 * the optional second user account.  `root_hash` is required. */
int install_write_credentials(const char *root_hash,
                              const char *username,
                              const char *user_hash);

/* One-shot orchestration driver.  Runs every phase in order using
 * the supplied progress struct.  Both the TUI and GUI installer
 * drive this after collecting disk + credentials from the user.
 *
 * Phases, in order:
 *   1. install_write_boot_cfg
 *   2. install_strip_test_binaries
 *   3. install_write_credentials (using the supplied hashes)
 *   4. install_write_gpt
 *   5. install_rescan_gpt
 *   6. install_copy_rootfs (to the rescanned root partition)
 *   7. install_copy_esp
 *   8. sync()
 *
 * Returns 0 on success, -1 on any failure.  On failure, the error
 * callback has been called with a diagnostic and the partially-
 * written disk is left in place (caller may retry or abort). */
int install_run_all(const char *devname, uint64_t disk_blocks,
                    uint32_t block_size,
                    const char *root_hash,
                    const char *username,
                    const char *user_hash,
                    install_progress_t *p);

/* Obtain a sudo-style admin session for THIS process so the kernel will
 * authorize raw whole-disk writes (CAP_KIND_DISK_ADMIN). Fork/exec's
 * /bin/login -elevate, which verifies the separate admin credential
 * (/etc/aegis/admin) and elevates its parent (this process).
 *
 *   admin_pw != NULL — feed the password to login over a pipe on its stdin
 *                      (GUI installer: no controlling tty).
 *   admin_pw == NULL — leave stdin connected to the shared tty so login
 *                      prompts interactively (text installer).
 *
 * Returns 0 if login verified the credential and elevation succeeded,
 * <0 on any failure (bad password, fork/exec error). Must be called before
 * the first install_run_all / install_copy_* on the target disk. */
int install_elevate(const char *admin_pw);

#endif /* LIBINSTALL_H */
