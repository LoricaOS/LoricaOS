#ifndef HERALD_MANIFEST_H
#define HERALD_MANIFEST_H
#include <stddef.h>

/* Per-binary capability policy: a manifest may declare caps for executables it
 * ships beyond its own launcher (e.g. an engine's RequestServer) via lines of
 * the form `caps.<binary>=<space-separated caps>`. Each entry is written to
 * /etc/aegis/caps.d/<binary> at install (subject to the same allow-list and
 * collision guards as the primary caps=). */
#define HERALD_MAX_BINCAPS 8
typedef struct {
    char binary[64];    /* exec basename the policy applies to */
    char caps[256];     /* space-separated cap names */
} herald_bincap_t;

typedef struct {
    char id[64];        /* bundle dir under /apps; required */
    char name[64];      /* display name; required */
    char version[32];   /* required */
    char exec[64];      /* ELF basename inside apps/<id>/; required */
    char caps[256];     /* raw value: space-separated cap names; "" if none */
    char depends[256];  /* raw value: space-separated ids; "" if none */
    char paths[256];    /* raw value: space-separated extra install prefixes
                         * (e.g. "apps/lantern/ lib/ladybird/"); "" => default
                         * to apps/<id>/ only. Lets a package install beyond its
                         * bundle dir (a browser shipping its engine library). */
    herald_bincap_t bincaps[HERALD_MAX_BINCAPS]; /* caps.<binary>= entries */
    int             nbincaps;
} herald_manifest_t;

/* Parse an ini manifest from memory (buf/len). key=value lines, '#' comments,
 * lines without '=' skipped, newline-terminated. Trims trailing CR/space.
 * Returns 0 on success (id, name, version, exec all present & non-empty),
 * negative if a required key is missing or input malformed. */
int manifest_parse(const void *buf, size_t len, herald_manifest_t *out);
#endif
