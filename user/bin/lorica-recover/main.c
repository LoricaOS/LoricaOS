/* Minimal PID 1 for the offline LoricaOS recovery root. */
#include "ext2_repair.h"
#include "cryptroot_crypto.h"
#include <stdint.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <termios.h>
#include <unistd.h>

#define SYS_BLKDEV_LIST 510
#define SYS_BLKDEV_IO   511
#define MAX_DEVS 16

typedef struct {
    char name[16]; uint64_t block_count; uint32_t block_size, pad;
} blkdev_t;

typedef struct { blkdev_t dev; int encrypted; install_xts_ctx_t xts; } target_t;

static int raw_io(target_t *t,uint64_t sector,uint64_t count,void *buf,int write)
{ return syscall(SYS_BLKDEV_IO,t->dev.name,sector,count,buf,write)<0?-1:0; }

static int disk_io(void *ctx, uint64_t sector, uint64_t count,
                   void *buf, int write)
{
    target_t *t = ctx;
    unsigned char *p=buf;static unsigned char tmp[65536];
    if(!t->encrypted)return raw_io(t,sector,count,buf,write);
    if(count>sizeof(tmp)/512)return -1;
    if(write){memcpy(tmp,buf,(size_t)count*512);for(uint64_t i=0;i<count;i++)install_xts(&t->xts,tmp+i*512,512,sector+i,1);int rc=raw_io(t,sector+8,count,tmp,1);memset(tmp,0,(size_t)count*512);return rc;}
    if(raw_io(t,sector+8,count,buf,0)<0)return -1;
    for(uint64_t i=0;i<count;i++)install_xts(&t->xts,p+i*512,512,sector+i,0);
    return 0;
}

static uint32_t le32(const unsigned char*p){return(uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static int equal(const unsigned char*a,const unsigned char*b){unsigned char d=0;for(int i=0;i<32;i++)d|=a[i]^b[i];return d==0;}
static int unlock_target(target_t *t)
{
    unsigned char h[4096],key[64],check[32];char pw[128];
    if(raw_io(t,0,8,h,0)<0||memcmp(h,"LORICRY1",8))return 0;
    uint32_t rounds=le32(h+16);if(le32(h+8)!=1||le32(h+12)!=8||rounds<10000||rounds>2000000){puts("Invalid encrypted-root header.");return -1;}
    for(int tries=0;tries<3;tries++){
        struct termios old,noecho;printf("Root passphrase: ");fflush(stdout);tcgetattr(0,&old);noecho=old;noecho.c_lflag&=~ECHO;tcsetattr(0,TCSANOW,&noecho);
        if(!fgets(pw,sizeof(pw),stdin)){tcsetattr(0,TCSANOW,&old);return -1;}tcsetattr(0,TCSANOW,&old);putchar('\n');pw[strcspn(pw,"\r\n")]=0;
        install_pbkdf2((unsigned char*)pw,strlen(pw),h+20,rounds,key);memset(pw,0,sizeof(pw));install_key_verifier(key,check);
        if(equal(check,h+36)){install_xts_init(&t->xts,key);memset(key,0,sizeof(key));t->encrypted=1;t->dev.block_count-=8;puts("Encrypted root unlocked.");return 0;}
        memset(key,0,sizeof(key));puts("Incorrect passphrase.");
    }
    return -1;
}

static int is_partition(const char *name)
{
    const char *p = strrchr(name, 'p');
    return p && p[1] >= '0' && p[1] <= '9';
}

static int read_choice(void)
{
    int c, first = '\n';
    while ((c = getchar()) != '\n' && c != EOF)
        if (first == '\n') first = c;
    return first;
}

static int automatic_test(void)
{
    char buf[256]; int fd = open("/proc/cmdline", O_RDONLY);
    ssize_t n = fd < 0 ? -1 : read(fd, buf, sizeof(buf) - 1);
    if (fd >= 0) close(fd);
    if (n < 0) return 0;
    buf[n] = '\0';
    return strstr(buf, "recovery_auto") != NULL;
}

static int choose_target(target_t *out)
{
    blkdev_t devs[MAX_DEVS]; int n, i, count = 0, choice = 0;
    n = (int)syscall(SYS_BLKDEV_LIST, devs, sizeof(devs));
    if (n < 0) {
        perror("recovery: block-device list");
        return -1;
    }
    printf("Installed Aegis filesystems:\n");
    for (i = 0; i < n && i < MAX_DEVS; i++) {
        if (!is_partition(devs[i].name)) continue;
        if (devs[i].block_size != 512) {
            printf("  skipping %s: unsupported logical block size %u\n",
                   devs[i].name, devs[i].block_size);
            continue;
        }
        printf("  %d. %s (%llu MiB)\n", ++count, devs[i].name,
               (unsigned long long)(devs[i].block_count / 2048u));
        if (count == 1) out->dev = devs[i];
    }
    if (count == 0) return -1;
    if (count == 1) return 0;
    printf("Select filesystem [1-%d]: ", count); fflush(stdout);
    choice = read_choice() - '0';
    if (choice < 1 || choice > count) return -1;
    count = 0;
    for (i = 0; i < n && i < MAX_DEVS; i++) {
        if (!is_partition(devs[i].name) || devs[i].block_size != 512) continue;
        if (++count == choice) { out->dev = devs[i]; return 0; }
    }
    return -1;
}

int main(int argc, char **argv)
{
    target_t target={0}; ext2_repair_report_t report; const char *error = NULL;
    int rc, answer, automatic;
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc == 1) {
        char *again[] = { "/bin/vigil", "--authorized", NULL };
        execv(again[0], again);
        perror("recovery: capability re-exec");
        goto stay;
    }
    (void)argv;
    automatic = automatic_test();
    puts("\nLoricaOS Offline Recovery");
    puts("The installed root filesystem is not mounted.");
    if (choose_target(&target) < 0) {
        puts("No single repairable Aegis partition was found.");
        goto stay;
    }
    if(unlock_target(&target)<0){puts("Could not unlock encrypted root.");goto stay;}
    printf("Checking %s...\n", target.dev.name);
    rc = ext2_repair(disk_io, &target, target.dev.block_count, 0, &report, &error);
    if (rc < 0) {
        printf("UNSAFE TO REPAIR: %s\n", error ? error : "unknown damage");
        puts("No changes were made. Use full e2fsck from external media.");
        goto stay;
    }
    if (rc == 0) {
        printf("%s is clean (%u files, %u directories).\n",
               target.dev.name, report.inodes, report.directories);
    } else {
        printf("Repairable inconsistencies found on %s.\n", target.dev.name);
        printf("Repair now? [y/N]: "); answer = automatic ? 'y' : read_choice();
        if (answer != 'y' && answer != 'Y') {
            puts("No changes made."); goto stay;
        }
        rc = ext2_repair(disk_io, &target, target.dev.block_count, 1,
                         &report, &error);
        if (rc < 0) {
            printf("REPAIR FAILED: %s\n", error ? error : "I/O error");
            goto stay;
        }
        printf("Repair complete: block bits +%u, inode bits +%u, counters %u.\n",
               report.block_bits_fixed, report.inode_bits_fixed,
               report.counters_fixed);
        if (automatic) puts("[RECOVERY] PASS");
    }
    if (automatic) goto stay;
    printf("Press r to reboot, or any other key to remain in recovery: ");
    if (read_choice() == 'r') syscall(169, 1L);
stay:
    puts("Recovery halted. Reboot with Ctrl-Alt-Del when ready.");
    for (;;) pause();
}
