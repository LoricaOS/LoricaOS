/* theme.c — runtime theming for Glyph.
 *
 * The surface/text tokens in glyph_theme.h are fixed (dark), but the ACCENT
 * color is runtime-selectable: glyph_theme.h defines THEME_ACCENT et al. as
 * reads of the global g_glyph_theme, which this file owns and which
 * glyph_theme_load() populates from config at startup (font_init calls it).
 *
 * Config: a one-line `accent=<name>` file, loaded from /etc/aegis/theme.conf
 * (system default) then $HOME/.config/aegis/theme.conf (per-user override).
 * glyph_theme_save() persists the per-user file.
 */
#include "glyph_theme.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

/* Live theme state — default accent "indigo", animations on, 24-hour clock,
 * traditional scrolling, UTC, dark theme, normal pointer, Aegis wallpaper,
 * night light off, NTP on. Palette fields default to the dark theme
 * ("obsidian": near-black neutrals with a violet undertone — 1.1.0 facelift;
 * the old palette was a bluer slate). Keep in sync with apply_palette(). */
glyph_theme_t g_glyph_theme = {
    .accent = 0x006366F1u, .accent_hover = 0x007C7FF5u, .accent_active = 0x004F52D6u,
    .animations = 1, .clock_24h = 1, .natural_scroll = 0, .tz_offset_min = 0,
    .light = 0, .pointer_speed = 150, .wallpaper = 0, .night_light = 0, .ntp_auto = 1,
    /* terminal: Lorica scheme, 16px, block cursor, blinking, 500-line history */
    .term_scheme = 0, .term_font_px = 16, .term_cursor = 0, .term_blink = 1,
    .term_scrollback = 500,
    /* dark palette */
    .bg = 0x000D0E14u, .surface = 0x00171923u, .surface_2 = 0x001F2230u,
    .hover = 0x002A2E40u, .input_bg = 0x0011121Au,
    .desktop_top = 0x001D1C30u, .desktop_bot = 0x000B0B12u,
    .border = 0x002A2D3Cu, .border_strong = 0x003B3F52u,
    .text = 0x00ECEDF4u, .text_dim = 0x00A0A5B6u, .text_faint = 0x00626880u,
    .text_on_accent = 0x00FFFFFFu,
    .ok = 0x0030C85Au, .warn = 0x00E8B23Eu, .error = 0x00FF5F57u,
};

/* Wallpaper presets (rendered procedurally by the compositor). */
static const char *s_walls[] = { "LoricaOS", "Midnight", "Slate", "Accent" };
static const int   s_nwalls = (int)(sizeof(s_walls) / sizeof(s_walls[0]));

/* Set the 13 palette fields from the theme mode. */
static void
apply_palette(int light)
{
    if (light) {
        g_glyph_theme.bg             = 0x00E6E9F0u;
        g_glyph_theme.surface        = 0x00FBFCFEu;
        g_glyph_theme.surface_2      = 0x00EEF1F6u;
        g_glyph_theme.hover          = 0x00E0E5EFu;
        g_glyph_theme.input_bg       = 0x00FFFFFFu;
        g_glyph_theme.desktop_top    = 0x00D6E2F2u;
        g_glyph_theme.desktop_bot    = 0x00BCCEE6u;
        g_glyph_theme.border         = 0x00D0D6E0u;
        g_glyph_theme.border_strong  = 0x00B4BFCEu;
        g_glyph_theme.text           = 0x001A2230u;
        g_glyph_theme.text_dim       = 0x00586678u;
        g_glyph_theme.text_faint     = 0x0095A0B0u;
        g_glyph_theme.text_on_accent = 0x00FFFFFFu;
        /* Deeper status hues for AA contrast on the light surfaces. */
        g_glyph_theme.ok             = 0x001E8E3Eu;
        g_glyph_theme.warn           = 0x00B06000u;
        g_glyph_theme.error          = 0x00C5221Fu;
    } else {
        /* "Obsidian" — near-black neutrals, violet undertone (1.1.0). */
        g_glyph_theme.bg             = 0x000D0E14u;
        g_glyph_theme.surface        = 0x00171923u;
        g_glyph_theme.surface_2      = 0x001F2230u;
        g_glyph_theme.hover          = 0x002A2E40u;
        g_glyph_theme.input_bg       = 0x0011121Au;
        g_glyph_theme.desktop_top    = 0x001D1C30u;
        g_glyph_theme.desktop_bot    = 0x000B0B12u;
        g_glyph_theme.border         = 0x002A2D3Cu;
        g_glyph_theme.border_strong  = 0x003B3F52u;
        g_glyph_theme.text           = 0x00ECEDF4u;
        g_glyph_theme.text_dim       = 0x00A0A5B6u;
        g_glyph_theme.text_faint     = 0x00626880u;
        g_glyph_theme.text_on_accent = 0x00FFFFFFu;
        /* Dark-tuned status hues (the prior fixed values). */
        g_glyph_theme.ok             = 0x0030C85Au;
        g_glyph_theme.warn           = 0x00E8B23Eu;
        g_glyph_theme.error          = 0x00FF5F57u;
    }
    g_glyph_theme.light = light ? 1 : 0;
}

