#ifndef HERALD_REPO_H
#define HERALD_REPO_H
#include <stddef.h>

/* herald's architecture (the only one it can install). Carried in Packages so
 * a multi-arch repo serves the right binaries. */
#define HERALD_ARCH       "x86_64"
#define HERALD_SOURCES    "/etc/herald/sources.list"
#define HERALD_LISTS_DIR  "/var/lib/herald/lists"

/* One parsed Packages stanza (a candidate to install). */
typedef struct {
    char name[64];          /* Package: (herald id / install name) */
    char version[48];
    char arch[16];
    char filename[192];     /* path under the repo base (pool/...) */
    char sha256[65];        /* hash pinned by the signed Release->Packages chain */
    char depends[256];
    char display_name[64];
    char exec[64];
    char caps[256];
    char base_url[256];     /* the source base URL this came from (for download) */
} herald_stanza_t;

/* Fetch + verify Release (signature against the embedded trust key) and the
 * per-arch Packages (hash pinned by the verified Release) for every source in
 * /etc/herald/sources.list. Returns total package count, or negative on error. */
int repo_sync(void);

/* Find the best (highest-version) candidate named `name` across all cached,
 * verified Packages lists. 1 = found (fills *out), 0 = not found, <0 error. */
int repo_find(const char *name, herald_stanza_t *out);

/* Print every cached package whose name/display name matches `term`
 * ("" matches all). Returns 0, or negative if no synced lists exist. */
int repo_search(const char *term);

#endif
