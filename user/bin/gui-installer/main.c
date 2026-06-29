/* user/bin/gui-installer/main.c — Glyph-based AspisOS installer wizard
 *
 * A graphical front-end for libinstall.a.  Maps the framebuffer
 * directly (same pattern as Bastion) and drives a five-screen
 * keyboard-navigable wizard: Welcome → Disk → User → Confirm →
 * Progress.
 *
 * This file owns only the UI. All install logic lives in
 * ../../lib/libinstall/.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <stdint.h>
#include <sys/syscall.h>

#include <glyph.h>
#include <lumen_client.h>
#include "font.h"

#include "libinstall.h"

#define SYS_REBOOT  169

/* ── Screen enumeration ─────────────────────────────────────────────── */

typedef enum {
    SCREEN_WELCOME = 1,
    SCREEN_DISK    = 2,
    SCREEN_USER    = 3,
    SCREEN_CONFIRM = 4,
    SCREEN_PROGRESS = 5,
} screen_id_t;

/* ── Wizard state ───────────────────────────────────────────────────── */

#define MAX_DISKS 8

typedef struct {
    /* Lumen window */
    int            lfd;   /* socket fd from lumen_connect() */
    lumen_window_t *lwin;
    uint32_t  *backbuf;  /* == (uint32_t *)lwin->backbuf */
    int        fb_w, fb_h, pitch_px;
    surface_t  surf;

    /* Current screen */
    screen_id_t screen;
    int         dirty;

    /* Disk selection */
    install_blkdev_t disks[MAX_DISKS];
    int              ndisks;
    int              selected_disk;

    /* Credentials */
    char root_pw[64];
    char root_pw_confirm[64];
    char username[64];
    char user_pw[64];
    char user_pw_confirm[64];
    char validation_error[128];

    /* Hashes (computed at confirm time) */
    char root_hash[256];
    char user_hash[256];

    /* Admin authorization (Confirm screen): the separate admin credential
     * verified by `login -elevate` so the kernel grants DISK_ADMIN for the
     * raw disk write. Distinct from the root/user account passwords above. */
    char admin_pw[64];

    /* Progress (Screen 5) */
    char progress_label[128];
    int  progress_value;
    int  install_done;
    int  install_failed;
    char progress_error[256];
} wizard_state_t;

static wizard_state_t g_st;

/* Set to 1 once the install has been kicked off on Screen 5,
 * so we don't re-run it on redraws. */
static int s_install_started = 0;

/* Screen 3 focus state. 0=root_pw, 1=root_confirm, 2=username,
 * 3=user_pw, 4=user_confirm, 5=next-button. */
static int s_user_focus = 0;

/* ── Signal handling ────────────────────────────────────────────────── */

static volatile sig_atomic_t s_term_requested;

static void sigterm_handler(int sig) { (void)sig; s_term_requested = 1; }

/* ── Drawing helpers ────────────────────────────────────────────────── */

static void fill_bg(void)
{
    draw_fill_rect(&g_st.surf, 0, 0, g_st.fb_w, g_st.fb_h, 0x00202030);
}

static void blit(void)
{
    lumen_window_present(g_st.lwin);
}

static void draw_text14(int x, int y, const char *s, uint32_t color)
{
    if (g_font_ui)
        font_draw_text(&g_st.surf, g_font_ui, 14, x, y, s, color);
    else
        draw_text_t(&g_st.surf, x, y, s, color);
}

static void draw_text18(int x, int y, const char *s, uint32_t color)
{
    if (g_font_ui)
        font_draw_text(&g_st.surf, g_font_ui, 18, x, y, s, color);
    else
        draw_text_t(&g_st.surf, x, y, s, color);
}

/* ── Forward declarations for each screen's draw function ───────────── */

static void draw_screen_welcome(void);
static void draw_screen_disk(void);
static void draw_screen_user(void);
static void draw_screen_confirm(void);
static void draw_screen_progress(void);

static void render_current_screen(void)
{
    if (!g_st.dirty) return;
    g_st.dirty = 0;

    switch (g_st.screen) {
    case SCREEN_WELCOME:  draw_screen_welcome();  break;
    case SCREEN_DISK:     draw_screen_disk();     break;
    case SCREEN_USER:     draw_screen_user();     break;
    case SCREEN_CONFIRM:  draw_screen_confirm();  break;
    case SCREEN_PROGRESS: draw_screen_progress(); break;
    }
    blit();
}

