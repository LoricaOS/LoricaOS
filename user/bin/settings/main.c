/* user/bin/settings/main.c — Aegis Settings app (external Lumen client)
 *
 * A two-pane System-Settings app: a left sidebar of categories + a right
 * content pane rendered as cards. Each pane mixes the controls Aegis can
 * actually drive (rendered live) with greyed "(Future Development)" rows
 * for capabilities the system does not expose yet — so the UI maps out the
 * intended surface without pretending to control things it can't.
 *
 * Categories:
 *   - System:      edition, kernel, processor, cores, memory, uptime, arch,
 *                  + the hostname editor (admin POWER cap).
 *   - Display:     resolution / color depth (real) + scaling/brightness/...
 *   - Appearance:  accent picker (real) + theme-mode/wallpaper/font (future).
 *   - Sound:       output/input (future — HDA exists, no mixer interface yet).
 *   - Network:     live status from sys_netcfg + "Open Network Manager"
 *                  (lumen_invoke) + Wi-Fi/VPN/proxy (future).
 *   - Date & Time: live clock + date (real) + timezone/NTP toggle (future).
 *   - Input:       keyboard / mouse tuning (future).
 *   - Users:       current user / uid / hostname (real) + management (future).
 *   - Storage:     mounted filesystems from /proc/mounts (real) + usage (future).
 *   - Power:       Restart / Power Off (real) + sleep/battery (future).
 *   - Privacy:     the capability security model (real facts) + permissions.
 *   - About:       Aegis / version / hardware summary.
 *
 * Real data sources: /proc/{version,meminfo,cpuinfo,uptime,mounts}, uname,
 * getuid, clock_gettime, sys_netcfg (syscall 500), LUMEN_FB_W/H. Nothing is
 * invented — panes show exactly what the kernel reports.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <stdint.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/syscall.h>
#include <sys/utsname.h>

#include <glyph.h>
#include <lumen_client.h>
#include "apps.h"
#include "font.h"

#define SYS_SETHOSTNAME 170
#define SYS_REBOOT      169
#define SYS_NETCFG      500
#define SYS_SET_AUTOLOGIN 501
#define SYS_SET_NTP       502
#define HOSTNAME_MAX    64   /* matches kernel/syscall/sys_hostname.c */

/* Mirrors kernel netcfg_info_t (kernel/syscall/sys_socket.c). */
typedef struct {
    uint8_t  mac[6];
    uint8_t  pad[2];
    uint32_t ip;
    uint32_t mask;
    uint32_t gateway;
} netcfg_info_t;

/* ── Window + pane geometry ─────────────────────────────────────────────── */

#define WIN_W       720
#define WIN_H       600

#define SIDEBAR_W   180
#define ROW_H       38   /* sidebar row height          */
#define ROW_GAP     SP_1 /* gap between sidebar rows     */
#define SIDEBAR_PAD SP_3 /* sidebar inset               */

#define CONTENT_X   (SIDEBAR_W + SP_5)   /* left inset of the content pane */
#define CONTENT_TOP SP_5                 /* top inset of the content pane  */

#define CARD_R      R_MD
#define CARD_PAD    SP_4

#define FS_BODY     TYPE_BODY     /* 14 */
#define FS_TITLE    TYPE_TITLE    /* 16 */
#define FS_DISPLAY  TYPE_DISPLAY  /* 20 */

/* One label/value row height, and the title block above a card's first row. */
#define KV_ROW_H    (FS_BODY + SP_3)
#define CARD_HEAD   (CARD_PAD + FS_TITLE + SP_3)

/* Synthetic arrow-key codes from Lumen's CSI translator (lumen/main.c). */
#define KEY_ARROW_UP    ((char)0xF1)
#define KEY_ARROW_DOWN  ((char)0xF2)
#define KEY_ARROW_RIGHT ((char)0xF3)
#define KEY_ARROW_LEFT  ((char)0xF4)

/* ── Categories ─────────────────────────────────────────────────────────── */

enum {
    CAT_SYSTEM = 0,
    CAT_DISPLAY,
    CAT_APPEARANCE,
    CAT_SOUND,
    CAT_NETWORK,
    CAT_DATETIME,
    CAT_INPUT,
    CAT_USERS,
    CAT_STORAGE,
    CAT_POWER,
    CAT_PRIVACY,
    CAT_ABOUT,
    CAT_COUNT
};

static const char *const CATEGORY_NAMES[CAT_COUNT] = {
    "System", "Display", "Appearance", "Sound", "Network", "Date & Time",
    "Input", "Users", "Storage", "Power", "Privacy", "About",
};

/* ── App state ──────────────────────────────────────────────────────────── */

#define MAX_MOUNTS 6

typedef struct {
    int             lfd;
    lumen_window_t *lwin;
    surface_t       surf;
    int             fb_w, fb_h;

    int selected;
    int field_focused;
    int dirty;
    int done;

    /* System info */
    char kernel_version[80];
    char mem_line[80];
    char display_line[48];
    char processor[64];
    char cores[16];
    char uptime_line[48];
    char arch_line[24];

    /* Users */
    char user_line[40];
    char uid_line[24];

    /* Date & time */
    char time_line[24];
    char date_line[48];

    /* Storage */
    char     mount_mp[MAX_MOUNTS][40];
    char     mount_fs[MAX_MOUNTS][24];
    uint64_t mount_total[MAX_MOUNTS];   /* kB; 0 = not reported (ramfs) */
    uint64_t mount_free[MAX_MOUNTS];    /* kB                          */
    int      n_mounts;

    /* SMART / drive health (/proc/smart) */
    int  smart_present;
    char smart_health[16];
    char smart_temp[8];
    char smart_spare[8];
    char smart_used[8];
    char smart_poh[16];
    char smart_cycles[16];

    /* Admin system settings (file-backed, written via admin syscalls) */
    int ntp_on;            /* /etc/aegis/ntp        */
    int autologin_on;      /* /etc/aegis/autologin  */

    /* Network */
    netcfg_info_t net;
    int           have_net;

    /* Hostname */
    char current_hostname[HOSTNAME_MAX + 1];
    char edit[HOSTNAME_MAX + 1];

    char     status[96];
    uint32_t status_color;

    /* Interactive hit rects (cleared each render). */
    int field_x, field_y, field_w, field_h;
    int apply_x, apply_y, apply_w, apply_h;
    int restart_x, restart_y, restart_w, restart_h;
    int poweroff_x, poweroff_y, poweroff_w, poweroff_h;
    int netman_x, netman_y, netman_w, netman_h;
    int anim_x, anim_y, anim_w, anim_h;       /* Appearance: animations toggle */
    int clk_x, clk_y, clk_w, clk_h;           /* Date & Time: 24-hour toggle   */
    int tzp_x, tzp_y, tzp_w, tzp_h;           /* Date & Time: tz prev           */
    int tzn_x, tzn_y, tzn_w, tzn_h;           /* Date & Time: tz next           */
    int nat_x, nat_y, nat_w, nat_h;           /* Input: natural-scroll toggle   */
    int lock_x, lock_y, lock_w, lock_h;       /* Privacy: lock screen           */
    int lm_x, lm_y, lm_w, lm_h;               /* Appearance: light-mode toggle  */
    int wpp_x, wpp_y, wpp_w, wpp_h;           /* Appearance: wallpaper prev      */
    int wpn_x, wpn_y, wpn_w, wpn_h;           /* Appearance: wallpaper next      */
    int nl_x, nl_y, nl_w, nl_h;               /* Display: night-light toggle     */
    int psp_x, psp_y, psp_w, psp_h;           /* Input: pointer-speed prev       */
    int psn_x, psn_y, psn_w, psn_h;           /* Input: pointer-speed next       */
    int ntpt_x, ntpt_y, ntpt_w, ntpt_h;       /* Date & Time: NTP toggle         */
    int al_x, al_y, al_w, al_h;               /* Users: automatic-login toggle   */
    int tz_idx;                               /* selected timezone index        */
    int controls_live;
} settings_state_t;

static settings_state_t g_st;

static volatile sig_atomic_t s_term_requested;
static void sigterm_handler(int sig) { (void)sig; s_term_requested = 1; }

/* ── App permissions cache (Privacy pane) ───────────────────────────────────
 * For each installed /apps bundle we record its display name and the
 * capability policy the kernel applies at launch (from /etc/aegis/caps.d/).
 * Read-only: this is a security *inspector*, mirroring exactly what the
 * cap-policy layer grants — it cannot change a grant (that's by design;
 * policy lives in the install-protected /etc/aegis tree). */
#define PERM_MAX_APPS   16
#define PERM_MAX_CAPS   6

typedef struct {
    char name[40];                    /* display name from app.ini */
    char tier[8];                     /* "service" | "admin"        */
    char caps[PERM_MAX_CAPS][16];     /* granted cap names           */
    int  n_caps;
    int  admin;                       /* 1 = caps require auth (admin tier) */
} app_perm_t;

static app_perm_t g_perms[PERM_MAX_APPS];
static int        g_n_perms;

static int read_first_line(const char *path, char *out, size_t n);  /* fwd */

