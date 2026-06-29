#include <pwd.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    struct passwd *pw = getpwuid(getuid());
    if (pw && pw->pw_name && pw->pw_name[0]) {
        puts(pw->pw_name);
        return 0;
    }
    /* fallback if /etc/passwd not readable */
    write(1, "root\n", 5);
    return 0;
}
