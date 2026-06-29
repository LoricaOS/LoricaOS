/* apps.h — /apps bundle scanner.
 *
 * System-wide application bundles live in /apps/<id>/ containing the ELF
 * (named by app.ini's exec=, default <id>) and an app.ini manifest:
 *
 *     name=Calculator
 *     exec=calculator
 *
 * /apps is root-controlled like /bin: the kernel cap-policy trusted-path
 * anchor accepts it, so bundles may carry caps.d policies. Per-user app
 * dirs are deliberately NOT scanned — a user-writable path must never be
 * a policy subject, and unprivileged binaries already run from anywhere
 * with baseline caps.
 *
 * Consumers: Lumen's invoke handler (id → exec path), the Applications
 * launcher grid, citadel-dock.
 */
#ifndef GLYPH_APPS_H
#define GLYPH_APPS_H

#define GLYPH_APPS_DIR "/apps"
#define GLYPH_APPS_MAX 64

typedef struct {
    char id[64];     /* bundle dir name — LUMEN_OP_INVOKE name + icon key */
    char name[64];   /* display name from app.ini (defaults to id) */
    char exec[256];  /* absolute path to the ELF */
} glyph_app_t;

/* Scan /apps; fill out[] (up to max), sorted by display name.
 * Bundles whose ELF is missing are skipped (e.g. gui-installer after
 * vigil removes it on an installed system). Returns the count. */
int glyph_apps_scan(glyph_app_t *out, int max);

/* Look up one bundle by id. Returns 1 and fills *out, or 0. */
int glyph_apps_find(const char *id, glyph_app_t *out);

#endif /* GLYPH_APPS_H */
