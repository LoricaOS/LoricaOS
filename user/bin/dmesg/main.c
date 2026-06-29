/* dmesg — print the kernel log ring buffer.
 *
 * Reads /proc/dmesg until EOF and writes it to stdout.
 *
 * No flags, deliberately: the Aegis klog ring stores raw printk bytes
 * with no per-record timestamps or priority levels, so Linux's -t
 * ("omit timestamps") would be a no-op and there is no record structure
 * to filter on.  The kernel exposes the newest ~4KB tail of the 64KB
 * ring (the procfs generation buffer is one page).
 */
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

int main(void)
{
    int fd = open("/proc/dmesg", O_RDONLY);
    if (fd < 0) {
        perror("/proc/dmesg");
        return 1;
    }

    char buf[4096];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(1, buf + off, (size_t)(n - off));
            if (w < 0) {
                perror("write");
                close(fd);
                return 1;
            }
            off += w;
        }
    }
    if (n < 0) {
        perror("/proc/dmesg");
        close(fd);
        return 1;
    }
    close(fd);
    return 0;
}