/* ── Shared chrome (title + status bar) ─────────────────────────────── */

static void draw_chrome(const char *title)
{
    fill_bg();

    /* Title bar */
    draw_fill_rect(&g_st.surf, 0, 0, g_st.fb_w, 48, 0x002A2A3E);
    draw_text18((g_st.fb_w - (int)strlen(title) * 10) / 2,
                14, title, 0x00FFFFFF);
    draw_fill_rect(&g_st.surf, 0, 48, g_st.fb_w, 1, 0x00404060);

    /* Status bar */
    draw_fill_rect(&g_st.surf, 0, g_st.fb_h - 40,
                   g_st.fb_w, 40, 0x00181828);
    draw_fill_rect(&g_st.surf, 0, g_st.fb_h - 41,
                   g_st.fb_w, 1, 0x00404060);
    char step[32];
    snprintf(step, sizeof(step), "Step %d of 5", (int)g_st.screen);
    draw_text14(20, g_st.fb_h - 26, step, 0x00A0A0B0);
}

static void draw_button(int x, int y, int w, int h,
                        const char *label, int highlighted)
{
    uint32_t bg = highlighted ? 0x005577DD : 0x003A4A6A;
    draw_fill_rect(&g_st.surf, x, y, w, h, bg);
    int tw = (int)strlen(label) * 10;
    draw_text14(x + (w - tw) / 2, y + (h - 18) / 2,
                label, 0x00FFFFFF);
}

/* ── Screen 1: Welcome ──────────────────────────────────────────────── */

static void draw_screen_welcome(void)
{
    draw_chrome("AspisOS Installer");

    int cx = g_st.fb_w / 2;
    draw_text18(cx - 160, 120, "Welcome to AspisOS", 0x00FFFFFF);

    draw_text14(cx - 280, 180,
                "This installer will install AspisOS to your disk.",
                0x00C0C0D0);
    draw_text14(cx - 280, 204,
                "All data on the target disk will be erased.",
                0x00C0C0D0);
    /* Admin authorization. Listing and writing disks requires raw block
     * access (CAP_KIND_DISK_ADMIN), which the kernel grants only to an
     * admin-elevated session. Collect the separate admin credential here,
     * before the disk screen — `login -elevate` verifies it on Enter and
     * elevates this process, after which disk enumeration works. */
    draw_text14(cx - 280, 250,
                "Admin password (to authorize disk access):",
                0x00C0C0D0);
    int fx = cx - 280;
    int fy = 274;
    int field_w = 440;
    int field_h = 32;
    draw_fill_rect(&g_st.surf, fx, fy, field_w, field_h, 0x002A2A3E);
    draw_rect(&g_st.surf, fx, fy, field_w, field_h, 0x004488CC);
    {
        int vlen = (int)strlen(g_st.admin_pw);
        if (vlen > 0) {
            char stars[64];
            int si;
            for (si = 0; si < vlen && si < 63; si++) stars[si] = '*';
            stars[si] = '\0';
            draw_text14(fx + 8, fy + 8, stars, 0x00FFFFFF);
        }
    }
    draw_text14(cx - 280, fy + 50,
                "Type the admin password, then press Enter.",
                0x00808090);

    if (g_st.validation_error[0])
        draw_text14(cx - 280, fy + 78, g_st.validation_error, 0x00FF4444);

    /* Next button (always highlighted — only button on this screen) */
    draw_button(cx - 50, g_st.fb_h - 120, 100, 40, "Next", 1);
}

/* ── Screen 2: Disk selection ───────────────────────────────────────── */

static void screen_disk_populate(void)
{
    /* Populate disk list from libinstall, skipping ramdisks and
     * partitions (anything containing 'p' after the base name). */
    install_blkdev_t raw[MAX_DISKS];
    int n = install_list_blkdevs(raw, MAX_DISKS);
    g_st.ndisks = 0;
    for (int i = 0; i < n && g_st.ndisks < MAX_DISKS; i++) {
        if (strncmp(raw[i].name, "ramdisk", 7) == 0) continue;
        if (strchr(raw[i].name, 'p') != NULL) continue;
        g_st.disks[g_st.ndisks++] = raw[i];
    }
    if (g_st.selected_disk < 0 && g_st.ndisks > 0)
        g_st.selected_disk = 0;
}

