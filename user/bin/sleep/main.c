/* sleep — pause for N seconds (integer only).
 * Usage: sleep SECONDS
 * Exit 0 on success, 1 on bad usage. */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>

int
main(int argc, char **argv)
{
    if (argc != 2) {
        fputs("usage: sleep SECONDS\n", stderr);
        return 1;
    }

    char *end;
    long secs = strtol(argv[1], &end, 10);
    if (*argv[1] == '\0' || *end != '\0' || secs < 0) {
        fprintf(stderr, "sleep: invalid duration: %s\n", argv[1]);
        return 1;
    }

    if (secs == 0)
        return 0;

    struct timespec req = { .tv_sec = secs, .tv_nsec = 0 };
    struct timespec rem;
    while (nanosleep(&req, &rem) == -1 && errno == EINTR)
        req = rem;
    return 0;
}