/* Accent presets. base/hover/active are 0x00RRGGBB. */
static const struct {
    const char *name;
    uint32_t base, hover, active;
} s_accents[] = {
    { "blue",   0x004A90E8u, 0x005E9FF0u, 0x003A7BCCu },
    { "indigo", 0x006366F1u, 0x007C7FF5u, 0x004F52D6u },
    { "purple", 0x009B59E0u, 0x00AE72ECu, 0x007E42C4u },
    { "pink",   0x00E0529Bu, 0x00EC6FB0u, 0x00C43F82u },
    { "red",    0x00E0524Au, 0x00EC6E66u, 0x00C43F38u },
    { "orange", 0x00E08A3Au, 0x00ECA15Eu, 0x00C4722Au },
    { "green",  0x0033C06Au, 0x004FD083u, 0x0028A055u },
    { "teal",   0x002BB5C0u, 0x004FCAD4u, 0x002196A0u },
};

static int s_naccent = (int)(sizeof(s_accents) / sizeof(s_accents[0]));

static void user_config_path(char *out, size_t outsz);   /* fwd */

int
glyph_theme_accent_count(void)
{
    return s_naccent;
}

const char *
glyph_theme_accent_name(int i)
{
    return (i >= 0 && i < s_naccent) ? s_accents[i].name : "";
}

uint32_t
glyph_theme_accent_color(int i)
{
    return (i >= 0 && i < s_naccent) ? s_accents[i].base : 0;
}

int
glyph_theme_current_accent(void)
{
    for (int i = 0; i < s_naccent; i++)
        if (g_glyph_theme.accent == s_accents[i].base)
            return i;
    return -1;
}

void
glyph_theme_set_accent(int i)
{
    if (i < 0 || i >= s_naccent)
        return;
    g_glyph_theme.accent        = s_accents[i].base;
    g_glyph_theme.accent_hover  = s_accents[i].hover;
    g_glyph_theme.accent_active = s_accents[i].active;
}

int  glyph_theme_animations(void)        { return g_glyph_theme.animations; }
void glyph_theme_set_animations(int on)  { g_glyph_theme.animations = on ? 1 : 0; }
int  glyph_theme_clock24(void)           { return g_glyph_theme.clock_24h; }
void glyph_theme_set_clock24(int on)     { g_glyph_theme.clock_24h = on ? 1 : 0; }
int  glyph_theme_natural_scroll(void)    { return g_glyph_theme.natural_scroll; }
void glyph_theme_set_natural_scroll(int on) { g_glyph_theme.natural_scroll = on ? 1 : 0; }
int  glyph_theme_tz_offset(void)         { return g_glyph_theme.tz_offset_min; }
void glyph_theme_set_tz_offset(int m)    { g_glyph_theme.tz_offset_min = m; }
int  glyph_theme_light(void)             { return g_glyph_theme.light; }
void glyph_theme_set_light(int on)       { apply_palette(on ? 1 : 0); }
int  glyph_theme_pointer_speed(void)     { return g_glyph_theme.pointer_speed; }
void glyph_theme_set_pointer_speed(int p){ g_glyph_theme.pointer_speed = p; }
int  glyph_theme_wallpaper(void)         { return g_glyph_theme.wallpaper; }
void glyph_theme_set_wallpaper(int i)    { if (i >= 0 && i < s_nwalls) g_glyph_theme.wallpaper = i; }
int  glyph_theme_wallpaper_count(void)   { return s_nwalls; }
const char *glyph_theme_wallpaper_name(int i) { return (i >= 0 && i < s_nwalls) ? s_walls[i] : ""; }
int  glyph_theme_night_light(void)       { return g_glyph_theme.night_light; }
void glyph_theme_set_night_light(int on) { g_glyph_theme.night_light = on ? 1 : 0; }
int  glyph_theme_ntp_auto(void)          { return g_glyph_theme.ntp_auto; }
void glyph_theme_set_ntp_auto(int on)    { g_glyph_theme.ntp_auto = on ? 1 : 0; }
int  glyph_theme_term_scheme(void)       { return g_glyph_theme.term_scheme; }
void glyph_theme_set_term_scheme(int i)  { if (i >= 0) g_glyph_theme.term_scheme = i; }
int  glyph_theme_term_font_px(void)      { return g_glyph_theme.term_font_px; }
void glyph_theme_set_term_font_px(int px){ if (px >= 8 && px <= 40) g_glyph_theme.term_font_px = px; }
int  glyph_theme_term_cursor(void)       { return g_glyph_theme.term_cursor; }
void glyph_theme_set_term_cursor(int s)  { if (s >= 0 && s <= 2) g_glyph_theme.term_cursor = s; }
int  glyph_theme_term_blink(void)        { return g_glyph_theme.term_blink; }
void glyph_theme_set_term_blink(int on)  { g_glyph_theme.term_blink = on ? 1 : 0; }
int  glyph_theme_term_scrollback(void)   { return g_glyph_theme.term_scrollback; }
void glyph_theme_set_term_scrollback(int l) { if (l >= 100 && l <= 100000) g_glyph_theme.term_scrollback = l; }

