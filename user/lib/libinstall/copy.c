/* copy.c — block device enumeration + rootfs/ESP copy (libinstall) */
#include "libinstall.h"
#include "syscalls.h"
#include "ext2_grow.h"
#include "cryptroot_crypto.h"
#include <string.h>
#include <stdio.h>
#include <sys/random.h>

/*
 * All logical block devices (ramdisk0, ramdisk1, partition devices via the
 * kernel's 512e GPT wrapper) present 512-byte logical sectors.  Only the
 * raw NVMe device (e.g. "nvme0") has native 4K sectors on Advanced Format
 * drives.
 *
 * Therefore:
 *   - rootfs copy  (ramdisk0 → nvme0p1): both 512B, simple loop
 *   - ESP copy     (ramdisk1 → nvme0):   ramdisk1 is 512B, nvme0 may be
 *                                         4K native — use block_size for
 *                                         target offset + chunk calculation
 */

#define RAM_BLOCK_SIZE   512ULL
/* 64 KiB per transfer: the kernel's blkdev syscall bounce and the NVMe
 * driver's multi-page PRP path both take 64 KiB in one shot now, so this is
 * one syscall -> one NVMe command per chunk (the old 4 KiB value made a
 * 44 MiB install ~45k serialized round-trips across its four passes). */
#define XFER_BYTES       65536ULL
/* ESP_ALIGN_BYTES / ESP_SIZE_BYTES come from libinstall.h (shared with
 * gpt.c — the GPT layout and this raw copy must agree). */

static void report_err(install_progress_t *p, const char *msg)
{
    if (p && p->on_error)
        p->on_error(msg, p->ctx);
}

static int grow_io(void *ctx, uint64_t sector, uint64_t count,
                   void *buf, int write)
{
    return (int)li_blkdev_io((const char *)ctx, sector, count, buf, write);
}

#define CRYPT_HEADER_SECTORS 8ULL
#define CRYPT_PBKDF2_ROUNDS 100000U
typedef struct { const char *dev; install_xts_ctx_t xts; } crypt_io_t;

static int crypt_io(void *opaque,uint64_t sector,uint64_t count,void *buf,int write)
{
    crypt_io_t *c=opaque;unsigned char *p=buf;
    if(write){
        static unsigned char tmp[XFER_BYTES];
        if(count>sizeof(tmp)/RAM_BLOCK_SIZE)return -1;
        memcpy(tmp,buf,(size_t)(count*RAM_BLOCK_SIZE));
        for(uint64_t i=0;i<count;i++)install_xts(&c->xts,tmp+i*RAM_BLOCK_SIZE,512,sector+i,1);
        int rc=(int)li_blkdev_io(c->dev,sector+CRYPT_HEADER_SECTORS,count,tmp,1);
        memset(tmp,0,(size_t)(count*RAM_BLOCK_SIZE));return rc;
    }
    if(li_blkdev_io(c->dev,sector+CRYPT_HEADER_SECTORS,count,buf,0)<0)return -1;
    for(uint64_t i=0;i<count;i++)install_xts(&c->xts,p+i*RAM_BLOCK_SIZE,512,sector+i,0);
    return 0;
}

static void put32(unsigned char *p,uint32_t v)
{ p[0]=v;p[1]=v>>8;p[2]=v>>16;p[3]=v>>24; }

static int crypt_setup(const char *dev,const char *password,crypt_io_t *out,install_progress_t *p)
{
    static unsigned char header[CRYPT_HEADER_SECTORS*RAM_BLOCK_SIZE];
    unsigned char key[64];memset(header,0,sizeof(header));
    memcpy(header,"LORICRY1",8);put32(header+8,1);put32(header+12,CRYPT_HEADER_SECTORS);put32(header+16,CRYPT_PBKDF2_ROUNDS);
    if(getrandom(header+20,16,0)!=16){report_err(p,"secure random unavailable for encrypted root");return -1;}
    install_pbkdf2((const unsigned char *)password,(uint32_t)strlen(password),header+20,CRYPT_PBKDF2_ROUNDS,key);
    install_key_verifier(key,header+36);install_xts_init(&out->xts,key);out->dev=dev;memset(key,0,sizeof(key));
    if(li_blkdev_io(dev,0,CRYPT_HEADER_SECTORS,header,1)<0){report_err(p,"encrypted-root header write failed");return -1;}
    return 0;
}

