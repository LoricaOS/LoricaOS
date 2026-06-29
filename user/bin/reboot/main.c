#include <sys/syscall.h>
#include <unistd.h>
#include <stdio.h>

int main(void)
{
    long r = syscall(169, 1L);  /* sys_reboot(1) = keyboard reset reboot */
    if (r < 0) {
        perror("reboot");
        return 1;
    }
    return 0;
}