static void draw_screen_disk(void)
{
    /* Refresh the disk list each time the screen is entered —
     * cheap enough and handles live-inserted USB etc. */
    if (g_st.ndisks == 0)
        screen_disk_populate();

    draw_chrome("Select target disk");

    int cx = g_st.fb_w / 2;
    draw_text18(cx - 200, 96, "Select target disk", 0x00FFFFFF);
    draw_text14(cx - 280, 130,
                "Use Up/Down to change selection, Enter to confirm.",
                0x00A0A0B0);

    /* List of disks */
    int lx = cx - 220;
    int ly = 170;
    int lw = 440;
    int row_h = 32;

    draw_fill_rect(&g_st.surf, lx, ly, lw,
                   row_h * (g_st.ndisks > 0 ? g_st.ndisks : 1) + 8,
                   0x00181828);

    if (g_st.ndisks == 0) {
        draw_text14(lx + 12, ly + 10,
                    "No suitable disk found.", 0x00FF4444);
    } else {
        for (int i = 0; i < g_st.ndisks; i++) {
            int ry = ly + 4 + i * row_h;
            if (i == g_st.selected_disk)
                draw_fill_rect(&g_st.surf, lx + 4, ry - 2,
                               lw - 8, row_h, 0x004488CC);
            char line[80];
            int has_install = install_disk_has_aegis(g_st.disks[i].name);
            snprintf(line, sizeof(line), "%s  —  %llu MB%s",
                     g_st.disks[i].name,
                     (unsigned long long)g_st.disks[i].block_count *
                         g_st.disks[i].block_size / (1024 * 1024),
                     has_install ? "  [existing AspisOS install]" : "");
            uint32_t color = has_install ? 0x00FFAA40 : 0x00FFFFFF;
            draw_text14(lx + 12, ry + 8, line, color);
        }
    }

    /* Back + Next buttons */
    draw_button(cx - 160, g_st.fb_h - 120, 100, 40, "Back", 0);
    int next_hl = (g_st.ndisks > 0);
    draw_button(cx + 60, g_st.fb_h - 120, 100, 40, "Next", next_hl);
}

/* ── Screen 3: User account ─────────────────────────────────────────── */

typedef struct {
    const char *label;
    char       *buf;
    int         buf_sz;
    int         masked;
} user_field_t;

static user_field_t user_fields[5];
static int user_fields_inited = 0;

static void screen_user_init_fields(void)
{
    if (user_fields_inited) return;
    user_fields[0] = (user_field_t){ "Root password",         g_st.root_pw,         sizeof(g_st.root_pw),         1 };
    user_fields[1] = (user_field_t){ "Confirm root password", g_st.root_pw_confirm, sizeof(g_st.root_pw_confirm), 1 };
    user_fields[2] = (user_field_t){ "Username (optional)",   g_st.username,        sizeof(g_st.username),        0 };
    user_fields[3] = (user_field_t){ "User password",         g_st.user_pw,         sizeof(g_st.user_pw),         1 };
    user_fields[4] = (user_field_t){ "Confirm user password", g_st.user_pw_confirm, sizeof(g_st.user_pw_confirm), 1 };
    user_fields_inited = 1;
}

static int screen_user_validate(void)
{
    g_st.validation_error[0] = '\0';
    if (g_st.root_pw[0] == '\0') {
        snprintf(g_st.validation_error, sizeof(g_st.validation_error),
                 "Root password cannot be empty.");
        return -1;
    }
    if (strcmp(g_st.root_pw, g_st.root_pw_confirm) != 0) {
        snprintf(g_st.validation_error, sizeof(g_st.validation_error),
                 "Root passwords do not match.");
        return -1;
    }
    if (g_st.username[0] != '\0') {
        if (!install_username_valid(g_st.username)) {
            snprintf(g_st.validation_error, sizeof(g_st.validation_error),
                     "Username: a-z, 0-9, _ or - (max 31, start a-z/_).");
            return -1;
        }
        if (g_st.user_pw[0] == '\0') {
            snprintf(g_st.validation_error, sizeof(g_st.validation_error),
                     "User password cannot be empty.");
            return -1;
        }
        if (strcmp(g_st.user_pw, g_st.user_pw_confirm) != 0) {
            snprintf(g_st.validation_error, sizeof(g_st.validation_error),
                     "User passwords do not match.");
            return -1;
        }
    }
    return 0;
}

