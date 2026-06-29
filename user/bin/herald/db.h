#ifndef HERALD_DB_H
#define HERALD_DB_H
#include <stddef.h>

#define HERALD_DB_DIR  "/var/lib/herald"
#define HERALD_DB_PATH "/var/lib/herald/db"

typedef struct {
    char id[64];
    char version[32];
    char exec[64];
    char sha256[65];   /* 64 hex chars + NUL */
} herald_db_entry_t;

/* Read all entries into out[] (capacity max). Missing file => returns 0.
 * Returns count (>=0) or negative on error. */
int db_read(herald_db_entry_t *out, int max);

/* Insert or replace (matched by id). Creates HERALD_DB_DIR if needed.
 * Returns 0 on success, negative on error. */
int db_put(const herald_db_entry_t *e);

/* Remove entry by id. Returns 0 if removed, 1 if not present, negative error. */
int db_remove(const char *id);

/* Find by id: 1 found (fills *out), 0 not found, negative error. */
int db_find(const char *id, herald_db_entry_t *out);
#endif
