#include <pwd.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    struct passwd *pw = getpwuid(getuid());
    if (pw && pw->pw_name && pw->pw_name[0]) {
        puts(pw->pw_name);
        return 0;
    }
    /* fallback if /etc/passwd not readable: the login-set $USER, else the uid */
    const char *u = getenv("USER");
    if (u && u[0]) { puts(u); return 0; }
    printf("%u\n", (unsigned)getuid());
    return 0;
}
