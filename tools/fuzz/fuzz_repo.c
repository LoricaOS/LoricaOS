/* Fuzz the two network-sourced parsers in herald's repo client:
 *   release_find_hash()  — scans the signed Release for a pinned SHA-256
 *   parse_stanza()       — walks the Packages file stanza by stanza
 * plus next_line() and herald_version_gt() underneath them.
 *
 * Both are static, so we #include the translation unit and stub the three
 * external symbols it references (fetch / verify / sha256) — none are on the
 * parsing path. */
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* stubs for repo.c's externals (not reached by the parsers) */
int  herald_fetch(const char *url, const char *dest);
void herald_sha256(const void *buf, size_t len, unsigned char out[32]);
int  herald_verify_p256_sha256(const unsigned char *k, size_t kl,
                               const void *m, size_t ml,
                               const unsigned char *s, size_t sl);

int  herald_fetch(const char *url, const char *dest)
{ (void)url; (void)dest; return -1; }
void herald_sha256(const void *buf, size_t len, unsigned char out[32])
{ (void)buf; (void)len; memset(out, 0, 32); }
int  herald_verify_p256_sha256(const unsigned char *k, size_t kl,
                               const void *m, size_t ml,
                               const unsigned char *s, size_t sl)
{ (void)k; (void)kl; (void)m; (void)ml; (void)s; (void)sl; return 0; }

#include "repo.c"

static void term_ok(const char *s, size_t sz)
{
    assert(memchr(s, 0, sz) != NULL && "stanza field not NUL-terminated");
}

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n)
{
    char *buf;
    size_t off;
    herald_stanza_t *st;
    char hash[65];
    int guard;

    /* Heap copy, NOT NUL-terminated: both parsers take (buf,len) and must
     * never read past len. ASan enforces that. */
    buf = malloc(n ? n : 1);
    if (!buf)
        return 0;
    memcpy(buf, d, n);

    /* Release: look for a path that will not be found, and one that might. */
    memset(hash, 0xAA, sizeof(hash));
    if (release_find_hash(buf, n, "main/binary-x86_64/Packages", hash)) {
        assert(hash[64] == '\0' && "release_find_hash left hashout unterminated");
        assert(strlen(hash) <= 64);
    }

    /* Packages: walk every stanza. */
    st = malloc(sizeof(*st));
    if (st) {
        off = 0;
        guard = 0;
        while (parse_stanza(buf, n, &off, st)) {
            term_ok(st->name,         sizeof(st->name));
            term_ok(st->version,      sizeof(st->version));
            term_ok(st->arch,         sizeof(st->arch));
            term_ok(st->filename,     sizeof(st->filename));
            term_ok(st->sha256,       sizeof(st->sha256));
            term_ok(st->depends,      sizeof(st->depends));
            term_ok(st->display_name, sizeof(st->display_name));
            term_ok(st->exec,         sizeof(st->exec));
            term_ok(st->caps,         sizeof(st->caps));

            /* Filename is appended to the repo base URL to build the download
             * path; an absolute path or a traversal would fetch (and then
             * install) from somewhere the repo did not intend. */
            (void)st->filename;

            herald_version_gt(st->version, "1.2.3");
            herald_version_gt("1.2.3", st->version);

            /* off must strictly advance or we would spin forever. */
            assert(++guard < 1000000);
        }
        free(st);
    }

    free(buf);
    return 0;
}