/* Parse one caps.d policy line ("<tier> CAP CAP ...") into *p. */
static void parse_caps_line(const char *line, app_perm_t *p)
{
    p->n_caps = 0;
    p->admin  = 0;
    p->tier[0] = '\0';

    const char *s = line;
    char tok[24];
    int first = 1;
    while (*s) {
        while (*s == ' ' || *s == '\t') s++;
        int i = 0;
        while (*s && *s != ' ' && *s != '\t' && *s != '\n' && i < (int)sizeof(tok) - 1)
            tok[i++] = *s++;
        tok[i] = '\0';
        if (i == 0) break;

        if (first) {
            snprintf(p->tier, sizeof(p->tier), "%.7s", tok);
            p->admin = (strcmp(tok, "admin") == 0);
            first = 0;
        } else if (p->n_caps < PERM_MAX_CAPS) {
            snprintf(p->caps[p->n_caps], sizeof(p->caps[0]), "%.15s", tok);
            p->n_caps++;
        }
    }
}

static void load_app_perms(void)
{
    g_n_perms = 0;

    glyph_app_t apps[GLYPH_APPS_MAX];
    int n = glyph_apps_scan(apps, GLYPH_APPS_MAX);

    for (int i = 0; i < n && g_n_perms < PERM_MAX_APPS; i++) {
        /* caps.d is keyed by the executable basename. */
        const char *base = strrchr(apps[i].exec, '/');
        base = base ? base + 1 : apps[i].exec;

        char path[80];
        snprintf(path, sizeof(path), "/etc/aegis/caps.d/%s", base);
        char line[160] = {0};

        app_perm_t *p = &g_perms[g_n_perms];
        snprintf(p->name, sizeof(p->name), "%s", apps[i].name);

        if (read_first_line(path, line, sizeof(line)) == 0)
            parse_caps_line(line, p);
        else
            parse_caps_line("service", p);   /* no policy → baseline only */

        g_n_perms++;
    }
}

/* ── System info readers ────────────────────────────────────────────────── */

static int read_first_line(const char *path, char *out, size_t n)
{
    if (n == 0) return -1;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t r = read(fd, out, n - 1);
    close(fd);
    if (r <= 0) return -1;
    out[r] = '\0';
    char *nl = strchr(out, '\n');
    if (nl) *nl = '\0';
    return 0;
}

static void read_proc_version(void)
{
    if (read_first_line("/proc/version", g_st.kernel_version,
                        sizeof(g_st.kernel_version)) != 0)
        snprintf(g_st.kernel_version, sizeof(g_st.kernel_version),
                 "(unavailable)");
}

static unsigned long long meminfo_kb(const char *buf, const char *key)
{
    const char *p = strstr(buf, key);
    if (!p) return 0;
    p += strlen(key);
    while (*p == ' ') p++;
    unsigned long long v = 0;
    while (*p >= '0' && *p <= '9') { v = v * 10ULL + (*p - '0'); p++; }
    return v;
}

static void read_meminfo(void)
{
    g_st.mem_line[0] = '\0';
    int fd = open("/proc/meminfo", O_RDONLY);
    if (fd < 0) { snprintf(g_st.mem_line, sizeof(g_st.mem_line), "(unavailable)"); return; }
    char buf[512];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) { snprintf(g_st.mem_line, sizeof(g_st.mem_line), "(unavailable)"); return; }
    buf[n] = '\0';
    unsigned long long total_kb = meminfo_kb(buf, "MemTotal:");
    unsigned long long free_kb  = meminfo_kb(buf, "MemFree:");
    snprintf(g_st.mem_line, sizeof(g_st.mem_line),
             "%llu MB total, %llu MB free", total_kb / 1024, free_kb / 1024);
}

/* Pull "<key>...: <value>" out of /proc/cpuinfo. */
static int cpuinfo_field(const char *buf, const char *key, char *out, size_t n)
{
    const char *p = strstr(buf, key);
    if (!p) return -1;
    p = strchr(p, ':');
    if (!p) return -1;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    size_t i = 0;
    while (*p && *p != '\n' && i < n - 1) out[i++] = *p++;
    out[i] = '\0';
    return 0;
}

static void read_cpuinfo(void)
{
    snprintf(g_st.processor, sizeof(g_st.processor), "(unknown)");
    snprintf(g_st.cores, sizeof(g_st.cores), "?");
    int fd = open("/proc/cpuinfo", O_RDONLY);
    if (fd < 0) return;
    char buf[512];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return;
    buf[n] = '\0';
    cpuinfo_field(buf, "model name", g_st.processor, sizeof(g_st.processor));
    cpuinfo_field(buf, "cpus", g_st.cores, sizeof(g_st.cores));
}

static void read_uptime(void)
{
    snprintf(g_st.uptime_line, sizeof(g_st.uptime_line), "(unknown)");
    int fd = open("/proc/uptime", O_RDONLY);
    if (fd < 0) return;
    char buf[64];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return;
    buf[n] = '\0';
    unsigned long long secs = 0;
    const char *p = buf;
    while (*p >= '0' && *p <= '9') { secs = secs * 10ULL + (*p - '0'); p++; }

    unsigned long long d = secs / 86400, h = (secs % 86400) / 3600,
                       m = (secs % 3600) / 60, s = secs % 60;
    if (d)
        snprintf(g_st.uptime_line, sizeof(g_st.uptime_line),
                 "%llud %lluh %llum", d, h, m);
    else if (h)
        snprintf(g_st.uptime_line, sizeof(g_st.uptime_line),
                 "%lluh %llum", h, m);
    else if (m)
        snprintf(g_st.uptime_line, sizeof(g_st.uptime_line),
                 "%llum %llus", m, s);
    else
        snprintf(g_st.uptime_line, sizeof(g_st.uptime_line), "%llus", s);
}

static void read_hostname(void)
{
    struct utsname u;
    if (uname(&u) == 0) {
        snprintf(g_st.current_hostname, sizeof(g_st.current_hostname),
                 "%s", u.nodename);
        snprintf(g_st.arch_line, sizeof(g_st.arch_line), "%.23s", u.machine);
    } else {
        snprintf(g_st.current_hostname, sizeof(g_st.current_hostname), "(unknown)");
        snprintf(g_st.arch_line, sizeof(g_st.arch_line), "x86_64");
    }
}

static void read_user(void)
{
    const char *u = getenv("USER");
    if (!u || !*u) u = "root";
    snprintf(g_st.user_line, sizeof(g_st.user_line), "%s", u);
    snprintf(g_st.uid_line, sizeof(g_st.uid_line), "%u", (unsigned)getuid());
}

static void read_display(void)
{
    const char *w = getenv("LUMEN_FB_W");
    const char *h = getenv("LUMEN_FB_H");
    if (w && h)
        snprintf(g_st.display_line, sizeof(g_st.display_line), "%sx%s", w, h);
    else
        snprintf(g_st.display_line, sizeof(g_st.display_line),
                 "%dx%d", g_st.fb_w, g_st.fb_h);
}

/* Curated fixed-offset timezones. Aegis has no zoneinfo DB, so a zone is just
 * a UTC offset in minutes applied to the (UTC) wall clock — honest and DST-free. */
static const struct { const char *name; int off; } TZONES[] = {
    { "UTC-08 Pacific",   -480 },
    { "UTC-07 Mountain",  -420 },
    { "UTC-06 Central",   -360 },
    { "UTC-05 Eastern",   -300 },
    { "UTC-04 Atlantic",  -240 },
    { "UTC-03 Brasilia",  -180 },
    { "UTC+00 London",       0 },
    { "UTC+01 Berlin",      60 },
    { "UTC+02 Athens",     120 },
    { "UTC+03 Moscow",     180 },
    { "UTC+05:30 India",   330 },
    { "UTC+08 China",      480 },
    { "UTC+09 Tokyo",      540 },
    { "UTC+10 Sydney",     600 },
};
static const int N_TZONES = (int)(sizeof(TZONES) / sizeof(TZONES[0]));

/* Pointer-speed presets (percent multiplier; 150 = historical 1.5x). */
static const struct { const char *name; int pct; } PSPEEDS[] = {
    { "Slow", 100 }, { "Normal", 150 }, { "Fast", 250 },
};
static const int N_PSPEEDS = (int)(sizeof(PSPEEDS) / sizeof(PSPEEDS[0]));

static int pspeed_idx(void)
{
    for (int i = 0; i < N_PSPEEDS; i++)
        if (PSPEEDS[i].pct == glyph_theme_pointer_speed()) return i;
    return 1;   /* Normal */
}

static void read_datetime(void)
{
    /* Apply the timezone offset to UTC, then format as UTC wall time. */
    time_t now = time(NULL) + (time_t)glyph_theme_tz_offset() * 60;
    struct tm tmv;
    if (!gmtime_r(&now, &tmv)) {
        snprintf(g_st.time_line, sizeof(g_st.time_line), "--:--:--");
        snprintf(g_st.date_line, sizeof(g_st.date_line), "(no clock)");
        return;
    }
    if (glyph_theme_clock24()) {
        strftime(g_st.time_line, sizeof(g_st.time_line), "%H:%M:%S", &tmv);
    } else {
        int h12 = tmv.tm_hour % 12;
        if (h12 == 0) h12 = 12;
        snprintf(g_st.time_line, sizeof(g_st.time_line), "%d:%02d:%02d %s",
                 h12, tmv.tm_min, tmv.tm_sec, tmv.tm_hour < 12 ? "AM" : "PM");
    }
    strftime(g_st.date_line, sizeof(g_st.date_line), "%A, %B %d, %Y", &tmv);
}