static void draw_screen_user(void)
{
    screen_user_init_fields();
    draw_chrome("User account");

    int cx = g_st.fb_w / 2;
    draw_text18(cx - 100, 72, "User account", 0x00FFFFFF);

    int fx = cx - 220;
    int fy = 120;
    int field_w = 440;
    int field_h = 32;
    int gap     = 16;

    for (int i = 0; i < 5; i++) {
        int y = fy + i * (field_h + gap + 18);
        draw_text14(fx, y, user_fields[i].label, 0x00A0A0B0);
        draw_fill_rect(&g_st.surf, fx, y + 18, field_w, field_h, 0x002A2A3E);
        if (s_user_focus == i)
            draw_rect(&g_st.surf, fx, y + 18, field_w, field_h, 0x004488CC);

        const char *val = user_fields[i].buf;
        int vlen = (int)strlen(val);
        if (user_fields[i].masked && vlen > 0) {
            char stars[64];
            int si;
            for (si = 0; si < vlen && si < 63; si++) stars[si] = '*';
            stars[si] = '\0';
            draw_text14(fx + 8, y + 26, stars, 0x00FFFFFF);
        } else if (vlen > 0) {
            draw_text14(fx + 8, y + 26, val, 0x00FFFFFF);
        }
    }

    /* Back + Next buttons */
    draw_button(cx - 160, g_st.fb_h - 120, 100, 40, "Back", 0);
    draw_button(cx + 60, g_st.fb_h - 120, 100, 40, "Next",
                (s_user_focus == 5));

    /* Validation error message */
    if (g_st.validation_error[0]) {
        int tw = (int)strlen(g_st.validation_error) * 8;
        draw_text14(cx - tw / 2, g_st.fb_h - 160,
                    g_st.validation_error, 0x00FF4444);
    }
}

/* ── Screen 4: Confirm summary ──────────────────────────────────────── */

static void draw_screen_confirm(void)
{
    draw_chrome("Ready to install");

    int cx = g_st.fb_w / 2;
    draw_text18(cx - 120, 96, "Ready to install", 0x00FFFFFF);

    int lx = cx - 220;
    int y = 150;

    draw_text14(lx, y, "Target disk:", 0x00A0A0B0);
    {
        char line[64];
        if (g_st.selected_disk >= 0 && g_st.selected_disk < g_st.ndisks) {
            snprintf(line, sizeof(line), "%s  (%llu MB)",
                     g_st.disks[g_st.selected_disk].name,
                     (unsigned long long)g_st.disks[g_st.selected_disk]
                         .block_count *
                         g_st.disks[g_st.selected_disk].block_size /
                         (1024 * 1024));
        } else {
            snprintf(line, sizeof(line), "(none)");
        }
        draw_text14(lx + 180, y, line, 0x00FFFFFF);
    }
    y += 30;

    draw_text14(lx, y, "Root password:", 0x00A0A0B0);
    draw_text14(lx + 180, y, "set", 0x00FFFFFF);
    y += 30;

    draw_text14(lx, y, "User account:", 0x00A0A0B0);
    draw_text14(lx + 180, y,
                g_st.username[0] ? g_st.username : "(none)",
                0x00FFFFFF);
    y += 60;

    /* If the selected disk already has an AspisOS install, make the
     * destruction explicit. The user has reported losing systems by
     * accidentally re-installing without noticing. */
    int sel = g_st.selected_disk;
    int existing = (sel >= 0 && sel < g_st.ndisks) &&
                   install_disk_has_aegis(g_st.disks[sel].name);
    if (existing) {
        draw_text14(lx, y,
                    "WARNING: this disk already contains an AspisOS install.",
                    0x00FF6060);
        y += 22;
        draw_text14(lx, y,
                    "Pressing Install will ERASE that system permanently.",
                    0x00FF6060);
    } else {
        draw_text14(lx, y,
                    "WARNING: all existing data on the target disk will be erased.",
                    0x00FFAA40);
    }

    /* Back + Install buttons.  Install is the default focus. */
    draw_button(cx - 160, g_st.fb_h - 120, 100, 40, "Back", 0);
    draw_button(cx + 60, g_st.fb_h - 120, 100, 40, "Install", 1);
}