/* Find `key` at the start of any line in buf; return the value pointer (just
 * past key) or NULL. Line-anchored so "light=" won't match in "night_light=". */
static const char *
find_key(const char *buf, const char *key)
{
    size_t klen = strlen(key);
    const char *p = buf;
    while (p && *p) {
        if (strncmp(p, key, klen) == 0)
            return p + klen;
        const char *nl = strchr(p, '\n');
        p = nl ? nl + 1 : NULL;
    }
    return NULL;
}

/* Parse `<key>=on|off` (also 1/0/true/yes); updates *v if the key is present. */
static void
parse_bool_key(const char *buf, const char *key, int *v)
{
    const char *p = find_key(buf, key);
    if (!p)
        return;
    int on = (*p == '1' || *p == 'o' || *p == 'O' || *p == 't' || *p == 'y');
    if ((p[0] == 'o' || p[0] == 'O') && (p[1] == 'f' || p[1] == 'F'))
        on = 0;            /* "off" starts 'o' like "on" — 2nd char decides */
    *v = on ? 1 : 0;
}

/* Parse `<key>=<signed int>`; updates *v if the key is present. */
static void
parse_int_key(const char *buf, const char *key, int *v)
{
    const char *p = find_key(buf, key);
    if (!p)
        return;
    int sign = 1;
    if (*p == '-') { sign = -1; p++; }
    else if (*p == '+') p++;
    if (*p < '0' || *p > '9')
        return;
    int n = 0;
    while (*p >= '0' && *p <= '9') { n = n * 10 + (*p - '0'); p++; }
    *v = sign * n;
}

/* Apply all runtime-pref keys found in `buf` to the live theme. */
static void
apply_prefs(const char *buf)
{
    parse_bool_key(buf, "animations=",     &g_glyph_theme.animations);
    parse_bool_key(buf, "clock24=",        &g_glyph_theme.clock_24h);
    parse_bool_key(buf, "natural_scroll=", &g_glyph_theme.natural_scroll);
    parse_int_key (buf, "tz_offset=",      &g_glyph_theme.tz_offset_min);
    parse_bool_key(buf, "light=",          &g_glyph_theme.light);
    parse_int_key (buf, "pointer_speed=",  &g_glyph_theme.pointer_speed);
    parse_int_key (buf, "wallpaper=",      &g_glyph_theme.wallpaper);
    parse_bool_key(buf, "night_light=",    &g_glyph_theme.night_light);
    parse_bool_key(buf, "ntp=",            &g_glyph_theme.ntp_auto);
    parse_int_key (buf, "term_scheme=",    &g_glyph_theme.term_scheme);
    parse_int_key (buf, "term_font=",      &g_glyph_theme.term_font_px);
    parse_int_key (buf, "term_cursor=",    &g_glyph_theme.term_cursor);
    parse_bool_key(buf, "term_blink=",     &g_glyph_theme.term_blink);
    parse_int_key (buf, "term_scrollback=",&g_glyph_theme.term_scrollback);
    apply_palette(g_glyph_theme.light);    /* keep palette in sync with mode */
}

