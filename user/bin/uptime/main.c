/* uptime — time since boot, current time, process count.
 *
 * CLOCK_MONOTONIC on Aegis is raw ticks since boot, so it IS the uptime;
 * CLOCK_REALTIME is wall time once chronos has synced. There is no load
 * average yet — the live process count stands in for it.
 *
 *   uptime        " 12:01:33 up 2:13, 14 processes"
 *   uptime -p     "up 2 hours, 13 minutes"
 *   uptime -s     boot time, "YYYY-MM-DD HH:MM:SS"
 */
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <dirent.h>

static int count_procs(void)
{
    DIR *d = opendir("/proc");
    if (!d) return 0;
    int n = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL)
        if (isdigit((unsigned char)de->d_name[0]))
            n++;
    closedir(d);
    return n;
}

int main(int argc, char **argv)
{
    int pretty = 0, since = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0)      pretty = 1;
        else if (strcmp(argv[i], "-s") == 0) since = 1;
        else {
            fprintf(stderr, "usage: uptime [-p|-s]\n");
            return 1;
        }
    }

    struct timespec mono, real;
    clock_gettime(CLOCK_MONOTONIC, &mono);
    clock_gettime(CLOCK_REALTIME, &real);

    long up = mono.tv_sec;
    long days = up / 86400;
    long hrs  = (up % 86400) / 3600;
    long mins = (up % 3600) / 60;

    if (since) {
        time_t boot = real.tv_sec - up;
        struct tm tm;
        gmtime_r(&boot, &tm);
        char out[64];
        strftime(out, sizeof(out), "%Y-%m-%d %H:%M:%S", &tm);
        printf("%s\n", out);
        return 0;
    }

    if (pretty) {
        printf("up");
        int wrote = 0;
        if (days) { printf(" %ld day%s", days, days == 1 ? "" : "s"); wrote = 1; }
        if (hrs)  { printf("%s %ld hour%s", wrote ? "," : "", hrs,
                           hrs == 1 ? "" : "s"); wrote = 1; }
        printf("%s %ld minute%s\n", wrote ? "," : "", mins,
               mins == 1 ? "" : "s");
        return 0;
    }

    struct tm tm;
    time_t now = real.tv_sec;
    gmtime_r(&now, &tm);
    char clock[16];
    strftime(clock, sizeof(clock), "%H:%M:%S", &tm);

    printf(" %s up ", clock);
    if (days)
        printf("%ld day%s, %ld:%02ld", days, days == 1 ? "" : "s", hrs, mins);
    else if (hrs)
        printf("%ld:%02ld", hrs, mins);
    else
        printf("%ld min", mins);
    printf(", %d processes\n", count_procs());
    return 0;
}
