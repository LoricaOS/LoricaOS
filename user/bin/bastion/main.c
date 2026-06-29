/* bastion — graphical display manager for Aegis.
 *
 * Presents a login form, authenticates via libauth, then spawns Lumen
 * as the authenticated user's session. Handles lock/unlock via SIGUSR1/2.
 *
 * Vigil service: graphical mode, respawn policy.
 * Capabilities: AUTH, FB, SETUID from kernel policy table (service tier).
 * After successful auth, calls auth_elevate_session() so the spawned
 * Lumen/shell gets admin-tier caps from the policy table.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <termios.h>
#include <stdint.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <sys/mman.h>

#include <glyph.h>
#include "font.h"
#include "auth.h"
#include "audio.h"

extern char **environ;

#define SYS_FB_MAP  513
#define SYS_SPAWN   514
#define SYS_NETCFG  500

/* Mirrors kernel netcfg_info_t — for the greeter network status line. */
typedef struct {
    uint8_t  mac[6];
    uint8_t  pad[2];
    uint32_t ip;       /* network byte order */
    uint32_t mask;
    uint32_t gateway;
} bastion_netcfg_t;

/* ---- Framebuffer ----------------------------------------------------- */

typedef struct {
    uint64_t addr;
    uint32_t width, height, pitch, bpp;
} fb_info_t;

/* ---- Mouse event (matches /dev/mouse) -------------------------------- */

typedef struct __attribute__((packed)) {
    uint8_t  buttons;
    int16_t  dx;
    int16_t  dy;
    int16_t  scroll;
} mouse_event_t;

/* ---- State ----------------------------------------------------------- */

static fb_info_t  s_fb_info;
static uint32_t  *s_fb;
static uint32_t  *s_backbuf;
static int        s_fb_w, s_fb_h, s_pitch_px;

static volatile sig_atomic_t s_locked;
static pid_t      s_lumen_pid;
static char       s_username[64]; /* remembered for lock screen */
static char       s_home[256] = "/root"; /* authenticated user's $HOME */
static int        s_uid, s_gid;

/* Live input-source counters from /proc/kbdstat, rendered bottom-left of
 * the greeter (serial-less bare-metal input triage). */
static char       s_kbdstat[96];
static int        s_form_dirty = 1; /* 1 = needs redraw */
static int        s_input_seen = 0; /* 1 once any key arrives -> hide diag line */
static int        s_diag_enabled = 0; /* 1 = greeter_diag cmdline flag present */

static void
update_kbdstat(void)
{
    char raw[64];
    int fd = open("/proc/kbdstat", O_RDONLY);
    if (fd < 0) return;
    int n = (int)read(fd, raw, sizeof(raw) - 1);
    close(fd);
    if (n <= 0) return;
    while (n > 0 && (raw[n-1] == '\n' || raw[n-1] == '\r')) n--;
    raw[n] = '\0';

    char next[sizeof(s_kbdstat)];
    snprintf(next, sizeof(next), "input  %s", raw);
    if (strcmp(next, s_kbdstat) != 0) {
        memcpy(s_kbdstat, next, sizeof(s_kbdstat));
        s_form_dirty = 1;
    }
}

/* ---- Logo ------------------------------------------------------------ */

static uint32_t *s_logo_pixels;
static int       s_logo_w, s_logo_h;