static int crypt_copy_verify(crypt_io_t *crypt,uint64_t count,install_progress_t *p)
{
    static unsigned char plain[XFER_BYTES],check[XFER_BYTES];uint64_t done=0,max=XFER_BYTES/RAM_BLOCK_SIZE;
    while(done<count){uint64_t n=count-done;if(n>max)n=max;
        if(li_blkdev_io("ramdisk0",done,n,plain,0)<0||crypt_io(crypt,done,n,plain,1)<0){report_err(p,"encrypted root copy failed");return -1;}
        if(crypt_io(crypt,done,n,check,0)<0||memcmp(plain,check,(size_t)(n*RAM_BLOCK_SIZE))){report_err(p,"encrypted root verification failed");return -1;}
        done+=n;if(p&&p->on_progress)p->on_progress((int)(done*100/count),p->ctx);
    }
    memset(plain,0,sizeof(plain));memset(check,0,sizeof(check));return 0;
}

/*
 * Simple 512B-unit copy: both src and dst present 512-byte logical sectors.
 * count is in 512B units.
 */
static int copy_512b(const char *src_dev, uint64_t src_lba,
                     const char *dst_dev, uint64_t dst_lba,
                     uint64_t count, install_progress_t *p)
{
    static unsigned char buf[XFER_BYTES];
    uint64_t max_chunk = XFER_BYTES / RAM_BLOCK_SIZE; /* = 128 */
    int last_pct = -1;
    uint64_t done = 0;

    while (done < count) {
        uint64_t chunk = count - done;
        if (chunk > max_chunk) chunk = max_chunk;

        if (li_blkdev_io(src_dev, src_lba + done, chunk, buf, 0) < 0) {
            report_err(p, "block read failed");
            return -1;
        }
        if (li_blkdev_io(dst_dev, dst_lba + done, chunk, buf, 1) < 0) {
            report_err(p, "block write failed");
            return -1;
        }
        done += chunk;

        int pct = (int)(done * 100 / count);
        if (pct != last_pct && pct % 10 == 0) {
            if (p && p->on_progress) p->on_progress(pct, p->ctx);
            last_pct = pct;
        }
    }
    if (p && p->on_progress && last_pct != 100)
        p->on_progress(100, p->ctx);
    return 0;
}

/*
 * Copy src_block_count 512-byte ramdisk sectors to a raw device (dst_dev)
 * that may have native block_size sectors.  dst_start_lba is in native
 * block_size units.  Reads 128 × 512B (= 64 KiB) at a time; writes
 * XFER_BYTES/block_size native sectors at a time.
 */
static int copy_to_native(const char *src_dev, uint64_t src_block_count,
                           const char *dst_dev, uint64_t dst_start_lba,
                           uint32_t dst_block_size, install_progress_t *p)
{
    static unsigned char buf[XFER_BYTES];
    uint64_t src_per_xfer = XFER_BYTES / RAM_BLOCK_SIZE;          /* = 128 */
    uint64_t dst_per_xfer = XFER_BYTES / (uint64_t)dst_block_size; /* = 16 for 4K */
    uint64_t total_xfers  = (src_block_count + src_per_xfer - 1) / src_per_xfer;
    int last_pct = -1;
    uint64_t i;

    for (i = 0; i < total_xfers; i++) {
        uint64_t s_lba   = i * src_per_xfer;
        uint64_t d_lba   = dst_start_lba + i * dst_per_xfer;
        uint64_t s_count = src_per_xfer;
        if (s_lba + s_count > src_block_count)
            s_count = src_block_count - s_lba;

        memset(buf, 0, sizeof(buf));
        if (li_blkdev_io(src_dev, s_lba, s_count, buf, 0) < 0) {
            report_err(p, "block read failed");
            return -1;
        }
        if (li_blkdev_io(dst_dev, d_lba, dst_per_xfer, buf, 1) < 0) {
            report_err(p, "block write failed");
            return -1;
        }

        int pct = (int)((i + 1) * 100 / total_xfers);
        if (pct != last_pct && pct % 10 == 0) {
            if (p && p->on_progress) p->on_progress(pct, p->ctx);
            last_pct = pct;
        }
    }
    if (p && p->on_progress && last_pct != 100)
        p->on_progress(100, p->ctx);
    return 0;
}

/*
 * Read-back verification: re-read every block of the freshly written range
 * and compare against the source.  This exists because the copy loop only
 * proves the controller ACKed each write — on real hardware a transient
 * NVMe fault (timed-out command, late DMA into the shared kernel bounce
 * buffer) can corrupt written data while every write still reports success.
 * QEMU never exhibits this, so without the verify pass a bad install looks
 * identical to a good one until the installed system misbehaves.
 */
