/* aegisctl — headless admin configuration for system-wide settings.
 *
 *   aegisctl autologin on|off    enable/disable passwordless login (bastion)
 *   aegisctl ntp on|off          enable/disable automatic time sync (chronos)
 *
 * These mirror the Settings → Users / Date & Time toggles. The kernel gates
 * the writes on an authenticated root session (POWER cap + auth_uid == 0), so
 * a non-root caller gets EPERM. Useful for scripted / headless setups where
 * there is no GUI.
 */
#include <sys/syscall.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

#define SYS_SET_AUTOLOGIN 501
#define SYS_SET_NTP       502

static int onoff(const char *s)
{
    if (strcmp(s, "on") == 0)  return 1;
    if (strcmp(s, "off") == 0) return 0;
    return -1;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: aegisctl <autologin|ntp> <on|off>\n");
        return 2;
    }

    int val = onoff(argv[2]);
    if (val < 0) {
        fprintf(stderr, "aegisctl: expected 'on' or 'off', got '%s'\n", argv[2]);
        return 2;
    }

    long num;
    if (strcmp(argv[1], "autologin") == 0)
        num = SYS_SET_AUTOLOGIN;
    else if (strcmp(argv[1], "ntp") == 0)
        num = SYS_SET_NTP;
    else {
        fprintf(stderr, "aegisctl: unknown setting '%s'\n", argv[1]);
        return 2;
    }

    if (syscall(num, (long)val) < 0) {
        fprintf(stderr, "aegisctl: %s %s failed (must be root)\n",
                argv[1], argv[2]);
        return 1;
    }

    printf("aegisctl: %s %s\n", argv[1], argv[2]);
    return 0;
}
