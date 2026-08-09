/* selftest.c — host-side regression checks for herald's parsers.
 *
 * These are the invariants the installer's containment depends on, and both
 * were broken until a fuzzing pass on 2026-08-09 found them:
 *
 *   1. id / exec / caps.<binary> must be BARE NAMES. They are pasted straight
 *      into paths (/apps/<id>/, /etc/aegis/caps.d/<exec>), and the parser only
 *      rejected '/'. "." and ".." got through, so `herald remove ..` ran
 *      unlink("/apps/../<exec>") — a delete at /.
 *   2. herald_version_gt must not overflow. It accumulated into a signed long,
 *      which is UB past ~19 digits and wraps negative in practice, inverting
 *      the anti-rollback guard so a replayed older package reads as newer.
 *
 * Built and run on the BUILD HOST (plain cc, no musl/BearSSL): `make selftest`.
 * It links manifest.c + repo.c against the stubs below, which stand in for the
 * crypto/network symbols repo.c needs but the tested parsers never call.
 */
#include <stdio.h>
#include <string.h>

#include "manifest.h"
#include "repo.h"

/* repo.c's externals: not reached by herald_version_gt. */
int  herald_fetch(const char *url, const char *dest)
{ (void)url; (void)dest; return -1; }
void herald_sha256(const void *b, size_t l, unsigned char o[32])
{ (void)b; (void)l; memset(o, 0, 32); }
int  herald_verify_p256_sha256(const unsigned char *k, size_t kl, const void *m,
                               size_t ml, const unsigned char *s, size_t sl)
{ (void)k; (void)kl; (void)m; (void)ml; (void)s; (void)sl; return 0; }

static int fails;

#define CHECK(cond) do {                                                      \
    if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
                   fails++; }                                                 \
} while (0)

static int parses(const char *text, herald_manifest_t *m)
{
    return manifest_parse(text, strlen(text), m) == 0;
}

static void test_bare_names(void)
{
    herald_manifest_t m;

    CHECK(manifest_is_bare_name("lumen-files"));
    CHECK(manifest_is_bare_name("a"));
    CHECK(!manifest_is_bare_name(""));
    CHECK(!manifest_is_bare_name("."));
    CHECK(!manifest_is_bare_name(".."));
    CHECK(!manifest_is_bare_name("a/b"));
    CHECK(!manifest_is_bare_name("../etc"));
    /* Names that merely start with dots are legitimate files. */
    CHECK(manifest_is_bare_name("...."));
    CHECK(manifest_is_bare_name(".hidden"));

    /* A well-formed manifest still parses. */
    CHECK(parses("id=fuzz\nname=Fuzz\nversion=1.0\nexec=fuzz\n", &m));
    CHECK(strcmp(m.id, "fuzz") == 0);

    /* id must not be a traversal component. */
    CHECK(!parses("id=..\nname=X\nversion=1\nexec=x\n", &m));
    CHECK(!parses("id=.\nname=X\nversion=1\nexec=x\n", &m));
    CHECK(!parses("id=a/b\nname=X\nversion=1\nexec=x\n", &m));

    /* Nor exec. */
    CHECK(!parses("id=x\nname=X\nversion=1\nexec=..\n", &m));
    CHECK(!parses("id=x\nname=X\nversion=1\nexec=.\n", &m));

    /* caps.<binary> is silently ignored rather than fatal, so assert the entry
     * simply never lands — it would become /etc/aegis/caps.d/.. otherwise. */
    CHECK(parses("id=x\nname=X\nversion=1\nexec=x\ncaps...=admin\n", &m));
    CHECK(m.nbincaps == 0);
    CHECK(parses("id=x\nname=X\nversion=1\nexec=x\ncaps..=admin\n", &m));
    CHECK(m.nbincaps == 0);
    /* ...while a real per-binary policy still works. */
    CHECK(parses("id=x\nname=X\nversion=1\nexec=x\ncaps.helper=service\n", &m));
    CHECK(m.nbincaps == 1 && strcmp(m.bincaps[0].binary, "helper") == 0);
}

static void test_version_gt(void)
{
    /* Ordinary ordering. */
    CHECK(herald_version_gt("1.0.1", "1.0.0"));
    CHECK(!herald_version_gt("1.0.0", "1.0.1"));
    CHECK(!herald_version_gt("2.0", "2.0"));
    CHECK(herald_version_gt("1.10", "1.9"));

    /* Components past LONG_MAX (~9.2e18) used to wrap negative. The
     * anti-rollback shape: installed is newer, so a downgrade must be
     * detected. This is the assertion that failed before the fix. */
    CHECK(herald_version_gt("9999999999999999999", "1.0"));
    CHECK(!herald_version_gt("1.0", "9999999999999999999"));
    /* Beyond 64 bits the value saturates, so two such components compare
     * EQUAL rather than inverting — deliberate, and still monotone. */
    CHECK(!herald_version_gt("1.0.99999999999999999999",
                             "1.0.99999999999999999998"));
    CHECK(!herald_version_gt("1.0.99999999999999999998",
                             "1.0.99999999999999999999"));

    /* Long-but-in-range components must keep ordering correctly too. */
    CHECK(herald_version_gt("1.0.1754870400000000001",
                            "1.0.1754870400000000000"));
    CHECK(!herald_version_gt("1.0.1754870400000000000",
                             "1.0.1754870400000000001"));
}

int main(void)
{
    test_bare_names();
    test_version_gt();
    if (fails) {
        printf("herald selftest: %d FAILED\n", fails);
        return 1;
    }
    printf("herald selftest: all checks passed\n");
    return 0;
}