/* ── Progress callbacks (call back into the UI from libinstall) ─────── */

static void prog_on_step(const char *label, void *ctx)
{
    (void)ctx;
    snprintf(g_st.progress_label, sizeof(g_st.progress_label),
             "%s", label);
    g_st.progress_value = 0;
    dprintf(2, "[INSTALLER] step=%s\n", label);
    /* Repaint immediately so the user sees phase transitions. */
    g_st.dirty = 1;
    render_current_screen();
}

static void prog_on_progress(int pct, void *ctx)
{
    (void)ctx;
    g_st.progress_value = pct;
    if (pct % 10 == 0 || pct == 100)
        dprintf(2, "[INSTALLER] progress=%d\n", pct);
    g_st.dirty = 1;
    render_current_screen();
}

static void prog_on_error(const char *msg, void *ctx)
{
    (void)ctx;
    snprintf(g_st.progress_error, sizeof(g_st.progress_error),
             "%s", msg);
    dprintf(2, "[INSTALLER] error=%s\n", msg);
    /* install_run_all returns -1 after calling us, which the caller
     * below turns into install_failed = 1. */
}

static void run_install(void)
{
    if (s_install_started) return;
    s_install_started = 1;

    /* Hash passwords */
    if (install_hash_password(g_st.root_pw,
                              g_st.root_hash,
                              sizeof(g_st.root_hash)) < 0) {
        snprintf(g_st.progress_error, sizeof(g_st.progress_error),
                 "failed to hash root password");
        dprintf(2, "[INSTALLER] error=%s\n", g_st.progress_error);
        g_st.install_failed = 1;
        return;
    }
    if (g_st.username[0]) {
        if (install_hash_password(g_st.user_pw,
                                  g_st.user_hash,
                                  sizeof(g_st.user_hash)) < 0) {
            snprintf(g_st.progress_error, sizeof(g_st.progress_error),
                     "failed to hash user password");
            dprintf(2, "[INSTALLER] error=%s\n", g_st.progress_error);
            g_st.install_failed = 1;
            return;
        }
    }

    install_progress_t p = {
        .on_step     = prog_on_step,
        .on_progress = prog_on_progress,
        .on_error    = prog_on_error,
        .ctx         = NULL,
    };

    int rc = install_run_all(
        g_st.disks[g_st.selected_disk].name,
        g_st.disks[g_st.selected_disk].block_count,
        g_st.disks[g_st.selected_disk].block_size,
        g_st.root_hash,
        g_st.username[0] ? g_st.username     : NULL,
        g_st.username[0] ? g_st.user_hash    : NULL,
        &p);

    if (rc == 0) {
        g_st.install_done = 1;
        dprintf(2, "[INSTALLER] done\n");
    } else {
        g_st.install_failed = 1;
    }
    g_st.dirty = 1;
}

/* ── Screen 5: Progress ─────────────────────────────────────────────── */

