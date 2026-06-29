/* stat — print metadata for each path. */
#include <unistd.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>

int
main(int argc, char **argv)
{
    if (argc < 2) {
        dprintf(2, "usage: stat <file>...\n");
        return 1;
    }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        struct stat st;
        if (lstat(argv[i], &st) != 0) {
            perror(argv[i]);
            rc = 1;
            continue;
        }
        char buf[256];
        int n = snprintf(buf, sizeof(buf),
            "  File: %s\n  Size: %lld  Mode: %o  Uid: %u  Gid: %u  Inode: %lu\n",
            argv[i], (long long)st.st_size, (unsigned)st.st_mode,
            (unsigned)st.st_uid, (unsigned)st.st_gid,
            (unsigned long)st.st_ino);
        write(1, buf, (size_t)n);
    }
    return rc;
}