static void read_mounts(void)
{
    g_st.n_mounts = 0;
    int fd = open("/proc/mounts", O_RDONLY);
    if (fd < 0) return;
    char buf[1024];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return;
    buf[n] = '\0';

    char *line = buf;
    while (line && *line && g_st.n_mounts < MAX_MOUNTS) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        /* fields: device mountpoint fstype total_kb free_kb
         * (the kernel's /proc/mounts carries per-mount totals; ramfs lines
         *  report "0 0" = size unknown). */
        char dev[40] = {0}, mp[40] = {0}, fs[24] = {0};
        unsigned long long tot = 0, fr = 0;
        int got = sscanf(line, "%39s %39s %23s %llu %llu", dev, mp, fs, &tot, &fr);
        if (got >= 2) {
            int k = g_st.n_mounts;
            snprintf(g_st.mount_mp[k], sizeof(g_st.mount_mp[0]), "%s", mp);
            snprintf(g_st.mount_fs[k], sizeof(g_st.mount_fs[0]),
                     "%s", fs[0] ? fs : "fs");
            g_st.mount_total[k] = (got >= 4) ? tot : 0;
            g_st.mount_free[k]  = (got >= 5) ? fr  : 0;
            g_st.n_mounts++;
        }
        line = nl ? nl + 1 : NULL;
    }
}

static void read_net(void)
{
    g_st.have_net = (syscall(SYS_NETCFG, 1, (long)&g_st.net, 0, 0) == 0);
}

/* Admin system settings, read from their /etc/aegis config files.
 * NTP defaults on (missing file = enabled); autologin defaults off. */
static void read_admin_state(void)
{
    g_st.ntp_on = 1;
    g_st.autologin_on = 0;

    char buf[64];
    if (read_first_line("/etc/aegis/ntp", buf, sizeof(buf)) == 0) {
        if (buf[0] == 'o' && buf[1] == 'f' && buf[2] == 'f')
            g_st.ntp_on = 0;
    }
    if (read_first_line("/etc/aegis/autologin", buf, sizeof(buf)) == 0)
        g_st.autologin_on = (buf[0] != '\0');
}

/* Extract a "<key> <value>" field (key includes the trailing ':'). */
static void smart_field(const char *buf, const char *key, char *out, size_t n)
{
    out[0] = '\0';
    const char *p = strstr(buf, key);
    if (!p) return;
    p += strlen(key);
    while (*p == ' ' || *p == '\t') p++;
    size_t i = 0;
    while (*p && *p != '\n' && i < n - 1) out[i++] = *p++;
    out[i] = '\0';
}

static void read_smart(void)
{
    g_st.smart_present = 0;
    int fd = open("/proc/smart", O_RDONLY);
    if (fd < 0) return;
    char buf[256];
    ssize_t nb = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (nb <= 0) return;
    buf[nb] = '\0';

    char dev[16];
    smart_field(buf, "device:", dev, sizeof(dev));
    if (dev[0] == '\0' || strcmp(dev, "none") == 0)
        return;   /* no NVMe controller */

    g_st.smart_present = 1;
    smart_field(buf, "health:",       g_st.smart_health, sizeof(g_st.smart_health));
    smart_field(buf, "temperature:",  g_st.smart_temp,   sizeof(g_st.smart_temp));
    smart_field(buf, "spare:",        g_st.smart_spare,  sizeof(g_st.smart_spare));
    smart_field(buf, "used:",         g_st.smart_used,   sizeof(g_st.smart_used));
    smart_field(buf, "poweronhours:", g_st.smart_poh,    sizeof(g_st.smart_poh));
    smart_field(buf, "powercycles:",  g_st.smart_cycles, sizeof(g_st.smart_cycles));
}