static void draw_screen_progress(void)
{
    /* On first draw of Screen 5, run the install synchronously. */
    if (!s_install_started)
        run_install();

    const char *title;
    if (g_st.install_done)        title = "Installation complete";
    else if (g_st.install_failed) title = "Installation failed";
    else                          title = "Installing...";

    draw_chrome(title);
    int cx = g_st.fb_w / 2;

    draw_text18(cx - 160, 96, title, 0x00FFFFFF);

    /* Current phase label */
    if (g_st.progress_label[0])
        draw_text14(cx - 280, 150, g_st.progress_label, 0x00C0C0D0);

    /* Progress bar */
    int bar_x = cx - 240;
    int bar_y = 184;
    int bar_w = 480;
    int bar_h = 24;
    draw_fill_rect(&g_st.surf, bar_x, bar_y, bar_w, bar_h, 0x00181828);
    int fill_w = bar_w * g_st.progress_value / 100;
    if (fill_w > 0)
        draw_fill_rect(&g_st.surf, bar_x, bar_y, fill_w, bar_h, 0x004488CC);
    char pct_txt[8];
    snprintf(pct_txt, sizeof(pct_txt), "%d%%", g_st.progress_value);
    draw_text14(cx - 16, bar_y + bar_h + 10, pct_txt, 0x00A0A0B0);

    /* Error message (on failure) */
    if (g_st.install_failed && g_st.progress_error[0]) {
        draw_text14(cx - 280, 260, "Error:", 0x00FF4444);
        draw_text14(cx - 280, 284, g_st.progress_error, 0x00FF8888);
    }

    /* Buttons — only visible once the run has completed. */
    if (g_st.install_done) {
        draw_button(cx - 50, g_st.fb_h - 120, 100, 40, "Reboot", 1);
    } else if (g_st.install_failed) {
        draw_button(cx - 160, g_st.fb_h - 120, 100, 40, "Retry", 0);
        draw_button(cx + 60,  g_st.fb_h - 120, 100, 40, "Abort", 1);
    }
}

/* ── Per-screen key handlers ────────────────────────────────────────── */

static void handle_key_welcome(char c)
{
    if (c == '\n' || c == '\r') {
        /* Authorize disk access via login -elevate (fed the typed admin
         * password over a pipe). Only advance to the disk screen on success
         * — without an admin session the disk list comes back empty. */
        if (install_elevate(g_st.admin_pw) < 0) {
            snprintf(g_st.validation_error, sizeof(g_st.validation_error),
                     "Admin authentication failed — check the password.");
            dprintf(2, "[INSTALLER] elevate failed\n");
            /* Clear the field so a retry starts clean (a startup focus race
             * with the lumen-probe window can deliver a partial password). */
            g_st.admin_pw[0] = '\0';
            g_st.dirty = 1;
            return;
        }
        g_st.validation_error[0] = '\0';
        dprintf(2, "[INSTALLER] elevated\n");
        g_st.screen = SCREEN_DISK;
        dprintf(2, "[INSTALLER] screen=2\n");
        g_st.dirty = 1;
        return;
    }
    /* Edit the admin-password field. */
    int len = (int)strlen(g_st.admin_pw);
    if (c == '\b' || c == 127) {
        if (len > 0) {
            g_st.admin_pw[len - 1] = '\0';
            g_st.dirty = 1;
        }
        return;
    }
    if (c >= ' ' && c < 127 && len < (int)sizeof(g_st.admin_pw) - 1) {
        g_st.admin_pw[len]     = c;
        g_st.admin_pw[len + 1] = '\0';
        g_st.dirty = 1;
    }
}

static void handle_key_disk(char c)
{
    /* Arrow keys arrive as 3-byte ANSI escape sequences from stsh's
     * cooked mode passthrough (ESC [ A/B). Our TTY is in raw mode so
     * we'll see all three bytes. Parse a simple state machine. */
    static int arrow_state = 0;
    if (arrow_state == 1) {
        arrow_state = (c == '[') ? 2 : 0;
        return;
    }
    if (arrow_state == 2) {
        if (c == 'A' && g_st.selected_disk > 0) {
            g_st.selected_disk--;
            g_st.dirty = 1;
        } else if (c == 'B' && g_st.selected_disk < g_st.ndisks - 1) {
            g_st.selected_disk++;
            g_st.dirty = 1;
        }
        arrow_state = 0;
        return;
    }
    if (c == 27) {
        /* Might be start of arrow or standalone Escape.
         * Defer the decision one byte. */
        arrow_state = 1;
        return;
    }
    if (c == '\n' || c == '\r') {
        if (g_st.ndisks > 0 && g_st.selected_disk >= 0) {
            g_st.screen = SCREEN_USER;
            dprintf(2, "[INSTALLER] screen=3\n");
            g_st.dirty = 1;
        }
    }
}