static int verify_512b(const char *src_dev, uint64_t src_lba,
                       const char *dst_dev, uint64_t dst_lba,
                       uint64_t count, install_progress_t *p)
{
    static unsigned char buf_a[XFER_BYTES];
    static unsigned char buf_b[XFER_BYTES];
    uint64_t max_chunk = XFER_BYTES / RAM_BLOCK_SIZE; /* = 128 */
    int last_pct = -1;
    uint64_t done = 0;

    while (done < count) {
        uint64_t chunk = count - done;
        if (chunk > max_chunk) chunk = max_chunk;

        if (li_blkdev_io(src_dev, src_lba + done, chunk, buf_a, 0) < 0) {
            report_err(p, "verify: source read failed");
            return -1;
        }
        if (li_blkdev_io(dst_dev, dst_lba + done, chunk, buf_b, 0) < 0) {
            report_err(p, "verify: target read failed");
            return -1;
        }
        if (memcmp(buf_a, buf_b, (size_t)(chunk * RAM_BLOCK_SIZE)) != 0) {
            char msg[96];
            snprintf(msg, sizeof(msg),
                     "verify mismatch at sector %llu — install corrupt",
                     (unsigned long long)(dst_lba + done));
            report_err(p, msg);
            return -1;
        }
        done += chunk;

        int pct = (int)(done * 100 / count);
        if (pct != last_pct && pct % 10 == 0) {
            if (p && p->on_progress) p->on_progress(pct, p->ctx);
            last_pct = pct;
        }
    }
    if (p && p->on_progress && last_pct != 100)
        p->on_progress(100, p->ctx);
    return 0;
}

/* Verify counterpart of copy_to_native: same chunking, full-page compares
 * (the copy wrote zero-padded XFER_BYTES transfers, so padding matches the
 * memset below). */
static int verify_to_native(const char *src_dev, uint64_t src_block_count,
                            const char *dst_dev, uint64_t dst_start_lba,
                            uint32_t dst_block_size, install_progress_t *p)
{
    static unsigned char buf_a[XFER_BYTES];
    static unsigned char buf_b[XFER_BYTES];
    uint64_t src_per_xfer = XFER_BYTES / RAM_BLOCK_SIZE;
    uint64_t dst_per_xfer = XFER_BYTES / (uint64_t)dst_block_size;
    uint64_t total_xfers  = (src_block_count + src_per_xfer - 1) / src_per_xfer;
    uint64_t i;

    for (i = 0; i < total_xfers; i++) {
        uint64_t s_lba   = i * src_per_xfer;
        uint64_t d_lba   = dst_start_lba + i * dst_per_xfer;
        uint64_t s_count = src_per_xfer;
        if (s_lba + s_count > src_block_count)
            s_count = src_block_count - s_lba;

        memset(buf_a, 0, sizeof(buf_a));
        if (li_blkdev_io(src_dev, s_lba, s_count, buf_a, 0) < 0) {
            report_err(p, "verify: source read failed");
            return -1;
        }
        if (li_blkdev_io(dst_dev, d_lba, dst_per_xfer, buf_b, 0) < 0) {
            report_err(p, "verify: target read failed");
            return -1;
        }
        if (memcmp(buf_a, buf_b, sizeof(buf_a)) != 0) {
            char msg[96];
            snprintf(msg, sizeof(msg),
                     "verify mismatch at LBA %llu — install corrupt",
                     (unsigned long long)d_lba);
            report_err(p, msg);
            return -1;
        }
    }
    return 0;
}

/* ── Public: install_list_blkdevs ───────────────────────────────────── */

int install_list_blkdevs(install_blkdev_t *out, int max)
{
    long n = li_blkdev_list(out,
                            (unsigned long)(sizeof(install_blkdev_t) * (unsigned)max));
    /* Preserve a negative kernel error (notably -ENOCAP when the caller lacks
     * CAP_KIND_DISK_ADMIN) so callers can tell "not authorized" from "zero
     * devices". Both installers treat n<0 as an admin-capability error; the
     * GUI's `i < n` populate loop simply iterates zero times on a negative. */
    return (int)n;
}

/* ── Public: install_disk_has_aegis ─────────────────────────────────── */

int install_disk_has_aegis(const char *devname)
{
    install_blkdev_t devs[8];
    int n = install_list_blkdevs(devs, 8);
    int dlen = 0;
    while (devname[dlen]) dlen++;
    int i;
    for (i = 0; i < n; i++) {
        /* Must start with `<devname>` and have `p<digit>` after. */
        int j;
        int prefix_ok = 1;
        for (j = 0; j < dlen; j++) {
            if (devs[i].name[j] != devname[j]) { prefix_ok = 0; break; }
        }
        if (!prefix_ok) continue;
        if (devs[i].name[dlen] == 'p' &&
            devs[i].name[dlen + 1] >= '0' &&
            devs[i].name[dlen + 1] <= '9')
            return 1;
    }
    return 0;
}

