/* hostname — print or set the system hostname.
 * Setting requires CAP_KIND_POWER (granted via /etc/aegis/caps.d/hostname). */
#include <unistd.h>
#include <sys/utsname.h>
#include <sys/syscall.h>
#include <stdio.h>
#include <string.h>

#define SYS_SETHOSTNAME 170

int
main(int argc, char **argv)
{
    if (argc >= 2) {
        size_t len = strlen(argv[1]);
        long rc = syscall(SYS_SETHOSTNAME, argv[1], (long)len);
        if (rc != 0) {
            dprintf(2, "hostname: set failed (%ld)\n", rc);
            return 1;
        }
        return 0;
    }
    struct utsname u;
    if (uname(&u) != 0) {
        perror("uname");
        return 1;
    }
    int n = (int)strlen(u.nodename);
    write(1, u.nodename, (size_t)n);
    write(1, "\n", 1);
    return 0;
}