static void handle_key_user(char c)
{
    screen_user_init_fields();

    if (c == '\t') {
        /* Tab cycles focus 0..5 */
        s_user_focus = (s_user_focus + 1) % 6;
        g_st.dirty = 1;
        return;
    }
    if (c == '\n' || c == '\r') {
        if (s_user_focus < 5) {
            s_user_focus++;
            g_st.dirty = 1;
            return;
        }
        /* Focus was on Next — validate and advance. */
        if (screen_user_validate() == 0) {
            g_st.validation_error[0] = '\0';
            g_st.screen = SCREEN_CONFIRM;
            dprintf(2, "[INSTALLER] screen=4\n");
        }
        g_st.dirty = 1;
        return;
    }
    if (s_user_focus >= 5) return;

    user_field_t *f = &user_fields[s_user_focus];
    int len = (int)strlen(f->buf);
    if (c == '\b' || c == 127) {
        if (len > 0) {
            f->buf[len - 1] = '\0';
            g_st.dirty = 1;
        }
        return;
    }
    if (c >= ' ' && c < 127 && len < f->buf_sz - 1) {
        f->buf[len]     = c;
        f->buf[len + 1] = '\0';
        g_st.dirty = 1;
    }
}

static void handle_key_confirm(char c)
{
    if (c == '\n' || c == '\r') {
        g_st.screen = SCREEN_PROGRESS;
        dprintf(2, "[INSTALLER] screen=5\n");
        g_st.dirty = 1;
    }
}

static void handle_key_progress(char c)
{
    if (!g_st.install_done && !g_st.install_failed)
        return;  /* install still running, ignore keys */

    if (c == '\n' || c == '\r') {
        if (g_st.install_done) {
            /* Reboot — sys_reboot(1) matches Lumen's menu behavior. */
            syscall(SYS_REBOOT, 1L);
            /* If reboot fails, fall through. */
            _exit(0);
        } else if (g_st.install_failed) {
            /* Retry: reset state and go back to the confirm screen. */
            s_install_started = 0;
            g_st.install_failed = 0;
            g_st.progress_value = 0;
            g_st.progress_label[0] = '\0';
            g_st.progress_error[0] = '\0';
            g_st.screen = SCREEN_CONFIRM;
            dprintf(2, "[INSTALLER] screen=4\n");
            g_st.dirty = 1;
        }
    } else if (c == 27) {
        /* Escape on failure = Abort */
        if (g_st.install_failed)
            _exit(1);
    }
}

static void handle_back(void);

/* Synthetic arrow-key codes from Lumen's CSI translator (lumen/main.c).
 * The proxy on_key callback delivers one byte per key event, so multi-
 * byte CSI sequences (ESC [ A/B/C/D) get folded into these high-range
 * codes that don't collide with ASCII / UTF-8. */
#define KEY_ARROW_UP    ((char)0xF1)
#define KEY_ARROW_DOWN  ((char)0xF2)
#define KEY_ARROW_RIGHT ((char)0xF3)
#define KEY_ARROW_LEFT  ((char)0xF4)

static void handle_key(char c)
{
    if (c == '\x1b' || c == KEY_ARROW_LEFT) { handle_back(); return; }
    if (c == KEY_ARROW_RIGHT) { handle_key('\r'); return; }
    switch (g_st.screen) {
    case SCREEN_WELCOME:  handle_key_welcome(c);  break;
    case SCREEN_DISK:     handle_key_disk(c);     break;
    case SCREEN_USER:     handle_key_user(c);     break;
    case SCREEN_CONFIRM:  handle_key_confirm(c);  break;
    case SCREEN_PROGRESS: handle_key_progress(c); break;
    }
}

/* Per-screen Back action. Welcome has no previous screen (no-op);
 * Disk/User/Confirm step backwards through the wizard; Progress
 * piggybacks on the Retry path that handle_key_progress already
 * implements (only meaningful after install_failed). */