static void fmt_ip(uint32_t addr, char *out, size_t n)
{
    const uint8_t *b = (const uint8_t *)&addr;
    snprintf(out, n, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

/* ── Drawing helpers ────────────────────────────────────────────────────── */

static int text_width(int size_px, const char *s)
{
    if (g_font_ui) return font_text_width(g_font_ui, size_px, s);
    return glyph_text_width(s);
}

static void draw_text_sz(int size_px, int x, int y, const char *s, uint32_t color)
{
    if (g_font_ui) font_draw_text(&g_st.surf, g_font_ui, size_px, x, y, s, color);
    else           draw_text_t(&g_st.surf, x, y, s, color);
}

static void draw_button(int x, int y, int w, int h, const char *label, uint32_t bg)
{
    draw_rounded_rect(&g_st.surf, x, y, w, h, R_SM, bg);
    int tw = text_width(FS_BODY, label);
    draw_text_sz(FS_BODY, x + (w - tw) / 2, y + (h - FS_BODY) / 2 - 1,
                 label, THEME_TEXT_ON_ACCENT);
}

static void draw_dot(int x, int y, int d, uint32_t color)
{
    draw_rounded_rect(&g_st.surf, x, y, d, d, d / 2, color);
}

/* An on/off toggle switch (track + knob), iOS/GNOME style. */
#define TOG_W      44
#define TOG_H      24
#define TOG_ROW_H  30

static void draw_toggle(int x, int y, int w, int h, int on)
{
    surface_t *s = &g_st.surf;
    draw_rounded_rect(s, x, y, w, h, h / 2,
                      on ? THEME_ACCENT : THEME_BORDER_STRONG);
    int kd = h - 6;
    int kx = on ? x + w - 3 - kd : x + 3;
    draw_rounded_rect(s, kx, y + 3, kd, kd, kd / 2, THEME_TEXT_ON_ACCENT);
}

/* A small square stepper button with a centered glyph ("<"/">"). */
static void draw_stepper(int x, int y, int d, const char *glyph)
{
    draw_rounded_rect(&g_st.surf, x, y, d, d, R_SM, THEME_HOVER);
    int gw = text_width(FS_BODY, glyph);
    draw_text_sz(FS_BODY, x + (d - gw) / 2, y + (d - FS_BODY) / 2 - 1, glyph,
                 THEME_TEXT);
}

/* A "label .......... [toggle]" row of height TOG_ROW_H. Fills the rect. */
static void draw_toggle_row(int cx, int cw, int row_y, const char *label, int on,
                            int *tx, int *ty, int *tw, int *th)
{
    draw_text_sz(FS_BODY, cx + CARD_PAD, row_y + (TOG_ROW_H - FS_BODY) / 2 - 1,
                 label, THEME_TEXT);
    int x = cx + cw - CARD_PAD - TOG_W;
    int y = row_y + (TOG_ROW_H - TOG_H) / 2;
    draw_toggle(x, y, TOG_W, TOG_H, on);
    *tx = x; *ty = y; *tw = TOG_W; *th = TOG_H;
}

/* A "label   [<] value [>]" stepper row of height TOG_ROW_H. Fills prev/next. */
static void draw_stepper_row(int cx, int cw, int row_y, const char *label,
                             const char *value,
                             int *px, int *py, int *pw, int *ph,
                             int *nx, int *ny, int *nw, int *nh)
{
    int ty = row_y + (TOG_ROW_H - FS_BODY) / 2 - 1;
    draw_text_sz(FS_BODY, cx + CARD_PAD, ty, label, THEME_TEXT);
    int sd = 24;
    int sy = row_y + (TOG_ROW_H - sd) / 2;
    int nextx = cx + cw - CARD_PAD - sd;
    int vw = text_width(FS_BODY, value);
    int vx = nextx - SP_2 - vw;
    int prevx = vx - SP_2 - sd;
    draw_stepper(prevx, sy, sd, "<");
    draw_text_sz(FS_BODY, vx, ty, value, THEME_TEXT_DIM);
    draw_stepper(nextx, sy, sd, ">");
    *px = prevx; *py = sy; *pw = sd; *ph = sd;
    *nx = nextx; *ny = sy; *nw = sd; *nh = sd;
}

static int card_h_rows(int n)
{
    return CARD_HEAD + n * KV_ROW_H + (CARD_PAD - SP_3);
}

static int draw_card(int x, int y, int w, int h, const char *title)
{
    draw_rounded_rect(&g_st.surf, x, y, w, h, CARD_R, THEME_SURFACE_2);
    draw_text_sz(FS_TITLE, x + CARD_PAD, y + CARD_PAD - 2, title, THEME_TEXT);
    return y + CARD_HEAD;
}

static int draw_kv_row(int card_x, int card_w, int row_y,
                       const char *label, const char *value)
{
    draw_text_sz(FS_BODY, card_x + CARD_PAD, row_y, label, THEME_TEXT_DIM);
    int vw = text_width(FS_BODY, value);
    draw_text_sz(FS_BODY, card_x + card_w - CARD_PAD - vw, row_y, value,
                 THEME_TEXT);
    return row_y + KV_ROW_H;
}

/* A greyed, non-interactive row: label dim, "(Future Development)" faint. */
static int draw_future_row(int card_x, int card_w, int row_y, const char *label)
{
    static const char *fd = "(Future Development)";
    draw_text_sz(FS_BODY, card_x + CARD_PAD, row_y, label, THEME_TEXT_DIM);
    int vw = text_width(FS_BODY, fd);
    draw_text_sz(FS_BODY, card_x + card_w - CARD_PAD - vw, row_y, fd,
                 THEME_TEXT_FAINT);
    return row_y + KV_ROW_H;
}

/* ── Sidebar ────────────────────────────────────────────────────────────── */

static void sidebar_row_rect(int idx, int *x, int *y, int *w, int *h)
{
    *x = SIDEBAR_PAD;
    *y = SIDEBAR_PAD + idx * (ROW_H + ROW_GAP);
    *w = SIDEBAR_W - 2 * SIDEBAR_PAD;
    *h = ROW_H;
}

static void draw_sidebar(void)
{
    surface_t *s = &g_st.surf;
    draw_fill_rect(s, 0, 0, SIDEBAR_W, g_st.fb_h, THEME_SURFACE_2);
    draw_fill_rect(s, SIDEBAR_W - 1, 0, 1, g_st.fb_h, THEME_BORDER);

    for (int i = 0; i < CAT_COUNT; i++) {
        int rx, ry, rw, rh;
        sidebar_row_rect(i, &rx, &ry, &rw, &rh);
        int sel = (i == g_st.selected);
        if (sel)
            draw_rounded_rect(s, rx, ry, rw, rh, R_SM, THEME_ACCENT);
        uint32_t tc = sel ? THEME_TEXT_ON_ACCENT : THEME_TEXT;
        draw_text_sz(FS_BODY, rx + SP_3, ry + (rh - FS_BODY) / 2 - 1,
                     CATEGORY_NAMES[i], tc);
    }
}

/* ── Content panes ──────────────────────────────────────────────────────── */

static int content_x(void) { return CONTENT_X; }
static int content_w(void) { return g_st.fb_w - CONTENT_X - SP_5; }

static void render_system(void)
{
    surface_t *s = &g_st.surf;
    int cx = content_x(), cw = content_w();

    int cardh = card_h_rows(7);
    int y = draw_card(cx, CONTENT_TOP, cw, cardh, "System");
    y = draw_kv_row(cx, cw, y, "Edition",      "Aegis");
    y = draw_kv_row(cx, cw, y, "Kernel",       g_st.kernel_version);
    y = draw_kv_row(cx, cw, y, "Processor",    g_st.processor);
    y = draw_kv_row(cx, cw, y, "Cores",        g_st.cores);
    y = draw_kv_row(cx, cw, y, "Memory",       g_st.mem_line);
    y = draw_kv_row(cx, cw, y, "Uptime",       g_st.uptime_line);
    (void)draw_kv_row(cx, cw, y, "Architecture", g_st.arch_line);

    /* Hostname editor. */
    int field_h = 34, btn_h = 34;
    int host_y = CONTENT_TOP + cardh + SP_4;
    int host_card_h = CARD_PAD + FS_TITLE + SP_3 + field_h + SP_3
                    + FS_BODY + CARD_PAD;
    int hy = draw_card(cx, host_y, cw, host_card_h, "Hostname");

    int apply_w = 96;
    int field_w = cw - 2 * CARD_PAD - apply_w - SP_3;
    int field_x = cx + CARD_PAD;
    int apply_x = field_x + field_w + SP_3;

    draw_rounded_rect(s, field_x, hy, field_w, field_h, R_SM, THEME_INPUT_BG);
    if (g_st.field_focused)
        draw_rounded_outline(s, field_x, hy, field_w, field_h, R_SM, 2,
                             THEME_ACCENT);
    draw_text_sz(FS_BODY, field_x + SP_3, hy + (field_h - FS_BODY) / 2 - 1,
                 g_st.edit[0] ? g_st.edit : "hostname",
                 g_st.edit[0] ? THEME_TEXT : THEME_TEXT_FAINT);
    draw_button(apply_x, hy, apply_w, btn_h, "Apply", THEME_ACCENT);

    g_st.field_x = field_x; g_st.field_y = hy;
    g_st.field_w = field_w; g_st.field_h = field_h;
    g_st.apply_x = apply_x; g_st.apply_y = hy;
    g_st.apply_w = apply_w; g_st.apply_h = btn_h;

    if (g_st.status[0])
        draw_text_sz(FS_BODY, field_x, hy + field_h + SP_3, g_st.status,
                     g_st.status_color);

    g_st.controls_live = 1;
}

static void render_display(void)
{
    int cx = content_x(), cw = content_w();
    int cardh = card_h_rows(2);
    int y = draw_card(cx, CONTENT_TOP, cw, cardh, "Display");
    y = draw_kv_row(cx, cw, y, "Resolution",  g_st.display_line);
    (void)draw_kv_row(cx, cw, y, "Color Depth", "32-bit");

    int oy = CONTENT_TOP + cardh + SP_4;
    int oh = CARD_HEAD + 4 * KV_ROW_H + TOG_ROW_H + (CARD_PAD - SP_3);
    int fy = draw_card(cx, oy, cw, oh, "Options");
    fy = draw_future_row(cx, cw, fy, "Scale");
    fy = draw_future_row(cx, cw, fy, "Brightness");

    /* Night Light — real warm-cast toggle. */
    draw_toggle_row(cx, cw, fy, "Night Light", glyph_theme_night_light(),
                    &g_st.nl_x, &g_st.nl_y, &g_st.nl_w, &g_st.nl_h);
    fy += TOG_ROW_H;

    fy = draw_future_row(cx, cw, fy, "Orientation");
    (void)draw_future_row(cx, cw, fy, "Multiple Displays");

    g_st.controls_live = 1;
}

#define SWATCH_SZ  40

static void swatch_rect(int i, int *x, int *y, int *w, int *h)
{
    *x = content_x() + CARD_PAD + i * (SWATCH_SZ + SP_2);
    *y = CONTENT_TOP + CARD_PAD + FS_TITLE + SP_3;
    *w = SWATCH_SZ;
    *h = SWATCH_SZ;
}

static void render_appearance(void)
{
    surface_t *s = &g_st.surf;
    int cx = content_x(), cw = content_w();
    int n = glyph_theme_accent_count();
    int cur = glyph_theme_current_accent();

    int cardh = CARD_PAD + FS_TITLE + SP_3 + SWATCH_SZ + SP_4 + FS_BODY + CARD_PAD;
    (void)draw_card(cx, CONTENT_TOP, cw, cardh, "Accent Color");

    for (int i = 0; i < n; i++) {
        int sx, sy, sw2, sh2;
        swatch_rect(i, &sx, &sy, &sw2, &sh2);
        if (i == cur)
            draw_rounded_outline(s, sx - 3, sy - 3, sw2 + 6, sh2 + 6,
                                 R_SM + 3, 2, THEME_TEXT);
        draw_rounded_rect(s, sx, sy, sw2, sh2, R_SM, glyph_theme_accent_color(i));
    }

    int ty = CONTENT_TOP + CARD_PAD + FS_TITLE + SP_3 + SWATCH_SZ + SP_4;
    const char *cur_name = (cur >= 0) ? glyph_theme_accent_name(cur) : "custom";
    char line[80];
    snprintf(line, sizeof(line), "Accent: %s", cur_name);
    draw_text_sz(FS_BODY, cx + CARD_PAD, ty, line, THEME_TEXT_DIM);

    int oy = CONTENT_TOP + cardh + SP_4;
    int oh = CARD_HEAD + 3 * TOG_ROW_H + KV_ROW_H + (CARD_PAD - SP_3);
    int fy = draw_card(cx, oy, cw, oh, "Theme");

    /* Light Mode toggle. */
    draw_toggle_row(cx, cw, fy, "Light Mode", glyph_theme_light(),
                    &g_st.lm_x, &g_st.lm_y, &g_st.lm_w, &g_st.lm_h);
    fy += TOG_ROW_H;

    /* Wallpaper stepper. */
    int wi = glyph_theme_wallpaper();
    draw_stepper_row(cx, cw, fy, "Wallpaper", glyph_theme_wallpaper_name(wi),
                     &g_st.wpp_x, &g_st.wpp_y, &g_st.wpp_w, &g_st.wpp_h,
                     &g_st.wpn_x, &g_st.wpn_y, &g_st.wpn_w, &g_st.wpn_h);
    fy += TOG_ROW_H;

    fy = draw_future_row(cx, cw, fy, "Font Size");

    /* Animations toggle. */
    draw_toggle_row(cx, cw, fy, "Animations", glyph_theme_animations(),
                    &g_st.anim_x, &g_st.anim_y, &g_st.anim_w, &g_st.anim_h);

    g_st.controls_live = 1;
}

static void render_sound(void)
{
    int cx = content_x(), cw = content_w();
    int oh = card_h_rows(3);
    int y = draw_card(cx, CONTENT_TOP, cw, oh, "Output");
    y = draw_future_row(cx, cw, y, "Output Device");
    y = draw_future_row(cx, cw, y, "Volume");
    (void)draw_future_row(cx, cw, y, "Balance");

    int iy = CONTENT_TOP + oh + SP_4;
    int ih = card_h_rows(2);
    int fy = draw_card(cx, iy, cw, ih, "Input");
    fy = draw_future_row(cx, cw, fy, "Microphone");
    (void)draw_future_row(cx, cw, fy, "Input Volume");
}

static void render_network(void)
{
    int cx = content_x(), cw = content_w();
    int connected = g_st.have_net && g_st.net.ip != 0;
    int btn_h = 34, btn_w = 210;

    int cardh = CARD_HEAD + 3 * KV_ROW_H + SP_2 + btn_h + (CARD_PAD - SP_3);
    int y = draw_card(cx, CONTENT_TOP, cw, cardh, "Network");

    /* Status row with a colored dot. */
    const char *sv = connected ? "Connected" : "Not connected";
    draw_text_sz(FS_BODY, cx + CARD_PAD, y, "Status", THEME_TEXT_DIM);
    int vw = text_width(FS_BODY, sv);
    int total = 10 + SP_2 + vw;
    int sx = cx + cw - CARD_PAD - total;
    draw_dot(sx, y + 1, 10, connected ? THEME_OK : THEME_TEXT_FAINT);
    draw_text_sz(FS_BODY, sx + 10 + SP_2, y, sv, THEME_TEXT);
    y += KV_ROW_H;

    char ip[20] = "—", gw[20] = "—";
    if (connected) {
        fmt_ip(g_st.net.ip, ip, sizeof(ip));
        fmt_ip(g_st.net.gateway, gw, sizeof(gw));
    }
    y = draw_kv_row(cx, cw, y, "IPv4", ip);
    y = draw_kv_row(cx, cw, y, "Gateway", gw);

    int by = y + SP_1;
    draw_button(cx + CARD_PAD, by, btn_w, btn_h, "Open Network Manager",
                THEME_ACCENT);
    g_st.netman_x = cx + CARD_PAD; g_st.netman_y = by;
    g_st.netman_w = btn_w;         g_st.netman_h = btn_h;

    int oy = CONTENT_TOP + cardh + SP_4;
    int oh = card_h_rows(4);
    int fy = draw_card(cx, oy, cw, oh, "Connections");
    fy = draw_future_row(cx, cw, fy, "Wi-Fi");
    fy = draw_future_row(cx, cw, fy, "VPN");
    fy = draw_future_row(cx, cw, fy, "Proxy");
    (void)draw_future_row(cx, cw, fy, "Firewall");

    g_st.controls_live = 1;
}

static void render_datetime(void)
{
    int cx = content_x(), cw = content_w();
    int cardh = card_h_rows(2);
    int y = draw_card(cx, CONTENT_TOP, cw, cardh, "Date & Time");
    y = draw_kv_row(cx, cw, y, "Time", g_st.time_line);
    (void)draw_kv_row(cx, cw, y, "Date", g_st.date_line);

    /* Format & zone — all real controls now. */
    int oy = CONTENT_TOP + cardh + SP_4;
    int oh = CARD_HEAD + 3 * TOG_ROW_H + (CARD_PAD - SP_3);
    int fy = draw_card(cx, oy, cw, oh, "Format & Zone");

    /* 24-Hour Time toggle. */
    int on = glyph_theme_clock24();
    draw_text_sz(FS_BODY, cx + CARD_PAD, fy + (TOG_ROW_H - FS_BODY) / 2 - 1,
                 "24-Hour Time", THEME_TEXT);
    int tgx = cx + cw - CARD_PAD - TOG_W;
    int tgy = fy + (TOG_ROW_H - TOG_H) / 2;
    draw_toggle(tgx, tgy, TOG_W, TOG_H, on);
    g_st.clk_x = tgx; g_st.clk_y = tgy; g_st.clk_w = TOG_W; g_st.clk_h = TOG_H;
    fy += TOG_ROW_H;

    /* Time Zone stepper:  [<]  Zone Name  [>]  */
    draw_text_sz(FS_BODY, cx + CARD_PAD, fy + (TOG_ROW_H - FS_BODY) / 2 - 1,
                 "Time Zone", THEME_TEXT);
    const char *zn = (g_st.tz_idx >= 0 && g_st.tz_idx < N_TZONES)
                   ? TZONES[g_st.tz_idx].name : "UTC";
    int sd = 24;                        /* stepper button size */
    int sy = fy + (TOG_ROW_H - sd) / 2;
    int nextx = cx + cw - CARD_PAD - sd;
    int znw = text_width(FS_BODY, zn);
    int znx = nextx - SP_2 - znw;
    int prevx = znx - SP_2 - sd;
    draw_stepper(prevx, sy, sd, "<");
    draw_text_sz(FS_BODY, znx, fy + (TOG_ROW_H - FS_BODY) / 2 - 1, zn, THEME_TEXT_DIM);
    draw_stepper(nextx, sy, sd, ">");
    g_st.tzp_x = prevx; g_st.tzp_y = sy; g_st.tzp_w = sd; g_st.tzp_h = sd;
    g_st.tzn_x = nextx; g_st.tzn_y = sy; g_st.tzn_w = sd; g_st.tzn_h = sd;
    fy += TOG_ROW_H;

    /* Set Automatically (NTP) — real toggle (admin/root only). */
    draw_toggle_row(cx, cw, fy, "Set Automatically", g_st.ntp_on,
                    &g_st.ntpt_x, &g_st.ntpt_y, &g_st.ntpt_w, &g_st.ntpt_h);

    g_st.controls_live = 1;
}

static void render_input(void)
{
    int cx = content_x(), cw = content_w();
    int kh = card_h_rows(3);
    int y = draw_card(cx, CONTENT_TOP, cw, kh, "Keyboard");
    y = draw_future_row(cx, cw, y, "Layout");
    y = draw_future_row(cx, cw, y, "Repeat Delay");
    (void)draw_future_row(cx, cw, y, "Repeat Rate");

    int my = CONTENT_TOP + kh + SP_4;
    int mh = CARD_HEAD + 2 * TOG_ROW_H + KV_ROW_H + (CARD_PAD - SP_3);
    int fy = draw_card(cx, my, cw, mh, "Mouse");

    /* Pointer Speed — real stepper (Slow / Normal / Fast). */
    draw_stepper_row(cx, cw, fy, "Pointer Speed", PSPEEDS[pspeed_idx()].name,
                     &g_st.psp_x, &g_st.psp_y, &g_st.psp_w, &g_st.psp_h,
                     &g_st.psn_x, &g_st.psn_y, &g_st.psn_w, &g_st.psn_h);
    fy += TOG_ROW_H;

    /* Natural Scrolling — real toggle. */
    draw_toggle_row(cx, cw, fy, "Natural Scrolling", glyph_theme_natural_scroll(),
                    &g_st.nat_x, &g_st.nat_y, &g_st.nat_w, &g_st.nat_h);
    fy += TOG_ROW_H;

    (void)draw_future_row(cx, cw, fy, "Tap to Click");

    g_st.controls_live = 1;
}

static void render_users(void)
{
    int cx = content_x(), cw = content_w();
    int cardh = card_h_rows(3);
    int y = draw_card(cx, CONTENT_TOP, cw, cardh, "Current User");
    y = draw_kv_row(cx, cw, y, "Username", g_st.user_line);
    y = draw_kv_row(cx, cw, y, "User ID",  g_st.uid_line);
    (void)draw_kv_row(cx, cw, y, "Hostname", g_st.current_hostname);

    int oy = CONTENT_TOP + cardh + SP_4;
    int oh = CARD_HEAD + 3 * KV_ROW_H + TOG_ROW_H + (CARD_PAD - SP_3);
    int fy = draw_card(cx, oy, cw, oh, "Management");
    fy = draw_future_row(cx, cw, fy, "Change Password");
    fy = draw_future_row(cx, cw, fy, "Add User");

    /* Automatic Login — real toggle (admin/root only). */
    draw_toggle_row(cx, cw, fy, "Automatic Login", g_st.autologin_on,
                    &g_st.al_x, &g_st.al_y, &g_st.al_w, &g_st.al_h);
    fy += TOG_ROW_H;

    (void)draw_future_row(cx, cw, fy, "Groups");

    g_st.controls_live = 1;
}

/* Human-readable kB → "12 KB" / "48 MB" / "1.4 GB". */
static void fmt_size(uint64_t kb, char *out, size_t n)
{
    if (kb >= 1024ULL * 1024ULL) {
        uint64_t gb = kb / (1024ULL * 1024ULL);
        uint64_t frac = ((kb % (1024ULL * 1024ULL)) * 10ULL) / (1024ULL * 1024ULL);
        snprintf(out, n, "%llu.%llu GB",
                 (unsigned long long)gb, (unsigned long long)frac);
    } else if (kb >= 1024ULL) {
        snprintf(out, n, "%llu MB", (unsigned long long)(kb / 1024ULL));
    } else {
        snprintf(out, n, "%llu KB", (unsigned long long)kb);
    }
}

/* One mounted-filesystem row: "<mountpoint> <fs>" + "used / total" + a
 * usage bar tinted by fill level. Returns the next row's y. */
#define MNT_BAR_H  8
#define MNT_ROW_H  (FS_BODY + SP_1 + MNT_BAR_H + SP_3)

static int draw_mount_row(int cx, int cw, int rowy, int i)
{
    surface_t *s = &g_st.surf;
    int bar_x = cx + CARD_PAD;
    int bar_w = cw - 2 * CARD_PAD;
    int bar_y = rowy + FS_BODY + SP_1;

    /* Mountpoint + dim fstype caption. */
    draw_text_sz(FS_BODY, cx + CARD_PAD, rowy, g_st.mount_mp[i], THEME_TEXT);
    int mpw = text_width(FS_BODY, g_st.mount_mp[i]);
    draw_text_sz(TYPE_CAPTION, cx + CARD_PAD + mpw + SP_2, rowy + 2,
                 g_st.mount_fs[i], THEME_TEXT_FAINT);

    uint64_t tot = g_st.mount_total[i];
    if (tot == 0) {
        /* Size not reported (ramfs): faint flat track, "—" value. */
        const char *na = "size not reported";
        int rw = text_width(FS_BODY, na);
        draw_text_sz(FS_BODY, cx + cw - CARD_PAD - rw, rowy, na, THEME_TEXT_FAINT);
        draw_rounded_rect(s, bar_x, bar_y, bar_w, MNT_BAR_H, MNT_BAR_H / 2,
                          THEME_INPUT_BG);
        return rowy + MNT_ROW_H;
    }

    uint64_t used = (g_st.mount_free[i] <= tot) ? tot - g_st.mount_free[i] : 0;
    int pct = (int)((used * 100ULL) / tot);

    char us[24], ts[24], rs[56];
    fmt_size(used, us, sizeof(us));
    fmt_size(tot, ts, sizeof(ts));
    snprintf(rs, sizeof(rs), "%s / %s  (%d%%)", us, ts, pct);
    int rw = text_width(FS_BODY, rs);
    draw_text_sz(FS_BODY, cx + cw - CARD_PAD - rw, rowy, rs, THEME_TEXT_DIM);

    /* Track + fill (green < 70%, amber < 90%, red otherwise). */
    draw_rounded_rect(s, bar_x, bar_y, bar_w, MNT_BAR_H, MNT_BAR_H / 2,
                      THEME_INPUT_BG);
    int fillw = (int)(((uint64_t)bar_w * used) / tot);
    if (fillw < MNT_BAR_H) fillw = MNT_BAR_H;     /* keep a sliver visible */
    if (fillw > bar_w)     fillw = bar_w;
    uint32_t col = (pct >= 90) ? THEME_ERROR : (pct >= 70) ? THEME_WARN : THEME_OK;
    draw_rounded_rect(s, bar_x, bar_y, fillw, MNT_BAR_H, MNT_BAR_H / 2, col);

    return rowy + MNT_ROW_H;
}

static void render_storage(void)
{
    int cx = content_x(), cw = content_w();
    int n = g_st.n_mounts > 0 ? g_st.n_mounts : 1;
    int cardh = CARD_HEAD + n * MNT_ROW_H + (CARD_PAD - SP_3);
    int y = draw_card(cx, CONTENT_TOP, cw, cardh, "Mounted Filesystems");

    if (g_st.n_mounts == 0) {
        (void)draw_kv_row(cx, cw, y, "Mounts", "none");
    } else {
        for (int i = 0; i < g_st.n_mounts; i++)
            y = draw_mount_row(cx, cw, y, i);
    }

    int oy = CONTENT_TOP + cardh + SP_4;
    if (g_st.smart_present) {
        /* Real SMART/health from /proc/smart (NVMe Get Log Page). */
        int oh = card_h_rows(7);
        int fy = draw_card(cx, oy, cw, oh, "Drive Health (SMART)");

        /* Health row with a status-colored value. */
        draw_text_sz(FS_BODY, cx + CARD_PAD, fy, "Health", THEME_TEXT_DIM);
        uint32_t hc = (strcmp(g_st.smart_health, "OK") == 0) ? THEME_OK : THEME_WARN;
        int hw = text_width(FS_BODY, g_st.smart_health);
        draw_text_sz(FS_BODY, cx + cw - CARD_PAD - hw, fy, g_st.smart_health, hc);
        fy += KV_ROW_H;

        char tmp[24];
        snprintf(tmp, sizeof(tmp), "%s \xc2\xb0" "C", g_st.smart_temp);
        fy = draw_kv_row(cx, cw, fy, "Temperature", tmp);
        snprintf(tmp, sizeof(tmp), "%s%%", g_st.smart_used);
        fy = draw_kv_row(cx, cw, fy, "Endurance Used", tmp);
        snprintf(tmp, sizeof(tmp), "%s%%", g_st.smart_spare);
        fy = draw_kv_row(cx, cw, fy, "Available Spare", tmp);
        fy = draw_kv_row(cx, cw, fy, "Power-On Hours", g_st.smart_poh);
        fy = draw_kv_row(cx, cw, fy, "Power Cycles", g_st.smart_cycles);
        (void)draw_future_row(cx, cw, fy, "Filesystem Check");
    } else {
        int oh = card_h_rows(2);
        int fy = draw_card(cx, oy, cw, oh, "Disks");
        (void)draw_kv_row(cx, cw, fy, "SMART", "No NVMe device");
        fy += KV_ROW_H;
        (void)draw_future_row(cx, cw, fy, "Filesystem Check");
    }
}

static void render_power(void)
{
    int cx = content_x(), cw = content_w();

    int pwr_card_h = CARD_PAD + FS_TITLE + SP_3 + 40 + SP_3 + FS_BODY + CARD_PAD;
    int cy = draw_card(cx, CONTENT_TOP, cw, pwr_card_h, "Power");

    int pbtn_w = 140, pbtn_h = 40;
    int restart_x  = cx + CARD_PAD;
    int poweroff_x = restart_x + pbtn_w + SP_4;

    draw_button(restart_x, cy, pbtn_w, pbtn_h, "Restart", THEME_ACCENT);
    draw_button(poweroff_x, cy, pbtn_w, pbtn_h, "Power Off", THEME_ERROR);

    g_st.restart_x = restart_x; g_st.restart_y = cy;
    g_st.restart_w = pbtn_w;    g_st.restart_h = pbtn_h;
    g_st.poweroff_x = poweroff_x; g_st.poweroff_y = cy;
    g_st.poweroff_w = pbtn_w;     g_st.poweroff_h = pbtn_h;

    draw_text_sz(FS_BODY, cx + CARD_PAD, cy + pbtn_h + SP_3,
                 "Restart or power off the system.", THEME_TEXT_DIM);

    int oy = CONTENT_TOP + pwr_card_h + SP_4;
    int oh = card_h_rows(4);
    int fy = draw_card(cx, oy, cw, oh, "Energy");
    fy = draw_future_row(cx, cw, fy, "Sleep");
    fy = draw_future_row(cx, cw, fy, "Battery");
    fy = draw_future_row(cx, cw, fy, "Automatic Suspend");
    (void)draw_future_row(cx, cw, fy, "Power Button Behavior");

    g_st.controls_live = 1;
}

/* A capability chip. Admin-tier caps (sensitive: POWER/SETUID/DISK_ADMIN/...)
 * render as a filled amber pill; service-tier caps as a subtle outline. */
#define CHIP_H   (TYPE_CAPTION + SP_2)   /* 20 */

static int chip_width(const char *cap)
{
    return text_width(TYPE_CAPTION, cap) + 2 * SP_2;
}

static void draw_chip(int x, int y, const char *cap, int admin)
{
    surface_t *s = &g_st.surf;
    int w = chip_width(cap);
    int ty = y + (CHIP_H - TYPE_CAPTION) / 2 - 1;
    if (admin) {
        draw_rounded_rect(s, x, y, w, CHIP_H, R_SM, THEME_WARN);
        draw_text_sz(TYPE_CAPTION, x + SP_2, ty, cap, THEME_BG);
    } else {
        draw_rounded_outline(s, x, y, w, CHIP_H, R_SM, 1, THEME_BORDER_STRONG);
        draw_text_sz(TYPE_CAPTION, x + SP_2, ty, cap, THEME_TEXT_DIM);
    }
}

#define APP_ROW_H  28

static void render_privacy(void)
{
    int cx = content_x(), cw = content_w();

    /* Security model — the always-on facts. */
    int smh = card_h_rows(2);
    int y = draw_card(cx, CONTENT_TOP, cw, smh, "Security Model");
    y = draw_kv_row(cx, cw, y, "Authority",    "Capability-based");
    (void)draw_kv_row(cx, cw, y, "Ambient Root", "None");

    /* App Permissions — live capability inspector. */
    int ay = CONTENT_TOP + smh + SP_4;
    int n = g_n_perms;
    int caph = CARD_HEAD + (FS_BODY + SP_2) + n * APP_ROW_H + (CARD_PAD - SP_3);
    int ry = draw_card(cx, ay, cw, caph, "App Permissions");

    draw_text_sz(TYPE_CAPTION, cx + CARD_PAD, ry,
                 "Capabilities each app is granted at launch", THEME_TEXT_DIM);
    ry += FS_BODY + SP_2;

    for (int i = 0; i < n; i++) {
        app_perm_t *p = &g_perms[i];
        int rowy = ry + i * APP_ROW_H;

        draw_text_sz(FS_BODY, cx + CARD_PAD,
                     rowy + (APP_ROW_H - FS_BODY) / 2 - 1, p->name, THEME_TEXT);

        if (p->n_caps == 0) {
            const char *bl = "baseline";
            int blw = text_width(TYPE_CAPTION, bl);
            draw_text_sz(TYPE_CAPTION, cx + cw - CARD_PAD - blw,
                         rowy + (APP_ROW_H - TYPE_CAPTION) / 2 - 1, bl,
                         THEME_TEXT_FAINT);
        } else {
            int chy = rowy + (APP_ROW_H - CHIP_H) / 2;
            int chx = cx + cw - CARD_PAD;
            for (int c = p->n_caps - 1; c >= 0; c--) {
                chx -= chip_width(p->caps[c]);
                draw_chip(chx, chy, p->caps[c], p->admin);
                chx -= SP_2;
            }
        }
    }

    /* Screen lock (real action) + one future row. */
    int oy = ay + caph + SP_4;
    int btn_h = 30, btn_w = 120;
    int oh = CARD_HEAD + (btn_h + SP_3) + KV_ROW_H + (CARD_PAD - SP_3);
    int fy = draw_card(cx, oy, cw, oh, "More");

    draw_text_sz(FS_BODY, cx + CARD_PAD, fy + (btn_h - FS_BODY) / 2 - 1,
                 "Screen Lock", THEME_TEXT);
    int bx = cx + cw - CARD_PAD - btn_w;
    draw_button(bx, fy, btn_w, btn_h, "Lock Screen", THEME_ACCENT);
    g_st.lock_x = bx; g_st.lock_y = fy; g_st.lock_w = btn_w; g_st.lock_h = btn_h;
    fy += btn_h + SP_3;

    (void)draw_future_row(cx, cw, fy, "Camera & Microphone");

    g_st.controls_live = 1;
}

static void render_about(void)
{
    surface_t *s = &g_st.surf;
    int cx = content_x(), cw = content_w();

    int cardh = CARD_PAD + FS_DISPLAY + SP_2 + FS_BODY + SP_4
              + 4 * KV_ROW_H + (CARD_PAD - SP_3);
    draw_rounded_rect(s, cx, CONTENT_TOP, cw, cardh, CARD_R, THEME_SURFACE_2);

    int y = CONTENT_TOP + CARD_PAD;
    draw_text_sz(FS_DISPLAY, cx + CARD_PAD, y, "Aegis", THEME_TEXT);
    y += FS_DISPLAY + SP_2;
    draw_text_sz(FS_BODY, cx + CARD_PAD, y,
                 "A capability-based POSIX kernel.", THEME_TEXT_DIM);
    y += FS_BODY + SP_4;

    y = draw_kv_row(cx, cw, y, "Version",   g_st.kernel_version);
    y = draw_kv_row(cx, cw, y, "Processor", g_st.processor);
    y = draw_kv_row(cx, cw, y, "Memory",    g_st.mem_line);
    (void)draw_kv_row(cx, cw, y, "Uptime",  g_st.uptime_line);

    int ty = CONTENT_TOP + cardh + SP_4;
    draw_text_sz(FS_BODY, cx + CARD_PAD, ty,
                 "No ambient authority. Forever vigilant.", THEME_TEXT_FAINT);
}

/* ── Render ─────────────────────────────────────────────────────────────── */

static void render(void)
{
    if (!g_st.dirty) return;
    g_st.dirty = 0;

    surface_t *s = &g_st.surf;
    draw_fill_rect(s, 0, 0, g_st.fb_w, g_st.fb_h, THEME_SURFACE);

    g_st.controls_live = 0;
    g_st.field_w = g_st.apply_w = g_st.restart_w = g_st.poweroff_w =
        g_st.netman_w = g_st.anim_w = g_st.clk_w = g_st.tzp_w = g_st.tzn_w =
        g_st.nat_w = g_st.lock_w = g_st.lm_w = g_st.wpp_w = g_st.wpn_w =
        g_st.nl_w = g_st.psp_w = g_st.psn_w = g_st.ntpt_w = g_st.al_w = 0;

    switch (g_st.selected) {
    case CAT_SYSTEM:     render_system();     break;
    case CAT_DISPLAY:    render_display();    break;
    case CAT_APPEARANCE: render_appearance(); break;
    case CAT_SOUND:      render_sound();      break;
    case CAT_NETWORK:    render_network();    break;
    case CAT_DATETIME:   render_datetime();   break;
    case CAT_INPUT:      render_input();      break;
    case CAT_USERS:      render_users();      break;
    case CAT_STORAGE:    render_storage();    break;
    case CAT_POWER:      render_power();      break;
    case CAT_PRIVACY:    render_privacy();    break;
    case CAT_ABOUT:      render_about();      break;
    }

    draw_sidebar();
    lumen_window_present(g_st.lwin);
}

/* ── Actions ────────────────────────────────────────────────────────────── */

static void select_category(int cat)
{
    if (cat < 0) cat = CAT_COUNT - 1;
    if (cat >= CAT_COUNT) cat = 0;
    if (cat == g_st.selected) return;
    g_st.selected = cat;
    g_st.field_focused = 0;
    g_st.status[0] = '\0';

    /* Refresh the live readers a pane depends on when entering it. */
    if (cat == CAT_DATETIME) read_datetime();
    if (cat == CAT_NETWORK)  read_net();
    if (cat == CAT_STORAGE)  { read_mounts(); read_smart(); }
    if (cat == CAT_DATETIME || cat == CAT_USERS) read_admin_state();
    if (cat == CAT_SYSTEM || cat == CAT_ABOUT) { read_uptime(); read_meminfo(); }

    g_st.dirty = 1;
}

static void apply_hostname(void)
{
    size_t len = strlen(g_st.edit);
    if (len == 0) {
        snprintf(g_st.status, sizeof(g_st.status), "hostname cannot be empty");
        g_st.status_color = THEME_ERROR;
        g_st.dirty = 1;
        return;
    }

    long rc = syscall(SYS_SETHOSTNAME, g_st.edit, (long)len);
    if (rc != 0) {
        if (errno == EPERM)
            snprintf(g_st.status, sizeof(g_st.status), "permission denied");
        else
            snprintf(g_st.status, sizeof(g_st.status), "set failed (%d)", errno);
        g_st.status_color = THEME_ERROR;
        dprintf(2, "[SETTINGS] sethostname failed errno=%d\n", errno);
    } else {
        snprintf(g_st.status, sizeof(g_st.status), "applied");
        g_st.status_color = THEME_OK;
        dprintf(2, "[SETTINGS] hostname applied\n");
    }

    read_hostname();
    if (rc == 0)
        snprintf(g_st.edit, sizeof(g_st.edit), "%s", g_st.current_hostname);
    g_st.dirty = 1;
}

static void do_restart(void)
{
    dprintf(2, "[SETTINGS] restart requested\n");
    if (kill(1, SIGINT) == 0) return;
    long rc = syscall(SYS_REBOOT, 1L);
    snprintf(g_st.status, sizeof(g_st.status),
             rc < 0 && errno == EPERM ? "restart: permission denied"
                                      : "restart failed");
    g_st.status_color = THEME_ERROR;
    g_st.dirty = 1;
}

static void do_poweroff(void)
{
    dprintf(2, "[SETTINGS] power off requested\n");
    if (kill(1, SIGTERM) != 0) {
        snprintf(g_st.status, sizeof(g_st.status), "power off failed");
        g_st.status_color = THEME_ERROR;
        g_st.dirty = 1;
    }
}

/* Step the selected timezone, persist the offset, refresh the clock. */
static void tz_step(int dir)
{
    g_st.tz_idx = (g_st.tz_idx + dir + N_TZONES) % N_TZONES;
    glyph_theme_set_tz_offset(TZONES[g_st.tz_idx].off);
    glyph_theme_save();
    read_datetime();
    g_st.dirty = 1;
}

/* ── Input handling ─────────────────────────────────────────────────────── */

static void handle_key(char c)
{
    if (c == '\x1b') { g_st.done = 1; return; }

    if (g_st.selected == CAT_SYSTEM && g_st.field_focused) {
        if (c == '\n' || c == '\r') { apply_hostname(); return; }
        int len = (int)strlen(g_st.edit);
        if (c == '\b' || c == 127) {
            if (len > 0) { g_st.edit[len - 1] = '\0'; g_st.dirty = 1; }
            return;
        }
        if (c == '\t') { g_st.field_focused = 0; g_st.dirty = 1; return; }
        if (c >= ' ' && c < 127 && len < HOSTNAME_MAX) {
            g_st.edit[len] = c; g_st.edit[len + 1] = '\0'; g_st.dirty = 1;
        }
        return;
    }

    if (c == '\t' || c == KEY_ARROW_DOWN || c == KEY_ARROW_RIGHT) {
        select_category(g_st.selected + 1);
        return;
    }
    if (c == KEY_ARROW_UP || c == KEY_ARROW_LEFT) {
        select_category(g_st.selected - 1);
        return;
    }
    if ((c == '\n' || c == '\r') && g_st.selected == CAT_SYSTEM) {
        g_st.field_focused = 1;
        g_st.dirty = 1;
    }
}

static int hit(int x, int y, int rx, int ry, int rw, int rh)
{
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static void handle_mouse_click(int x, int y)
{
    if (x < SIDEBAR_W) {
        for (int i = 0; i < CAT_COUNT; i++) {
            int rx, ry, rw, rh;
            sidebar_row_rect(i, &rx, &ry, &rw, &rh);
            if (hit(x, y, rx, ry, rw, rh)) { select_category(i); return; }
        }
        return;
    }

    if (!g_st.controls_live) return;

    if (g_st.selected == CAT_APPEARANCE) {
        int n = glyph_theme_accent_count();
        for (int i = 0; i < n; i++) {
            int sx, sy, sw2, sh2;
            swatch_rect(i, &sx, &sy, &sw2, &sh2);
            if (hit(x, y, sx, sy, sw2, sh2)) {
                glyph_theme_set_accent(i);
                glyph_theme_save();
                g_st.dirty = 1;
                return;
            }
        }
        if (hit(x, y, g_st.lm_x, g_st.lm_y, g_st.lm_w, g_st.lm_h)) {
            glyph_theme_set_light(!glyph_theme_light());
            glyph_theme_save();
            g_st.dirty = 1;
        } else if (hit(x, y, g_st.wpp_x, g_st.wpp_y, g_st.wpp_w, g_st.wpp_h)) {
            int n = glyph_theme_wallpaper_count();
            glyph_theme_set_wallpaper((glyph_theme_wallpaper() - 1 + n) % n);
            glyph_theme_save();
            g_st.dirty = 1;
        } else if (hit(x, y, g_st.wpn_x, g_st.wpn_y, g_st.wpn_w, g_st.wpn_h)) {
            int n = glyph_theme_wallpaper_count();
            glyph_theme_set_wallpaper((glyph_theme_wallpaper() + 1) % n);
            glyph_theme_save();
            g_st.dirty = 1;
        } else if (hit(x, y, g_st.anim_x, g_st.anim_y, g_st.anim_w, g_st.anim_h)) {
            glyph_theme_set_animations(!glyph_theme_animations());
            glyph_theme_save();
            g_st.dirty = 1;
        }
        return;
    }

    if (g_st.selected == CAT_DISPLAY &&
        hit(x, y, g_st.nl_x, g_st.nl_y, g_st.nl_w, g_st.nl_h)) {
        glyph_theme_set_night_light(!glyph_theme_night_light());
        glyph_theme_save();
        g_st.dirty = 1;
        return;
    }

    if (g_st.selected == CAT_NETWORK &&
        hit(x, y, g_st.netman_x, g_st.netman_y, g_st.netman_w, g_st.netman_h)) {
        dprintf(2, "[SETTINGS] launching netman\n");
        lumen_invoke(g_st.lfd, "netman");
        return;
    }

    if (g_st.selected == CAT_DATETIME) {
        if (hit(x, y, g_st.clk_x, g_st.clk_y, g_st.clk_w, g_st.clk_h)) {
            glyph_theme_set_clock24(!glyph_theme_clock24());
            glyph_theme_save();
            read_datetime();
            g_st.dirty = 1;
        } else if (hit(x, y, g_st.tzp_x, g_st.tzp_y, g_st.tzp_w, g_st.tzp_h)) {
            tz_step(-1);
        } else if (hit(x, y, g_st.tzn_x, g_st.tzn_y, g_st.tzn_w, g_st.tzn_h)) {
            tz_step(+1);
        } else if (hit(x, y, g_st.ntpt_x, g_st.ntpt_y, g_st.ntpt_w, g_st.ntpt_h)) {
            /* Admin-gated: kernel rejects with EPERM unless root. Re-read the
             * file state so the toggle reflects what actually took effect. */
            syscall(SYS_SET_NTP, g_st.ntp_on ? 0 : 1);
            read_admin_state();
            g_st.dirty = 1;
        }
        return;
    }

    if (g_st.selected == CAT_USERS &&
        hit(x, y, g_st.al_x, g_st.al_y, g_st.al_w, g_st.al_h)) {
        syscall(SYS_SET_AUTOLOGIN, g_st.autologin_on ? 0 : 1);
        read_admin_state();
        g_st.dirty = 1;
        return;
    }

    if (g_st.selected == CAT_INPUT) {
        if (hit(x, y, g_st.nat_x, g_st.nat_y, g_st.nat_w, g_st.nat_h)) {
            glyph_theme_set_natural_scroll(!glyph_theme_natural_scroll());
            glyph_theme_save();
            g_st.dirty = 1;
        } else if (hit(x, y, g_st.psp_x, g_st.psp_y, g_st.psp_w, g_st.psp_h)) {
            int i = (pspeed_idx() - 1 + N_PSPEEDS) % N_PSPEEDS;
            glyph_theme_set_pointer_speed(PSPEEDS[i].pct);
            glyph_theme_save();
            g_st.dirty = 1;
        } else if (hit(x, y, g_st.psn_x, g_st.psn_y, g_st.psn_w, g_st.psn_h)) {
            int i = (pspeed_idx() + 1) % N_PSPEEDS;
            glyph_theme_set_pointer_speed(PSPEEDS[i].pct);
            glyph_theme_save();
            g_st.dirty = 1;
        }
        return;
    }

    if (g_st.selected == CAT_PRIVACY &&
        hit(x, y, g_st.lock_x, g_st.lock_y, g_st.lock_w, g_st.lock_h)) {
        dprintf(2, "[SETTINGS] lock screen requested\n");
        lumen_invoke(g_st.lfd, "lock");
        return;
    }

    if (hit(x, y, g_st.field_x, g_st.field_y, g_st.field_w, g_st.field_h)) {
        g_st.field_focused = 1;
        g_st.dirty = 1;
    } else if (hit(x, y, g_st.apply_x, g_st.apply_y, g_st.apply_w, g_st.apply_h)) {
        apply_hostname();
    } else if (hit(x, y, g_st.restart_x, g_st.restart_y,
                   g_st.restart_w, g_st.restart_h)) {
        do_restart();
    } else if (hit(x, y, g_st.poweroff_x, g_st.poweroff_y,
                   g_st.poweroff_w, g_st.poweroff_h)) {
        do_poweroff();
    } else if (g_st.field_focused) {
        g_st.field_focused = 0;
        g_st.dirty = 1;
    }
}

/* ── Main ───────────────────────────────────────────────────────────────── */

int
main(int argc, char **argv)
{
    (void)argc; (void)argv;

    g_st.lfd = lumen_connect_retry();
    if (g_st.lfd < 0) {
        dprintf(2, "[SETTINGS] lumen_connect failed (%d)\n", g_st.lfd);
        return 1;
    }

    g_st.lwin = lumen_window_create(g_st.lfd, "Settings", WIN_W, WIN_H);
    if (!g_st.lwin) {
        dprintf(2, "[SETTINGS] lumen_window_create failed\n");
        close(g_st.lfd);
        return 1;
    }
    g_st.fb_w = g_st.lwin->w;
    g_st.fb_h = g_st.lwin->h;
    g_st.surf = (surface_t){
        .buf   = (uint32_t *)g_st.lwin->backbuf,
        .w     = g_st.fb_w,
        .h     = g_st.fb_h,
        .pitch = g_st.lwin->stride,
    };

    font_init();

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigterm_handler;
    sigaction(SIGTERM, &sa, NULL);

    read_proc_version();
    read_meminfo();
    read_cpuinfo();
    read_uptime();
    read_display();
    read_hostname();
    read_user();
    /* Resolve the persisted tz offset to a zone index (default UTC+00). */
    g_st.tz_idx = 6;
    for (int i = 0; i < N_TZONES; i++)
        if (TZONES[i].off == glyph_theme_tz_offset()) { g_st.tz_idx = i; break; }

    read_datetime();
    read_mounts();
    read_smart();
    read_net();
    read_admin_state();
    load_app_perms();
    snprintf(g_st.edit, sizeof(g_st.edit), "%s", g_st.current_hostname);

    g_st.selected = CAT_SYSTEM;
    g_st.field_focused = 0;
    g_st.dirty = 1;
    render();

    dprintf(2, "[SETTINGS] connected %dx%d\n", g_st.lwin->w, g_st.lwin->h);

    unsigned tick = 0;
    while (!s_term_requested && !g_st.done) {
        lumen_event_t ev;
        int r = lumen_wait_event(g_st.lfd, &ev, 16);
        if (r < 0) break;

        if (r == 1) {
            if (ev.type == LUMEN_EV_CLOSE_REQUEST) break;
            if (ev.type == LUMEN_EV_KEY && ev.key.pressed)
                handle_key((char)ev.key.keycode);
            if (ev.type == LUMEN_EV_MOUSE &&
                ev.mouse.evtype == LUMEN_MOUSE_DOWN &&
                (ev.mouse.buttons & 1))
                handle_mouse_click(ev.mouse.x, ev.mouse.y);
        } else {
            /* Idle tick (~16ms): refresh the live clock once per second. */
            tick++;
            if (g_st.selected == CAT_DATETIME && (tick % 62) == 0) {
                read_datetime();
                g_st.dirty = 1;
            }
        }

        render();
    }

    lumen_window_destroy(g_st.lwin);
    close(g_st.lfd);
    dprintf(2, "[SETTINGS] exit\n");
    return 0;
}
