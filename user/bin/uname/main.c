#include <stdio.h>
#include <sys/utsname.h>
int main(void) {
    struct utsname u;
    if (uname(&u) == 0)
        printf("%s %s\n", u.sysname, u.machine);
    else
        puts("Aegis");
    return 0;
}