static void handle_back(void)
{
    switch (g_st.screen) {
    case SCREEN_WELCOME:
        break;
    case SCREEN_DISK:
        g_st.screen = SCREEN_WELCOME;
        dprintf(2, "[INSTALLER] screen=1\n");
        g_st.dirty = 1;
        break;
    case SCREEN_USER:
        g_st.validation_error[0] = '\0';
        g_st.screen = SCREEN_DISK;
        dprintf(2, "[INSTALLER] screen=2\n");
        g_st.dirty = 1;
        break;
    case SCREEN_CONFIRM:
        g_st.screen = SCREEN_USER;
        dprintf(2, "[INSTALLER] screen=3\n");
        g_st.dirty = 1;
        break;
    case SCREEN_PROGRESS:
        /* The left button is "Retry" while the install is failed —
         * reuse handle_key_progress's existing Enter-to-retry path. */
        if (g_st.install_failed)
            handle_key_progress('\r');
        break;
    }
}

/* Mouse click handling. Buttons are drawn at fixed positions
 * (see draw_screen_*): width 100, height 40, y = fb_h - 120.
 * The "advance" button is either centered (Welcome, Reboot) at
 * (cx - 50, y) or on the right (Disk, User, Confirm, Abort) at
 * (cx + 60, y). The "Back" / "Retry" button is on the left at
 * (cx - 160, y) for Disk/User/Confirm/Progress-failed. */
static void handle_mouse_click(int x, int y)
{
    int cx     = g_st.fb_w / 2;
    int btn_y  = g_st.fb_h - 120;
    int btn_h  = 40;
    int btn_w  = 100;
    if (y < btn_y || y >= btn_y + btn_h) return;
    int left_x0   = cx - 160;
    int center_x0 = cx - 50;
    int right_x0  = cx + 60;
    if ((x >= center_x0 && x < center_x0 + btn_w) ||
        (x >= right_x0  && x < right_x0  + btn_w))
        handle_key('\r');
    else if (x >= left_x0 && x < left_x0 + btn_w)
        handle_back();
}

/* ── Main ───────────────────────────────────────────────────────────── */

int
main(int argc, char **argv)
{
    (void)argc; (void)argv;

    /* Connect to Lumen compositor (retries on ECONNREFUSED). */
    g_st.lfd = lumen_connect_retry();
    if (g_st.lfd < 0) {
        dprintf(2, "gui-installer: lumen_connect failed (%d)\n", g_st.lfd);
        return 1;
    }
    g_st.lwin = lumen_window_create(g_st.lfd, "Install AspisOS", 800, 600);
    if (!g_st.lwin) {
        dprintf(2, "gui-installer: lumen_window_create failed\n");
        return 1;
    }
    g_st.backbuf  = (uint32_t *)g_st.lwin->backbuf;
    g_st.fb_w     = g_st.lwin->w;
    g_st.fb_h     = g_st.lwin->h;
    g_st.pitch_px = g_st.lwin->stride;
    g_st.surf = (surface_t){
        .buf   = g_st.backbuf,
        .w     = g_st.fb_w,
        .h     = g_st.fb_h,
        .pitch = g_st.pitch_px,
    };

    /* Fonts */
    font_init();

    /* SIGTERM → graceful exit */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigterm_handler;
    sigaction(SIGTERM, &sa, NULL);

    /* Initial state */
    g_st.screen = SCREEN_WELCOME;
    g_st.dirty = 1;
    g_st.selected_disk = -1;

    /* Paint first frame and emit readiness marker. */
    render_current_screen();
    dprintf(2, "[LUMEN] installer ready\n");
    dprintf(2, "[INSTALLER] screen=1\n");

    /* Event loop */
    while (!s_term_requested) {
        lumen_event_t ev;
        int r = lumen_wait_event(g_st.lfd, &ev, 16);
        if (r < 0) break;

        if (r == 1) {
            if (ev.type == LUMEN_EV_CLOSE_REQUEST)
                break;
            if (ev.type == LUMEN_EV_KEY && ev.key.pressed)
                handle_key((char)ev.key.keycode);
            if (ev.type == LUMEN_EV_MOUSE &&
                ev.mouse.evtype == LUMEN_MOUSE_DOWN &&
                (ev.mouse.buttons & 1))
                handle_mouse_click(ev.mouse.x, ev.mouse.y);
        }

        render_current_screen();
    }

    lumen_window_destroy(g_st.lwin);
    close(g_st.lfd);
    return 0;
}