/* ── Public: install_copy_esp ───────────────────────────────────────── */

int install_copy_esp(const char *devname, uint32_t block_size,
                     install_progress_t *p)
{
    if (p && p->on_step)
        p->on_step("Installing EFI bootloader", p->ctx);

    install_blkdev_t devs[8];
    int n = install_list_blkdevs(devs, 8);
    uint64_t esp_blocks = 0; /* ramdisk1 block count (512B each) */
    int i;
    for (i = 0; i < n; i++) {
        if (strcmp(devs[i].name, "ramdisk1") == 0) {
            esp_blocks = devs[i].block_count;
            break;
        }
    }
    if (esp_blocks == 0) {
        report_err(p, "ramdisk1 (ESP image) not found");
        return -1;
    }

    /* Clamp to ESP partition size in 512B terms */
    uint64_t esp_max_src = ESP_SIZE_BYTES / RAM_BLOCK_SIZE;
    if (esp_blocks > esp_max_src) esp_blocks = esp_max_src;

    /* ESP start in native block_size LBAs on the raw disk */
    uint64_t esp_start_lba = ESP_ALIGN_BYTES / (uint64_t)block_size;

    /* devname is the raw disk (e.g. "nvme0") which may have 4K native blocks */
    if (copy_to_native("ramdisk1", esp_blocks,
                       devname, esp_start_lba, block_size, p) < 0)
        return -1;

    if (p && p->on_step)
        p->on_step("Verifying EFI bootloader", p->ctx);
    return verify_to_native("ramdisk1", esp_blocks,
                            devname, esp_start_lba, block_size, p);
}

/* ── Public: install_copy_rootfs ────────────────────────────────────── */

int install_copy_rootfs(const char *dst_dev, uint64_t dst_blocks,
                        uint32_t block_size, const char *password,
                        install_progress_t *p)
{
    (void)block_size; /* partition device presents 512B after 512e */
    if (p && p->on_step)
        p->on_step("Copying root filesystem", p->ctx);

    install_blkdev_t devs[8];
    int n = install_list_blkdevs(devs, 8);
    uint64_t src_blocks = 0; /* ramdisk0 block count (512B each) */
    int i;
    for (i = 0; i < n; i++) {
        if (strcmp(devs[i].name, "ramdisk0") == 0) {
            src_blocks = devs[i].block_count;
            break;
        }
    }
    if (src_blocks == 0) {
        report_err(p, "ramdisk0 not found");
        return -1;
    }
    uint64_t usable_blocks=dst_blocks;
    if(password&&password[0]){
        if(dst_blocks<=CRYPT_HEADER_SECTORS){report_err(p,"target too small for encrypted root");return -1;}
        usable_blocks-=CRYPT_HEADER_SECTORS;
    }
    if (src_blocks > usable_blocks) {
        report_err(p, "rootfs larger than target partition");
        return -1;
    }

    crypt_io_t crypt;
    if(password&&password[0]){
        if(p&&p->on_step)p->on_step("Encrypting root filesystem",p->ctx);
        if(crypt_setup(dst_dev,password,&crypt,p)<0||crypt_copy_verify(&crypt,src_blocks,p)<0)return -1;
    }else{
        if(copy_512b("ramdisk0",0,dst_dev,0,src_blocks,p)<0)return -1;
        if(p&&p->on_step)p->on_step("Verifying root filesystem",p->ctx);
        if(verify_512b("ramdisk0",0,dst_dev,0,src_blocks,p)<0)return -1;
    }

    if (p && p->on_step)
        p->on_step("Expanding root filesystem", p->ctx);
    const char *grow_error = NULL;
    uint64_t grown_sectors = 0;
    ext2_grow_io_fn io=(password&&password[0])?crypt_io:grow_io;
    void *io_ctx=(password&&password[0])?(void *)&crypt:(void *)dst_dev;
    if (ext2_grow(io, io_ctx, usable_blocks,
                  &grown_sectors, &grow_error) < 0) {
        report_err(p, grow_error ? grow_error : "root filesystem expansion failed");
        return -1;
    }
    (void)grown_sectors;
    if (p && p->on_progress) p->on_progress(100, p->ctx);
    return 0;
}