/* Re-read the runtime prefs (not accent) from config — cheap, no accent side
 * effects. Lets a running compositor pick up Settings changes without restart. */
void
glyph_theme_reload_prefs(void)
{
    char up[256];
    user_config_path(up, sizeof(up));
    const char *paths[2] = { "/etc/aegis/theme.conf", up };
    for (int i = 0; i < 2; i++) {       /* per-user file (last) wins */
        int fd = open(paths[i], O_RDONLY);
        if (fd < 0)
            continue;
        char buf[512];
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n <= 0)
            continue;
        buf[n] = '\0';
        apply_prefs(buf);
    }
}

static int
accent_index_by_name(const char *name)
{
    for (int i = 0; i < s_naccent; i++)
        if (strcmp(name, s_accents[i].name) == 0)
            return i;
    return -1;
}

/* Apply `accent=<name>` and `animations=on|off` from a config file.
 * Returns 1 if anything was applied. */
static int
load_config_file(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    char buf[512];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return 0;
    buf[n] = '\0';

    int applied = 0;

    const char *p = strstr(buf, "accent=");
    if (p) {
        p += 7;
        char name[32];
        int k = 0;
        while (*p && *p != '\n' && *p != '\r' && *p != ' ' &&
               k < (int)sizeof(name) - 1)
            name[k++] = *p++;
        name[k] = '\0';
        int idx = accent_index_by_name(name);
        if (idx >= 0) {
            glyph_theme_set_accent(idx);
            applied = 1;
        }
    }

    /* Any runtime-pref key present → apply them (apply_prefs only touches the
     * fields whose keys appear, and re-syncs the palette to `light`). */
    if (strchr(buf, '=')) {
        apply_prefs(buf);
        applied = 1;
    }

    return applied;
}

static void
user_config_path(char *out, size_t outsz)
{
    const char *home = getenv("HOME");
    if (!home || !home[0])
        home = "/";
    snprintf(out, outsz, "%s/.config/aegis/theme.conf", home);
}

int
glyph_theme_load(void)
{
    int applied = 0;
    /* System default first, then per-user override (user wins). */
    applied |= load_config_file("/etc/aegis/theme.conf");
    char up[256];
    user_config_path(up, sizeof(up));
    applied |= load_config_file(up);
    return applied;
}

int
glyph_theme_save(void)
{
    const char *home = getenv("HOME");
    if (!home || !home[0])
        home = "/";
    char dir[256];
    snprintf(dir, sizeof(dir), "%s/.config", home);
    mkdir(dir, 0755);
    snprintf(dir, sizeof(dir), "%s/.config/aegis", home);
    mkdir(dir, 0755);

    char path[256];
    user_config_path(path, sizeof(path));
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return 0;
    int idx = glyph_theme_current_accent();
    if (idx < 0)
        idx = 0;
    char line[512];
    int len = snprintf(line, sizeof(line),
                       "accent=%s\nanimations=%s\nclock24=%s\n"
                       "natural_scroll=%s\ntz_offset=%d\nlight=%s\n"
                       "pointer_speed=%d\nwallpaper=%d\nnight_light=%s\nntp=%s\n"
                       "term_scheme=%d\nterm_font=%d\nterm_cursor=%d\n"
                       "term_blink=%s\nterm_scrollback=%d\n",
                       s_accents[idx].name,
                       g_glyph_theme.animations ? "on" : "off",
                       g_glyph_theme.clock_24h ? "on" : "off",
                       g_glyph_theme.natural_scroll ? "on" : "off",
                       g_glyph_theme.tz_offset_min,
                       g_glyph_theme.light ? "on" : "off",
                       g_glyph_theme.pointer_speed,
                       g_glyph_theme.wallpaper,
                       g_glyph_theme.night_light ? "on" : "off",
                       g_glyph_theme.ntp_auto ? "on" : "off",
                       g_glyph_theme.term_scheme,
                       g_glyph_theme.term_font_px,
                       g_glyph_theme.term_cursor,
                       g_glyph_theme.term_blink ? "on" : "off",
                       g_glyph_theme.term_scrollback);
    ssize_t w = write(fd, line, (size_t)len);
    close(fd);
    return w == len;
}
