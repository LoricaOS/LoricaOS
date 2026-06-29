/* kill — send a signal to a process (or process group).
 *
 *   kill PID...              send SIGTERM
 *   kill -9 PID...           send by number
 *   kill -KILL PID...        send by name (SIG prefix optional)
 *   kill -s HUP PID...       POSIX -s form
 *   kill -l                  list signal names
 *   kill -l 9                name for a number
 *
 * Negative PID sends to the process group |PID| (kernel pgrp delivery).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <errno.h>

static const char *s_signames[32] = {
    NULL,     "HUP",  "INT",  "QUIT", "ILL",   "TRAP", "ABRT", "BUS",
    "FPE",    "KILL", "USR1", "SEGV", "USR2",  "PIPE", "ALRM", "TERM",
    "STKFLT", "CHLD", "CONT", "STOP", "TSTP",  "TTIN", "TTOU", "URG",
    "XCPU",   "XFSZ", "VTALRM", "PROF", "WINCH", "IO",  "PWR",  "SYS",
};

static int name_to_sig(const char *name)
{
    if (strncasecmp(name, "SIG", 3) == 0)
        name += 3;
    for (int i = 1; i < 32; i++)
        if (s_signames[i] && strcasecmp(name, s_signames[i]) == 0)
            return i;
    return -1;
}

static int parse_sig(const char *arg)
{
    if (isdigit((unsigned char)arg[0])) {
        int n = atoi(arg);
        return (n >= 0 && n < 64) ? n : -1;
    }
    return name_to_sig(arg);
}

static void list_signals(void)
{
    for (int i = 1; i < 32; i++) {
        printf("%2d) SIG%-7s", i, s_signames[i]);
        if (i % 4 == 0) printf("\n");
    }
    printf("\n");
}

static void usage(void)
{
    fprintf(stderr,
        "usage: kill [-SIGNAL | -s SIGNAL] pid...\n"
        "       kill -l [number]\n");
    exit(1);
}

int main(int argc, char **argv)
{
    int sig = SIGTERM;
    int i = 1;

    if (argc < 2) usage();

    if (strcmp(argv[1], "-l") == 0) {
        if (argc >= 3) {
            int n = atoi(argv[2]);
            if (n >= 1 && n < 32 && s_signames[n]) {
                printf("%s\n", s_signames[n]);
                return 0;
            }
            fprintf(stderr, "kill: %s: invalid signal number\n", argv[2]);
            return 1;
        }
        list_signals();
        return 0;
    }

    if (strcmp(argv[1], "-s") == 0) {
        if (argc < 4) usage();
        sig = parse_sig(argv[2]);
        if (sig < 0) {
            fprintf(stderr, "kill: %s: invalid signal\n", argv[2]);
            return 1;
        }
        i = 3;
    } else if (argv[1][0] == '-' && argv[1][1] != '\0') {
        /* -9 / -TERM / -SIGTERM. "-<digits>" in argv[1] is a signal, not
         * a negative pid — matching coreutils kill; pass a pgrp target
         * as a later argument ("kill -TERM -- -5" is not supported, use
         * "kill -15 -5" with the group second). */
        sig = parse_sig(argv[1] + 1);
        if (sig < 0) {
            fprintf(stderr, "kill: %s: invalid signal\n", argv[1] + 1);
            return 1;
        }
        i = 2;
    }

    if (i >= argc) usage();

    int rc = 0;
    for (; i < argc; i++) {
        char *end = NULL;
        long pid = strtol(argv[i], &end, 10);
        if (end == argv[i] || *end != '\0') {
            fprintf(stderr, "kill: %s: invalid pid\n", argv[i]);
            rc = 1;
            continue;
        }
        if (kill((pid_t)pid, sig) != 0) {
            fprintf(stderr, "kill: (%ld): %s\n", pid, strerror(errno));
            rc = 1;
        }
    }
    return rc;
}
