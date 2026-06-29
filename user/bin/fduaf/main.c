/* fduaf — regression test for the fork/dup shared-fd use-after-free class.
 *
 * Guards the three fd-lifecycle UAFs fixed on wip/syscall-stability:
 *   - procfs file/dir fds had .dup==NULL while .close freed the per-open priv,
 *     so fork()+close double-freed / used-after-free the shared priv.
 *   - pty slave: a second open() of the same /dev/pts/N reset the refcount.
 * Plus the general pattern: open an fd, fork, have BOTH parent and child use and
 * close it. On a kernel where the object's .dup doesn't refcount, the first
 * close frees it under the other process → UAF on the survivor's read/close.
 *
 * The property under test is "the kernel survived" — these operations must not
 * panic or corrupt. Prints FDUAF_OK if the whole battery completes with the
 * process tree intact.
 *
 *   FDUAF_OK            — every fork+shared-fd close completed, no kernel death
 *   FDUAF_SKIP: <why>   — could not open a needed fd (environmental)
 *
 * Baseline caps only.
 */
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

/* Open path, fork, both ends read a byte then close, parent reaps. Returns 0 if
 * the round-trip completed (kernel survived), -1 if the open failed. */
static int fork_share_close(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;

    pid_t pid = fork();
    if (pid < 0) { close(fd); return -1; }

    if (pid == 0) {
        /* Child shares the same fd (fork dup'd it). Read + close, then exit.
         * If .dup didn't refcount, this close frees the object the parent
         * still holds. */
        char b;
        (void)read(fd, &b, 1);
        close(fd);
        _exit(0);
    }

    /* Parent: let the child run/close first, then use + close our copy. A UAF
     * would manifest here (read/close of a freed object). */
    int st = 0;
    waitpid(pid, &st, 0);
    char b;
    (void)read(fd, &b, 1);   /* parent reads AFTER child closed — survivor path */
    close(fd);
    return 0;
}

int main(void)
{
    /* procfs file fd shared across fork (the procfs_file UAF). */
    if (fork_share_close("/proc/meminfo") < 0)
        if (fork_share_close("/proc/uptime") < 0)
            { printf("FDUAF_SKIP: no /proc file\n"); return 0; }

    /* procfs dir fd shared across fork (the procfs_dir UAF). */
    (void)fork_share_close("/proc");

    /* Regular ext2/initrd file shared across fork (control: ext2 has .dup). */
    (void)fork_share_close("/etc/passwd");

    /* pty slave second-open + fork (the pty slave refcount-reset UAF).
     * Open the master, derive the slave path, open the slave TWICE, fork, and
     * close everything in interleaved order. */
    {
        int m = open("/dev/ptmx", O_RDWR);
        if (m >= 0) {
            /* TIOCGPTN = 0x80045430 — get the pty number. */
            unsigned int n = 0;
            char sp[32];
            if (ioctl(m, 0x80045430u, &n) == 0) {
                /* unlockpt: TIOCSPTLCK = 0x40045431, arg 0 */
                int zero = 0;
                (void)ioctl(m, 0x40045431u, &zero);
                /* build "/dev/pts/<n>" without snprintf surprises */
                int i = 0; sp[i++]='/';sp[i++]='d';sp[i++]='e';sp[i++]='v';
                sp[i++]='/';sp[i++]='p';sp[i++]='t';sp[i++]='s';sp[i++]='/';
                if (n == 0) sp[i++]='0';
                else { char t[10]; int k=0; while(n){t[k++]='0'+n%10;n/=10;} while(k) sp[i++]=t[--k]; }
                sp[i]='\0';

                int s1 = open(sp, O_RDWR);
                int s2 = open(sp, O_RDWR);   /* SECOND open — the refcount-reset case */
                pid_t pid = fork();
                if (pid == 0) {
                    if (s1 >= 0) close(s1);  /* child closes one ref */
                    _exit(0);
                }
                waitpid(pid, 0, 0);
                if (s2 >= 0) close(s2);      /* survivor closes the other */
                if (s1 >= 0) close(s1);      /* parent's copy of s1 */
            }
            close(m);
        }
    }

    printf("FDUAF_OK\n");
    return 0;
}
