/* Fuzz herald's ini-manifest parser.
 *
 * Beyond memory safety (ASan/UBSan), this asserts the invariants the INSTALLER
 * relies on: `id` becomes a directory name under /apps and `exec` becomes a
 * filename under /etc/aegis/caps.d/, so neither may contain a path separator
 * or be a traversal component. The struct is heap-allocated so ASan gets
 * redzones around it. */
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "manifest.h"

static void no_sep(const char *s, const char *what)
{
    assert(strchr(s, '/') == NULL && "path separator in bare-name field");
    assert(strcmp(s, ".")  != 0 && "field is '.'");
    assert(strcmp(s, "..") != 0 && "field is '..'");
    (void)what;
}

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n)
{
    herald_manifest_t *m = malloc(sizeof(*m));
    if (!m)
        return 0;

    if (manifest_parse(d, n, m) == 0) {
        int i;

        /* Documented postcondition of a successful parse. */
        assert(m->id[0] && m->name[0] && m->version[0]);
        assert(m->is_system || m->exec[0]);

        /* Every char field must be NUL-terminated inside its own array. */
        assert(memchr(m->id,      0, sizeof(m->id))      != NULL);
        assert(memchr(m->name,    0, sizeof(m->name))    != NULL);
        assert(memchr(m->version, 0, sizeof(m->version)) != NULL);
        assert(memchr(m->exec,    0, sizeof(m->exec))    != NULL);
        assert(memchr(m->caps,    0, sizeof(m->caps))    != NULL);
        assert(memchr(m->depends, 0, sizeof(m->depends)) != NULL);
        assert(memchr(m->paths,   0, sizeof(m->paths))   != NULL);

        /* id -> /apps/<id>/ ; exec -> /etc/aegis/caps.d/<exec> */
        no_sep(m->id, "id");
        if (m->exec[0])
            no_sep(m->exec, "exec");

        assert(m->nbincaps >= 0 && m->nbincaps <= HERALD_MAX_BINCAPS);
        for (i = 0; i < m->nbincaps; i++) {
            assert(memchr(m->bincaps[i].binary, 0,
                          sizeof(m->bincaps[i].binary)) != NULL);
            assert(memchr(m->bincaps[i].caps, 0,
                          sizeof(m->bincaps[i].caps)) != NULL);
            /* Written as /etc/aegis/caps.d/<binary>. */
            no_sep(m->bincaps[i].binary, "caps.<binary>");
        }
    }

    free(m);
    return 0;
}