static void
load_logo(void)
{
    /* Try to load raw BGRA logo: first 8 bytes are uint32_t w, h */
    int fd = open("/usr/share/logo.raw", O_RDONLY);
    if (fd < 0) return;
    uint32_t hdr[2];
    if (read(fd, hdr, 8) != 8) { close(fd); return; }
    s_logo_w = (int)hdr[0];
    s_logo_h = (int)hdr[1];
    if (s_logo_w <= 0 || s_logo_h <= 0 || s_logo_w > 1200 || s_logo_h > 600) {
        close(fd); s_logo_w = s_logo_h = 0; return;
    }
    size_t sz = (size_t)(s_logo_w * s_logo_h) * 4;
    s_logo_pixels = malloc(sz);
    if (!s_logo_pixels) { close(fd); s_logo_w = s_logo_h = 0; return; }
    size_t got = 0;
    while (got < sz) {
        ssize_t n = read(fd, (char *)s_logo_pixels + got, sz - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    close(fd);
    if (got < sz) { free(s_logo_pixels); s_logo_pixels = NULL; s_logo_w = s_logo_h = 0; return; }

    /* Pre-composite alpha onto the background color so we can blit opaquely */
    uint32_t bg = THEME_SURFACE;
    int npx = s_logo_w * s_logo_h;
    for (int i = 0; i < npx; i++) {
        uint32_t px = s_logo_pixels[i];
        uint32_t a = (px >> 24) & 0xFF;
        if (a == 0xFF) continue;  /* fully opaque — keep as-is */
        if (a == 0) { s_logo_pixels[i] = bg; continue; }
        uint32_t inv = 255 - a;
        uint32_t r = (((px >> 16) & 0xFF) * a + ((bg >> 16) & 0xFF) * inv) / 255;
        uint32_t g = (((px >> 8) & 0xFF) * a + ((bg >> 8) & 0xFF) * inv) / 255;
        uint32_t b = ((px & 0xFF) * a + (bg & 0xFF) * inv) / 255;
        s_logo_pixels[i] = (r << 16) | (g << 8) | b;
    }
}

/* ---- Drawing helpers ------------------------------------------------- */

static surface_t s_surf; /* wraps s_backbuf for Glyph drawing */

/* The 4 MB backbuffer is only needed when Bastion is actually drawing
 * (greeter + lock screen). While the Lumen session runs, Bastion just
 * waitpid()s, so we release it. mmap/munmap (not malloc/free) so munmap
 * truly returns the pages to the OS — musl would retain a freed malloc
 * region of this size. */
static int
acquire_backbuf(void)
{
    if (s_backbuf)
        return 0;
    size_t sz = (size_t)s_pitch_px * s_fb_h * 4;
    s_backbuf = mmap(NULL, sz, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (s_backbuf == MAP_FAILED) {
        s_backbuf = NULL;
        dprintf(2, "bastion: backbuffer map failed\n");
        return -1;
    }
    s_surf = (surface_t){ .buf = s_backbuf, .w = s_fb_w, .h = s_fb_h,
                          .pitch = s_pitch_px };
    return 0;
}

static void
release_backbuf(void)
{
    if (s_backbuf) {
        munmap(s_backbuf, (size_t)s_pitch_px * s_fb_h * 4);
        s_backbuf = NULL;
        s_surf.buf = NULL;
    }
}

static void
fill_bg(void)
{
    draw_fill_rect(&s_surf, 0, 0, s_fb_w, s_fb_h, THEME_SURFACE);
}

static void
blit_to_fb(void)
{
    memcpy(s_fb, s_backbuf, (size_t)s_pitch_px * s_fb_h * 4);
    /* Present: no-op on a direct framebuffer, pushes the backing to the host
     * scanout on virtio-gpu (sys_fb_flush, syscall 515). */
    syscall(515, 0L);
}

static void
draw_logo(int cx, int y)
{
    if (!s_logo_pixels) return;
    int dw = s_logo_w / 2;
    int dh = s_logo_h / 2;
    int x0 = cx - dw / 2;
    draw_blit_scaled(&s_surf, x0, y, dw, dh,
                     s_logo_pixels, s_logo_w, s_logo_h);
}

static void
draw_text_simple(int x, int y, const char *text, uint32_t color)
{
    if (g_font_ui)
        font_draw_text(&s_surf, g_font_ui, 14, x, y, text, color);
    else
        draw_text_t(&s_surf, x, y, text, color);
}

/* ---- Crossfade helper ------------------------------------------------ */

static uint32_t *s_saved_frame;  /* snapshot of FB before first draw */

static void
crossfade(int steps, int delay_ms)
{
    if (!s_saved_frame) return;
    size_t npx = (size_t)s_pitch_px * s_fb_h;
    struct timespec ts = { 0, delay_ms * 1000000L };

    for (int step = 0; step < steps; step++) {
        int alpha = 255 - (step * 255 / (steps - 1));  /* 255 → 0 */
        int inv = 255 - alpha;
        for (size_t i = 0; i < npx; i++) {
            uint32_t old = s_saved_frame[i];
            uint32_t new_px = s_backbuf[i];
            uint32_t r = (((old >> 16) & 0xFF) * alpha + ((new_px >> 16) & 0xFF) * inv) / 255;
            uint32_t g = (((old >> 8) & 0xFF) * alpha + ((new_px >> 8) & 0xFF) * inv) / 255;
            uint32_t b = ((old & 0xFF) * alpha + (new_px & 0xFF) * inv) / 255;
            s_fb[i] = (r << 16) | (g << 8) | b;
        }
        nanosleep(&ts, NULL);
    }
    free(s_saved_frame);
    s_saved_frame = NULL;
}

/* ---- Login form ------------------------------------------------------ */

#define FIELD_W     240
#define FIELD_H     32
#define FIELD_GAP   10
#define BTN_H       34

static char s_user_buf[64];
static char s_pass_buf[128];
static int  s_user_len, s_pass_len;
static int  s_focus; /* 0=username, 1=password, 2=button */
static char s_error[128];
static int  s_is_lock; /* 1 = lock screen mode */

/* s_form_dirty is declared above (next to s_kbdstat). */

static void
draw_form(void)
{
    if (!s_form_dirty) return;
    s_form_dirty = 0;

    fill_bg();

    int cx = s_fb_w / 2;
    surface_t *surf = &s_surf;

    /* Logo centered — middle of logo at middle of screen.
     * draw_logo renders at 50% scale (dh = s_logo_h/2), so use that
     * for centering, not s_logo_h/4. */
    int logo_dh = s_logo_h > 0 ? s_logo_h / 2 : 20;
    int logo_y = s_fb_h / 2 - logo_dh / 2;
    /* Debug: compare with kernel splash position (serial only) */
    dprintf(2, "bastion: logo at y=%d (fb_h=%d, logo_dh=%d, orig_h=%d)\n",
            logo_y, s_fb_h, logo_dh, s_logo_h);
    if (s_logo_pixels) {
        draw_logo(cx, logo_y);
    } else {
        draw_text_simple(cx - 5 * 8 / 2, logo_y, "AEGIS", 0x00FFFFFF);
    }

    /* v1 disclaimer below logo — matches aegissite terminology */
    {
        const char *line1 = "v1 software -- first public release, not production-hardened";
        const char *line2 = "The C kernel likely contains real, exploitable vulnerabilities";
        int disc_y = logo_y + logo_dh + 16;
        int tw1 = g_font_ui ? font_text_width(g_font_ui, 13, line1) : (int)strlen(line1) * 8;
        int tw2 = g_font_ui ? font_text_width(g_font_ui, 13, line2) : (int)strlen(line2) * 8;
        if (g_font_ui) {
            font_draw_text(surf, g_font_ui, 13, cx - tw1 / 2, disc_y, line1, 0x00FFAA55);
            font_draw_text(surf, g_font_ui, 13, cx - tw2 / 2, disc_y + 18, line2, 0x00AA9080);
        } else {
            draw_text_simple(cx - tw1 / 2, disc_y, line1, 0x00FFAA55);
            draw_text_simple(cx - tw2 / 2, disc_y + 12, line2, 0x00AA9080);
        }
    }

    /* Fields well below logo — horizontal layout: [username] [password] [button] */
    int total_w = FIELD_W + FIELD_GAP + FIELD_W + FIELD_GAP + 100;
    int fx = cx - total_w / 2;
    int fy = s_fb_h * 3 / 4;  /* 75% down the screen */

    /* Lock mode indicator */
    if (s_is_lock) {
        int lock_tw = 6 * 8;
        draw_text_simple(cx - lock_tw / 2, fy - 22, "Locked", THEME_ERROR);
    }

    /* Network status line — top of screen, useful for headless verification
     * of the network stack on bare-metal where serial isn't available. */
    {
        bastion_netcfg_t info;
        memset(&info, 0, sizeof(info));
        (void)syscall(SYS_NETCFG, 1, (long)&info, 0, 0);
        char netline[96];
        if (info.ip != 0) {
            uint8_t *b = (uint8_t *)&info.ip;
            uint8_t *m = info.mac;
            snprintf(netline, sizeof(netline),
                     "net: %u.%u.%u.%u  mac %02x:%02x:%02x:%02x:%02x:%02x",
                     b[0], b[1], b[2], b[3],
                     m[0], m[1], m[2], m[3], m[4], m[5]);
            draw_text_simple(20, 16, netline, THEME_OK);
        } else {
            uint8_t *m = info.mac;
            int has_mac = m[0] || m[1] || m[2] || m[3] || m[4] || m[5];
            if (has_mac) {
                snprintf(netline, sizeof(netline),
                         "net: NO IP (mac %02x:%02x:%02x:%02x:%02x:%02x  DHCP failing)",
                         m[0], m[1], m[2], m[3], m[4], m[5]);
            } else {
                snprintf(netline, sizeof(netline),
                         "net: NO INTERFACE (driver did not register netdev)");
            }
            draw_text_simple(20, 16, netline, THEME_ERROR);
        }
    }

    /* Username field */
    draw_fill_rect(surf, fx, fy, FIELD_W, FIELD_H, THEME_INPUT_BG);
    if (s_focus == 0)
        draw_rect(surf, fx, fy, FIELD_W, FIELD_H, THEME_ACCENT);
    if (s_user_len > 0)
        draw_text_simple(fx + 8, fy + 8, s_user_buf, 0x00FFFFFF);
    else if (s_focus != 0)
        draw_text_simple(fx + 8, fy + 8, "username", THEME_TEXT_FAINT);
    int ux = fx + FIELD_W + FIELD_GAP;

    /* Password field */
    draw_fill_rect(surf, ux, fy, FIELD_W, FIELD_H, THEME_INPUT_BG);
    if (s_focus == 1)
        draw_rect(surf, ux, fy, FIELD_W, FIELD_H, THEME_ACCENT);
    if (s_pass_len > 0) {
        char stars[128];
        int i;
        for (i = 0; i < s_pass_len && i < 126; i++) stars[i] = '*';
        stars[i] = '\0';
        draw_text_simple(ux + 8, fy + 8, stars, 0x00FFFFFF);
    } else if (s_focus != 1) {
        draw_text_simple(ux + 8, fy + 8, "password", THEME_TEXT_FAINT);
    }
    int bx = ux + FIELD_W + FIELD_GAP;

    /* Login/Unlock button */
    uint32_t btn_color = (s_focus == 2) ? THEME_ACCENT : THEME_SURFACE_2;
    draw_fill_rect(surf, bx, fy, 100, FIELD_H, btn_color);
    const char *btn_text = s_is_lock ? "Unlock" : "Login";
    int btn_tw = (int)strlen(btn_text) * 8;
    draw_text_simple(bx + 50 - btn_tw / 2, fy + 8, btn_text, 0x00FFFFFF);

    /* Error message centered below */
    if (s_error[0]) {
        int err_tw = (int)strlen(s_error) * 8;
        draw_text_simple(cx - err_tw / 2, fy + FIELD_H + 16, s_error, THEME_ERROR);
    }

    /* Input diagnostics, bottom-left: live kernel input-source counters
     * from /proc/kbdstat.  This is the only way to triage "keyboard dead
     * at greeter" on machines without a serial console — type and watch:
     * counters moving = kernel sees input (delivery bug); frozen = the
     * hardware/IRQ level never fires.  Gated off in production (greeter_diag
     * cmdline flag); even when enabled it hides after the first keypress,
     * since by then the keyboard is provably alive. */
    if (s_diag_enabled && s_kbdstat[0] && !s_input_seen)
        draw_text_simple(8, s_fb_h - 24, s_kbdstat, THEME_TEXT_FAINT);

    static int s_greeter_announced = 0;
    if (!s_greeter_announced) {
        dprintf(2, "[BASTION] greeter ready\n");
        s_greeter_announced = 1;
    }
    blit_to_fb();
}

/* ---- Input handling -------------------------------------------------- */

static void
handle_key(char c)
{
    s_form_dirty = 1;
    s_input_seen = 1; /* keyboard proven alive -> drop the triage line */

    if (c == '\t') {
        s_focus = (s_focus + 1) % 3;
        if (s_is_lock && s_focus == 0) s_focus = 1;
        return;
    }

    if (c == '\n' || c == '\r') {
        s_focus = 2;
        return;
    }

    if (s_focus == 0 && !s_is_lock) {
        if (c == '\x7f' || c == '\b') {
            if (s_user_len > 0) s_user_buf[--s_user_len] = '\0';
        } else if (c >= ' ' && s_user_len < 62) {
            s_user_buf[s_user_len++] = c;
            s_user_buf[s_user_len] = '\0';
        }
    } else if (s_focus == 1) {
        if (c == '\x7f' || c == '\b') {
            if (s_pass_len > 0) s_pass_buf[--s_pass_len] = '\0';
        } else if (c >= ' ' && s_pass_len < 126) {
            s_pass_buf[s_pass_len++] = c;
            s_pass_buf[s_pass_len] = '\0';
        }
    }
}

/* ---- Signal handler for lock (SIGUSR1) ------------------------------- */

static void
sigusr1_handler(int sig)
{
    (void)sig;
    s_locked = 1;
}

/* ---- Session spawn --------------------------------------------------- */

static pid_t
spawn_lumen(void)
{
    /* Use fork+execve instead of sys_spawn.  The original "freeze" bug
     * (pty_pool_lock recursive deadlock in try_acquire_ctty) was fixed
     * on 2026-04-09 and Lumen DOES render via sys_spawn now.  We still
     * prefer fork+execve because sys_spawn duplicates the parent's
     * stdio_fd across child fd 0/1/2: bastion's fd 0 is the kbd TTY
     * (read-only), so the child's stderr silently swallows all writes.
     * fork+execve gives Lumen a full independent fd table with working
     * stdin/stdout/stderr. A future sys_spawn API change (separate fd
     * args for stdin/stdout/stderr) would let us switch. */
    pid_t pid = fork();
    if (pid == 0) {
        /* Child: grant caps AFTER fork (exec_caps not inherited by fork),
         * then exec lumen. */
        auth_grant_shell_caps();
        setenv("PATH", "/bin", 1);
        setenv("HOME", s_home, 1);
        setenv("USER", s_username, 1);
        setenv("TERM", "dumb", 1);
        char *argv[] = { "lumen", NULL };
        execve("/bin/lumen", argv, environ);
        _exit(127);
    }
    return pid;
}

/* ---- Authentication flow --------------------------------------------- */

static int
do_auth(void)
{
    char home[256], shell[256];
    int uid = 0, gid = 0;

    if (s_is_lock) {
        /* Lock mode: verify same user */
        if (auth_check(s_username, s_pass_buf, &uid, &gid,
                       home, (int)sizeof(home), shell, (int)sizeof(shell)) != 0)
            return -1;
        return 0;
    }

    /* Greeter mode: full auth */
    if (auth_check(s_user_buf, s_pass_buf, &uid, &gid,
                   home, (int)sizeof(home), shell, (int)sizeof(shell)) != 0)
        return -1;

    /* Save for lock screen and session */
    strncpy(s_username, s_user_buf, sizeof(s_username) - 1);
    /* Remember the authenticated user's home so the session gets the right
     * $HOME (was hardcoded to /root for everyone). Fall back to /root if
     * passwd has no home field. */
    if (home[0]) {
        strncpy(s_home, home, sizeof(s_home) - 1);
        s_home[sizeof(s_home) - 1] = '\0';
    }
    s_uid = uid;
    s_gid = gid;

    /* Elevate session (binds uid/gid) so spawned Lumen/shell gets admin-tier
     * caps and the setuid below is permitted to the verified identity. */
    auth_elevate_session(uid, gid);
    auth_set_identity(uid, gid);
    auth_grant_shell_caps();

    return 0;
}

/* Passwordless autologin: establish a session for `user` without a password,
 * the way /etc/aegis/autologin requests. Bastion holds AUTH/SETUID caps, so it
 * can elevate + bind the identity directly (no shadow check). Returns 0 on
 * success, -1 if the user isn't in /etc/passwd. */
static int
do_autologin_nopass(const char *user)
{
    int uid = 0, gid = 0;
    char home[256], shell[256];
    if (auth_lookup_passwd(user, &uid, &gid,
                           home, (int)sizeof(home), shell, (int)sizeof(shell)) != 0)
        return -1;

    strncpy(s_username, user, sizeof(s_username) - 1);
    s_username[sizeof(s_username) - 1] = '\0';
    if (home[0]) {
        strncpy(s_home, home, sizeof(s_home) - 1);
        s_home[sizeof(s_home) - 1] = '\0';
    }
    s_uid = uid;
    s_gid = gid;

    auth_elevate_session(uid, gid);
    auth_set_identity(uid, gid);
    auth_grant_shell_caps();
    return 0;
}

/* ---- Startup sound ----------------------------------------------------
 * Plays /usr/share/startup.mp3 (decoded to 48 kHz/16-bit by libaudio) through
 * /dev/audio at greeter start, or a short generated chime if no clip is
 * installed. Best-effort — never blocks the boot (the DMA plays async). */
#define AUDIO_SR 48000

static void
gen_chime(int fd)
{
    int total = AUDIO_SR * 35 / 100;        /* 0.35 s */
    int attack = AUDIO_SR / 50;             /* 20 ms fade in/out */
    int16_t chunk[256 * 2];
    int i = 0;
    while (i < total) {
        int n = 0;
        for (; n < 256 && i < total; n++, i++) {
            int freq   = (i < total / 2) ? 523 : 698;   /* C5 → F5 */
            int period = AUDIO_SR / freq;
            int half   = period / 2;
            int ph     = i % period;
            int v = (ph < half) ? (-7000 + (2 * 7000 * ph) / half)
                                : ( 7000 - (2 * 7000 * (ph - half)) / half);
            int env = 256;
            if (i < attack)              env = i * 256 / attack;
            else if (i > total - attack) env = (total - i) * 256 / attack;
            int16_t s = (int16_t)(v * env / 256);
            chunk[n * 2] = s;
            chunk[n * 2 + 1] = s;
        }
        write(fd, chunk, (size_t)n * 2 * sizeof(int16_t));
    }
}

/* Play the startup sound in a detached grandchild. /dev/audio now streams with
 * backpressure (writes block at realtime), so playing inline would stall the
 * boot for the whole clip — fork it off instead. Double-fork so the player is
 * reparented to init (vigil) and never lingers as a zombie under bastion. */
static void
play_startup_sound(void)
{
    pid_t c = fork();
    if (c < 0) return;
    if (c > 0) { waitpid(c, NULL, 0); return; }   /* reap the intermediate (instant) */

    if (fork() != 0) _exit(0);                    /* intermediate exits → grandchild detaches */

    int fd = open("/dev/audio", O_WRONLY);
    if (fd >= 0) {
        if (audio_play_file(fd, "/usr/share/startup.mp3") != 0)
            gen_chime(fd);
        close(fd);                                /* close drains the buffered tail */
    }
    _exit(0);
}

/* ---- Main ------------------------------------------------------------- */

int
main(int argc, char **argv)
{
    /* Exit immediately unless booted in graphical mode or --force flag.
     * Bastion is graphical only — if /proc/cmdline doesn't contain
     * "boot=graphical", exit immediately (unless overridden for debugging).
     *
     * Test harness hook: if /proc/cmdline contains "bastion_autologin=USER",
     * skip the greeter and authenticate as USER with the hardcoded test
     * password "forevervigilant". Production ISOs never set this flag, so
     * the test password isn't exposed in real builds. */
    char autologin_user[64] = "";
    {
        int graphical = 0;
        int i;
        for (i = 1; i < argc; i++) {
            if (strcmp(argv[i], "--force") == 0)
                graphical = 1;
        }
        dprintf(2, "bastion: argc=%d force=%d\n", argc, graphical);
        /* Always read /proc/cmdline so we can also pick up
         * bastion_autologin=USER for the test harness. */
        {
            int cfd = open("/proc/cmdline", O_RDONLY);
            if (cfd >= 0) {
                char cmd[256];
                int cn = (int)read(cfd, cmd, sizeof(cmd) - 1);
                close(cfd);
                if (cn > 0) {
                    cmd[cn] = '\0';
                    if (strstr(cmd, "boot=graphical")) {
                        graphical = 1;
                    } else if (!strstr(cmd, "boot=text")) {
                        /* No explicit boot= mode on the cmdline: vigil only
                         * starts bastion (a graphical-mode service) once it has
                         * inferred graphical mode from the compositor being
                         * installed, so mirror that inference here and come up.
                         * An explicit boot=text still forces an exit. This is
                         * what lets a server that ran `herald install desktop`
                         * reach the greeter on the next boot with no bootloader
                         * edit. (See vigil's matching /bin/bastion check.) */
                        graphical = 1;
                    }
                    /* The bottom-left input-diagnostics line is a triage aid
                     * (serial-less "is the keyboard alive?" debugging). It's
                     * off in production greeters and only rendered when the
                     * kernel cmdline carries "greeter_diag" — set by the
                     * "Aegis (input diagnostics)" boot-menu entry. */
                    if (strstr(cmd, "greeter_diag"))
                        s_diag_enabled = 1;
                    const char *au = strstr(cmd, "bastion_autologin=");
                    if (au) {
                        au += strlen("bastion_autologin=");
                        size_t i;
                        for (i = 0; i < sizeof(autologin_user) - 1 &&
                             au[i] && au[i] != ' ' && au[i] != '\t' &&
                             au[i] != '\n'; i++) {
                            autologin_user[i] = au[i];
                        }
                        autologin_user[i] = '\0';
                    }
                }
            }
        }
        if (!graphical) {
            dprintf(2, "bastion: not graphical, exiting\n");
            return 0;
        }
    }

    dprintf(2, "bastion: mapping framebuffer...\n");

    /* Map framebuffer */
    memset(&s_fb_info, 0, sizeof(s_fb_info));
    long fb_rc = syscall(SYS_FB_MAP, &s_fb_info);
    dprintf(2, "bastion: sys_fb_map returned %ld\n", fb_rc);
    if (fb_rc < 0) {
        dprintf(2, "bastion: sys_fb_map FAILED (%ld)\n", fb_rc);
        sleep(30);
        return 1;
    }
    s_fb = (uint32_t *)(uintptr_t)s_fb_info.addr;
    s_fb_w = (int)s_fb_info.width;
    s_fb_h = (int)s_fb_info.height;
    s_pitch_px = (int)(s_fb_info.pitch / (s_fb_info.bpp / 8));

    if (acquire_backbuf() != 0)
        return 1;

    /* Load logo + fonts */
    load_logo();
    font_init();

    /* Paint FB charcoal immediately to hide any kernel log remnants */
    fill_bg();
    blit_to_fb();

    /* Startup chime (async — DMA plays while the greeter comes up). */
    play_startup_sound();

    /* Raw keyboard mode */
    struct termios t_orig;
    tcgetattr(0, &t_orig);
    struct termios t_raw = t_orig;
    t_raw.c_lflag &= ~(unsigned)(ECHO | ICANON | ISIG);
    t_raw.c_cc[VMIN] = 0;
    t_raw.c_cc[VTIME] = 0;
    tcsetattr(0, TCSANOW, &t_raw);

    /* Install SIGUSR1 handler for lock screen */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigusr1_handler;
    sigaction(SIGUSR1, &sa, NULL);

    /* Open mouse (non-blocking) */
    int mouse_fd = open("/dev/mouse", O_RDONLY);

    /* ---- Autologin (test-only) ----------------------------------------
     * `bastion_autologin=USER` on /proc/cmdline skips the greeter and
     * authenticates as USER with the hardcoded test password
     * "forevervigilant". This bypasses the input-timing flake in the
     * gui-installer test harness. Production ISOs never include this
     * cmdline arg. */
    if (autologin_user[0]) {
        strncpy(s_user_buf, autologin_user, sizeof(s_user_buf) - 1);
        s_user_len = (int)strlen(s_user_buf);
        strncpy(s_pass_buf, "forevervigilant", sizeof(s_pass_buf) - 1);
        s_pass_len = (int)strlen(s_pass_buf);
        s_focus = 2;
        draw_form();   /* paints + emits [BASTION] greeter ready */

        /* auth_check sometimes returns -1 on the very first try in cold
         * boots — a libauth / fs-cache race that we don't fully
         * understand. Retry a few times before giving up. */
        int auth_ok = 0;
        for (int t = 0; t < 5; t++) {
            if (do_auth() == 0) { auth_ok = 1; break; }
            struct timespec ts = { 0, 200 * 1000000L };  /* 200ms */
            nanosleep(&ts, NULL);
        }
        if (auth_ok) {
            dprintf(2, "[BASTION] autologin OK for %s\n", autologin_user);
            s_lumen_pid = spawn_lumen();
            if (s_lumen_pid > 0)
                goto session;
            dprintf(2, "[BASTION] autologin: spawn_lumen failed\n");
        } else {
            dprintf(2, "[BASTION] autologin auth FAILED\n");
        }
        /* fall through to greeter on failure */
    }

    /* ---- File-based autologin (production) ---------------------------
     * /etc/aegis/autologin names a user to log in without a password.
     * Written by Settings → Users → Automatic Login (admin/root only).
     * Only consulted when the test cmdline hook didn't already autologin. */
    if (!autologin_user[0]) {
        char fu[64] = "";
        int afd = open("/etc/aegis/autologin", O_RDONLY);
        if (afd >= 0) {
            char b[80];
            int bn = (int)read(afd, b, sizeof(b) - 1);
            close(afd);
            if (bn > 0) {
                b[bn] = '\0';
                size_t i = 0;
                while (i < sizeof(fu) - 1 && b[i] && b[i] != '\n' &&
                       b[i] != '\r' && b[i] != ' ' && b[i] != '\t') {
                    fu[i] = b[i];
                    i++;
                }
                fu[i] = '\0';
            }
        }
        if (fu[0]) {
            dprintf(2, "[BASTION] file autologin user=%s\n", fu);
            draw_form();
            if (do_autologin_nopass(fu) == 0) {
                dprintf(2, "[BASTION] autologin OK for %s\n", fu);
                s_lumen_pid = spawn_lumen();
                if (s_lumen_pid > 0)
                    goto session;
                dprintf(2, "[BASTION] file autologin: spawn_lumen failed\n");
            } else {
                dprintf(2, "[BASTION] file autologin FAILED for %s\n", fu);
            }
        }
    }

    /* ---- Greeter loop ------------------------------------------------ */
greeter:
    s_is_lock = 0;
    s_user_buf[0] = '\0'; s_user_len = 0;
    s_pass_buf[0] = '\0'; s_pass_len = 0;
    s_error[0] = '\0';
    s_focus = 0;
    s_locked = 0;

    {
    int diag_tick = 0;
    for (;;) {
        /* Refresh the input-diagnostics line ~2x/s (32 × 16ms).
         * Skip the /proc/kbdstat read entirely when diagnostics are off. */
        if (s_diag_enabled && !s_input_seen && (diag_tick++ & 31) == 0)
            update_kbdstat();

        draw_form();

        /* Poll input */
        struct timespec ts = { 0, 16000000 }; /* 16ms */
        nanosleep(&ts, NULL);

        char c;
        while (read(0, &c, 1) == 1) {
            if (c == '\n' || c == '\r') {
                /* Submit */
                if (do_auth() == 0) {
                    /* Success — spawn Lumen */
                    s_lumen_pid = spawn_lumen();
                    if (s_lumen_pid <= 0) {
                        snprintf(s_error, sizeof(s_error), "Failed to start session");
                        s_form_dirty = 1;
                        continue;
                    }
                    goto session;
                } else {
                    snprintf(s_error, sizeof(s_error), "Invalid credentials");
                    s_pass_buf[0] = '\0'; s_pass_len = 0;
                    s_form_dirty = 1;
                }
            } else {
                handle_key(c);
            }
        }

        /* Read mouse (discard for now — no click handling in v1) */
        if (mouse_fd >= 0) {
            mouse_event_t me;
            while (read(mouse_fd, &me, sizeof(me)) == sizeof(me))
                ; /* drain */
        }
    }
    }

session:
    /* Keep stdin + mouse open — needed for lock screen.
     * Lumen reads from PTY master fds, not stdin, so there's no
     * contention. Put terminal back to raw mode for lock screen input. */

    /* Lumen owns the screen now — drop our 4 MB backbuffer until we need
     * to draw again (lock screen, or the greeter after Lumen exits). */
    release_backbuf();

    /* Wait for Lumen to exit */
    {
        int status;
        while (waitpid(s_lumen_pid, &status, 0) < 0) {
            if (errno != EINTR) break;
            if (s_locked) {
                /* Lock screen requested — re-enter raw mode for input */
                tcsetattr(0, TCSANOW, &t_raw);
                if (mouse_fd < 0)
                    mouse_fd = open("/dev/mouse", O_RDONLY);

                /* Need to draw again — re-acquire the backbuffer. */
                if (acquire_backbuf() != 0)
                    break;

                s_is_lock = 1;
                s_pass_buf[0] = '\0'; s_pass_len = 0;
                s_error[0] = '\0';
                s_focus = 1; /* password field */
                strncpy(s_user_buf, s_username, sizeof(s_user_buf) - 1);
                s_user_len = (int)strlen(s_user_buf);

                /* Draw lock screen and handle unlock */
                for (;;) {
                    draw_form();
                    struct timespec ts = { 0, 16000000 };
                    nanosleep(&ts, NULL);

                    char c;
                    while (read(0, &c, 1) == 1) {
                        if (c == '\n' || c == '\r') {
                            if (do_auth() == 0) {
                                /* Unlock — resume Lumen */
                                kill(s_lumen_pid, SIGUSR2);
                                s_locked = 0;
                                goto session;
                            } else {
                                snprintf(s_error, sizeof(s_error), "Invalid credentials");
                                s_pass_buf[0] = '\0'; s_pass_len = 0;
                                s_form_dirty = 1;
                            }
                        } else {
                            handle_key(c);
                        }
                    }

                    if (mouse_fd >= 0) {
                        mouse_event_t me;
                        while (read(mouse_fd, &me, sizeof(me)) == sizeof(me))
                            ;
                    }
                }
            }
        }
    }

    /* Lumen exited — re-acquire the backbuffer, reopen input devices, and
     * show the greeter again. */
    if (acquire_backbuf() != 0)
        return 1;
    open("/dev/kbd", O_RDONLY);  /* fd 0 — stdin/keyboard */
    mouse_fd = open("/dev/mouse", O_RDONLY);
    tcgetattr(0, &t_orig);
    t_raw = t_orig;
    t_raw.c_lflag &= ~(unsigned)(ECHO | ICANON | ISIG);
    t_raw.c_cc[VMIN] = 0;
    t_raw.c_cc[VTIME] = 0;
    tcsetattr(0, TCSANOW, &t_raw);
    goto greeter;
}
