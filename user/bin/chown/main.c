#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: chown UID:GID FILE\n");
        return 1;
    }
    char *colon = strchr(argv[1], ':');
    if (!colon) {
        fprintf(stderr, "format: UID:GID\n");
        return 1;
    }
    *colon = '\0';
    uid_t uid = (uid_t)atoi(argv[1]);
    gid_t gid = (gid_t)atoi(colon + 1);
    if (chown(argv[2], uid, gid) != 0) {
        perror("chown");
        return 1;
    }
    return 0;
}
