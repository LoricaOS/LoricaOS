/* lumen-probe — wait for /run/lumen.sock to exist, then call
 * lumen_connect() and print the result. Used by the
 * lumen_connect_probe_test and lumen_proxy_uaf_test regression tests
 * to verify the AF_UNIX handshake + proxy-window teardown end-to-end.
 *
 * Launched automatically by the `lumen-probe` vigil graphical-mode
 * oneshot service (rootfs/etc/vigil/services/lumen-probe/), but only
 * actually runs when /proc/cmdline carries the `bastion_autologin`
 * test hook (i.e. aegis-installer-test.iso). On production graphical
 * boots it exits silently so users never see the probe window. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <lumen_client.h>

static void
log_console(const char *msg)
{
    write(2, msg, strlen(msg));
    int cfd = open("/dev/console", O_WRONLY);
    if (cfd >= 0) { write(cfd, msg, strlen(msg)); close(cfd); }
}

int main(void)
{
    /* Test-only gate: run only on test ISOs (kernel cmdline carries
     * the bastion_autologin hook — production ISOs never set it, see
     * CLAUDE.md "v1.0.2 invariants"). Exit silently otherwise. */
    {
        char cmd[256];
        int cfd = open("/proc/cmdline", O_RDONLY);
        ssize_t cn = (cfd >= 0) ? read(cfd, cmd, sizeof(cmd) - 1) : -1;
        if (cfd >= 0) close(cfd);
        if (cn <= 0) return 0;
        cmd[cn] = '\0';
        if (!strstr(cmd, "bastion_autologin")) return 0;
    }

    /* Wait briefly so Bastion has time to authenticate and spawn Lumen
     * before we start hammering connect().  Without this delay the probe
     * starts retrying before Lumen's listening socket exists, polluting
     * the serial console with ECONNREFUSED spam during Bastion login. */
    {
        struct timespec wait = { 3, 0 };
        nanosleep(&wait, NULL);
    }

    log_console("[PROBE] starting\n");

    /* Aegis AF_UNIX bind does not create a filesystem entry — the
     * path lives in an in-kernel name table — so we cannot stat()
     * for readiness. Retry lumen_connect until it succeeds (or 30s). */
    char buf[96];
    int fd = -1;
    int attempts = 0;
    for (attempts = 0; attempts < 300; attempts++) {
        fd = lumen_connect();
        if (fd >= 0) break;
        /* fd is -errno here. ECONNREFUSED (-111) means listener not
         * up yet; anything else is a real failure we want to surface. */
        if (fd != -111) {
            int n = snprintf(buf, sizeof(buf),
                "[PROBE] lumen_connect returned %d on attempt %d (not retrying)\n",
                fd, attempts);
            if (n > 0) log_console(buf);
            break;
        }
        struct timespec ts = { 0, 100 * 1000 * 1000 };  /* 100ms */
        nanosleep(&ts, NULL);
    }

    int n = snprintf(buf, sizeof(buf),
        "[PROBE] lumen_connect=%d after %d attempts\n", fd, attempts);
    if (n > 0) log_console(buf);

    if (fd < 0) {
        n = snprintf(buf, sizeof(buf), "[PROBE] FAIL errno-equivalent=%d\n", -fd);
        if (n > 0) log_console(buf);
        return 1;
    }

    /* If connect worked, try creating a window too */
    lumen_window_t *win = lumen_window_create(fd, "Probe", 200, 100);
    if (!win) {
        log_console("[PROBE] FAIL: window_create returned NULL\n");
        close(fd);
        return 1;
    }
    n = snprintf(buf, sizeof(buf), "[PROBE] window_create OK: id=%u %dx%d\n",
                 win->id, win->w, win->h);
    if (n > 0) log_console(buf);

    /* Render solid color and present once */
    uint32_t *px = (uint32_t *)win->backbuf;
    for (int i = 0; i < win->w * win->h; i++) px[i] = 0x000080FFu;
    lumen_window_present(win);

    log_console("[PROBE] PASS\n");

    /* Keep window visible briefly so test can capture screendump if desired */
    struct timespec ts = { 2, 0 };
    nanosleep(&ts, NULL);

    lumen_window_destroy(win);
    close(fd);
    return 0;
}
