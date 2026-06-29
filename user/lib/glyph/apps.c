/* apps.c — /apps bundle scanner (see apps.h). */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>

#include "apps.h"

/* Parse /apps/<id>/app.ini into *app. Returns 1 if the bundle is valid
 * (manifest readable + ELF present), 0 otherwise. */
static int
load_bundle(const char *id, glyph_app_t *app)
{
    /* ids from readdir can't contain '/', but glyph_apps_find() gets ids
     * off the Lumen socket (LUMEN_OP_INVOKE) — reject path separators so
     * a crafted id can't escape /apps, and dotfiles/".."-prefixes. */
    if (!id[0] || id[0] == '.' || strchr(id, '/') ||
        strlen(id) >= sizeof(app->id))
        return 0;

    char ini_path[320];
    snprintf(ini_path, sizeof(ini_path), GLYPH_APPS_DIR "/%s/app.ini", id);
    FILE *f = fopen(ini_path, "r");
    if (!f)
        return 0;

    memset(app, 0, sizeof(*app));
    strncpy(app->id, id, sizeof(app->id) - 1);
    char exec_name[64];
    strncpy(exec_name, id, sizeof(exec_name) - 1);
    exec_name[sizeof(exec_name) - 1] = '\0';

    char line[128];
    while (fgets(line, sizeof(line), f)) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char *key = line, *val = eq + 1;
        if (strcmp(key, "name") == 0 && val[0]) {
            strncpy(app->name, val, sizeof(app->name) - 1);
        } else if (strcmp(key, "exec") == 0 && val[0] &&
                   !strchr(val, '/')) {  /* exec is a filename, not a path */
            strncpy(exec_name, val, sizeof(exec_name) - 1);
            exec_name[sizeof(exec_name) - 1] = '\0';
        }
    }
    fclose(f);

    if (!app->name[0])
        strncpy(app->name, id, sizeof(app->name) - 1);
    snprintf(app->exec, sizeof(app->exec), GLYPH_APPS_DIR "/%s/%s",
             id, exec_name);

    /* A bundle without its binary doesn't exist (vigil deletes the
     * gui-installer ELF on installed systems). */
    if (access(app->exec, F_OK) != 0)
        return 0;
    return 1;
}

static int
app_name_cmp(const void *a, const void *b)
{
    return strcmp(((const glyph_app_t *)a)->name,
                  ((const glyph_app_t *)b)->name);
}

int
glyph_apps_scan(glyph_app_t *out, int max)
{
    if (!out || max <= 0)
        return 0;

    DIR *d = opendir(GLYPH_APPS_DIR);
    if (!d)
        return 0;

    int n = 0;
    struct dirent *de;
    while (n < max && (de = readdir(d)) != NULL) {
        if (load_bundle(de->d_name, &out[n]))
            n++;
    }
    closedir(d);

    qsort(out, (size_t)n, sizeof(out[0]), app_name_cmp);
    return n;
}

int
glyph_apps_find(const char *id, glyph_app_t *out)
{
    if (!id || !out)
        return 0;
    return load_bundle(id, out);
}
