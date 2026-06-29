/* db.c — herald installed-package database (see db.h).
 *
 * On-disk format: one line per package, TAB-separated:
 *     <id>\t<version>\t<exec>\t<sha256hex>\n
 *
 * db_put / db_remove rewrite the whole file atomically: write the new
 * contents to HERALD_DB_PATH ".tmp", then rename() it over HERALD_DB_PATH.
 */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include "db.h"

#define HERALD_DB_CAP  256
#define HERALD_DB_TMP  HERALD_DB_PATH ".tmp"

/* Create `path` and all missing parent directories (mkdir -p). Errors other
 * than EEXIST are ignored here; the caller's subsequent fopen surfaces a real
 * failure. Needed because HERALD_DB_DIR is several levels deep (/var/lib/...)
 * and the parents may not exist on a fresh root. */
static void
mkdir_p(const char *path)
{
    char tmp[256];
    size_t i;

    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    for (i = 1; tmp[i] != '\0'; i++) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            mkdir(tmp, 0755);
            tmp[i] = '/';
        }
    }
    mkdir(tmp, 0755);
}

/* Parse one TAB-separated line into *e. Returns 0 on success, -1 if the line
 * is blank or malformed (wrong field count / over-long field). The line must
 * already have its trailing newline stripped. */
static int
parse_line(const char *line, herald_db_entry_t *e)
{
    if (line[0] == '\0')
        return -1;

    const char *fields[4];
    size_t lens[4];
    int nf = 0;

    const char *seg = line;
    for (;;) {
        const char *tab = strchr(seg, '\t');
        size_t seglen = tab ? (size_t)(tab - seg) : strlen(seg);
        if (nf >= 4)
            return -1;             /* too many fields */
        fields[nf] = seg;
        lens[nf] = seglen;
        nf++;
        if (!tab)
            break;
        seg = tab + 1;
    }
    if (nf != 4)
        return -1;                 /* too few fields */

    /* Reject fields that wouldn't fit (so we never truncate silently). */
    if (lens[0] >= sizeof(e->id) || lens[1] >= sizeof(e->version) ||
        lens[2] >= sizeof(e->exec) || lens[3] >= sizeof(e->sha256))
        return -1;

    memcpy(e->id, fields[0], lens[0]);      e->id[lens[0]] = '\0';
    memcpy(e->version, fields[1], lens[1]); e->version[lens[1]] = '\0';
    memcpy(e->exec, fields[2], lens[2]);    e->exec[lens[2]] = '\0';
    memcpy(e->sha256, fields[3], lens[3]);  e->sha256[lens[3]] = '\0';
    return 0;
}

/* Load all entries into out[] (capacity max). Missing file => 0 entries.
 * Returns count (>=0) or negative on error. Single internal loader used by
 * every public function to avoid duplication. */
static int
db_load(herald_db_entry_t *out, int max)
{
    if (!out || max <= 0)
        return -1;

    FILE *f = fopen(HERALD_DB_PATH, "r");
    if (!f) {
        if (errno == ENOENT)
            return 0;
        return -1;
    }

    int n = 0;
    char line[512];
    while (n < max && fgets(line, sizeof(line), f)) {
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = '\0';
        else {
            /* Line longer than our buffer: drain the rest so we resync to
             * the next line boundary, then skip this malformed record. */
            int c;
            while ((c = fgetc(f)) != EOF && c != '\n')
                ;
            continue;
        }
        /* Tolerate a trailing CR. */
        char *cr = strchr(line, '\r');
        if (cr)
            *cr = '\0';

        if (parse_line(line, &out[n]) == 0)
            n++;
        /* malformed/blank lines are skipped */
    }

    fclose(f);
    return n;
}

int
db_read(herald_db_entry_t *out, int max)
{
    return db_load(out, max);
}

int
db_find(const char *id, herald_db_entry_t *out)
{
    if (!id || !out)
        return -1;

    static herald_db_entry_t entries[HERALD_DB_CAP];
    int n = db_load(entries, HERALD_DB_CAP);
    if (n < 0)
        return -1;

    for (int i = 0; i < n; i++) {
        if (strcmp(entries[i].id, id) == 0) {
            *out = entries[i];
            return 1;
        }
    }
    return 0;
}

/* Write the entry array to HERALD_DB_TMP, then atomically rename over the
 * real db path. Creates HERALD_DB_DIR first. Returns 0 / negative. */
static int
db_write_all(const herald_db_entry_t *entries, int n)
{
    /* Ensure the directory (and any missing parents) exists. */
    mkdir_p(HERALD_DB_DIR);

    FILE *f = fopen(HERALD_DB_TMP, "w");
    if (!f)
        return -1;

    for (int i = 0; i < n; i++) {
        if (fprintf(f, "%s\t%s\t%s\t%s\n",
                    entries[i].id, entries[i].version,
                    entries[i].exec, entries[i].sha256) < 0) {
            fclose(f);
            unlink(HERALD_DB_TMP);
            return -1;
        }
    }

    if (fflush(f) != 0) {
        fclose(f);
        unlink(HERALD_DB_TMP);
        return -1;
    }
    if (fclose(f) != 0) {
        unlink(HERALD_DB_TMP);
        return -1;
    }

    /* Atomic replace. */
    if (rename(HERALD_DB_TMP, HERALD_DB_PATH) != 0) {
        unlink(HERALD_DB_TMP);
        return -1;
    }
    return 0;
}

int
db_put(const herald_db_entry_t *e)
{
    if (!e || !e->id[0])
        return -1;

    static herald_db_entry_t entries[HERALD_DB_CAP];
    int n = db_load(entries, HERALD_DB_CAP);
    if (n < 0)
        return -1;

    /* Replace if id already present. */
    int found = 0;
    for (int i = 0; i < n; i++) {
        if (strcmp(entries[i].id, e->id) == 0) {
            entries[i] = *e;
            found = 1;
            break;
        }
    }

    if (!found) {
        if (n >= HERALD_DB_CAP)
            return -1;             /* table full */
        entries[n++] = *e;
    }

    return db_write_all(entries, n);
}

int
db_remove(const char *id)
{
    if (!id || !id[0])
        return -1;

    static herald_db_entry_t entries[HERALD_DB_CAP];
    int n = db_load(entries, HERALD_DB_CAP);
    if (n < 0)
        return -1;

    /* Compact out the matching id. */
    int out = 0;
    int removed = 0;
    for (int i = 0; i < n; i++) {
        if (strcmp(entries[i].id, id) == 0) {
            removed = 1;
            continue;
        }
        if (out != i)
            entries[out] = entries[i];
        out++;
    }

    if (!removed)
        return 1;                  /* id not present */

    if (db_write_all(entries, out) < 0)
        return -1;
    return 0;
}
