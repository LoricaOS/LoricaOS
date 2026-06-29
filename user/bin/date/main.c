/* date — print current date/time. Default RFC-style; +FMT for custom. */
#include <time.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>

int
main(int argc, char **argv)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        perror("clock_gettime");
        return 1;
    }
    time_t t = ts.tv_sec;
    struct tm tm;
    gmtime_r(&t, &tm);

    const char *fmt = "%a %b %e %H:%M:%S UTC %Y";
    if (argc >= 2 && argv[1][0] == '+') fmt = argv[1] + 1;

    char out[128];
    size_t n = strftime(out, sizeof(out), fmt, &tm);
    write(1, out, n);
    write(1, "\n", 1);
    return 0;
}
