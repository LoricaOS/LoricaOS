/* free — memory usage from /proc/meminfo.
 *
 *   free          kibibytes (default, like procps)
 *   free -b/-k/-m/-g   fixed units
 *   free -h       human-readable
 *   free -t       add a Total row
 *
 * Aegis has no swap and no page cache accounting yet, so used is simply
 * total - free and the Swap row is all zeros (kept for script compat).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static long meminfo_kb(const char *buf, const char *key)
{
    const char *p = strstr(buf, key);
    if (!p) return -1;
    p += strlen(key);
    while (*p == ':' || *p == ' ' || *p == '\t') p++;
    return atol(p);
}

static void fmt(char *out, size_t cap, long kb, int unit, int human)
{
    if (human) {
        if (kb < 1024)
            snprintf(out, cap, "%ldK", kb);
        else if (kb < 1024L * 1024)
            snprintf(out, cap, "%ld.%ldM", kb / 1024,
                     (kb % 1024) * 10 / 1024);
        else
            snprintf(out, cap, "%ld.%ldG", kb / (1024L * 1024),
                     (kb % (1024L * 1024)) * 10 / (1024L * 1024));
        return;
    }
    long v = kb;
    switch (unit) {
    case 'b': v = kb * 1024;          break;
    case 'm': v = kb / 1024;          break;
    case 'g': v = kb / (1024L * 1024); break;
    default:  break; /* kB */
    }
    snprintf(out, cap, "%ld", v);
}

int main(int argc, char **argv)
{
    int unit = 'k', human = 0, total_row = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-b") == 0)      unit = 'b';
        else if (strcmp(argv[i], "-k") == 0) unit = 'k';
        else if (strcmp(argv[i], "-m") == 0) unit = 'm';
        else if (strcmp(argv[i], "-g") == 0) unit = 'g';
        else if (strcmp(argv[i], "-h") == 0) human = 1;
        else if (strcmp(argv[i], "-t") == 0) total_row = 1;
        else {
            fprintf(stderr, "usage: free [-b|-k|-m|-g|-h] [-t]\n");
            return 1;
        }
    }

    char buf[512];
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) { perror("/proc/meminfo"); return 1; }
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    long total = meminfo_kb(buf, "MemTotal");
    long freek = meminfo_kb(buf, "MemFree");
    long avail = meminfo_kb(buf, "MemAvailable");
    if (total < 0 || freek < 0) {
        fprintf(stderr, "free: cannot parse /proc/meminfo\n");
        return 1;
    }
    if (avail < 0) avail = freek;
    long used = total - freek;

    char st[24], su[24], sf[24], sa[24];
    fmt(st, sizeof(st), total, unit, human);
    fmt(su, sizeof(su), used,  unit, human);
    fmt(sf, sizeof(sf), freek, unit, human);
    fmt(sa, sizeof(sa), avail, unit, human);

    printf("%14s %11s %11s %11s\n", "total", "used", "free", "available");
    printf("Mem:  %8s %11s %11s %11s\n", st, su, sf, sa);
    printf("Swap: %8s %11s %11s\n", "0", "0", "0");
    if (total_row)
        printf("Total:%8s %11s %11s\n", st, su, sf);
    return 0;
}
