/* repo.c — herald's Debian-style repository client (Chancery format).
 *
 * sync   : for each source in /etc/herald/sources.list, fetch Release +
 *          Release.sig, verify the signature against the embedded trust key,
 *          then fetch the per-arch Packages and verify its SHA-256 against the
 *          hash pinned by the (now trusted) Release. The verified Packages is
 *          cached under /var/lib/herald/lists/.
 * find   : resolve a package name to its best candidate across cached lists.
 * search : list matching packages.
 *
 * Trust chain (Debian model): one signature over Release -> pins Packages
 * hash -> pins package hash. Packages are NOT individually signed.
 */

#include "repo.h"
#include "net.h"
#include "verify.h"
#include "trusted_key.h"   /* herald_trusted_key[65] */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

typedef struct {
    char url[256];
    char suite[64];
    char component[64];
} herald_source_t;

/* ---- small helpers ---------------------------------------------------- */

static int read_file(const char *path, unsigned char **out, size_t *outlen)
{
    int fd = open(path, O_RDONLY);
    struct stat st;
    unsigned char *buf;
    size_t n, got = 0;
    if (fd < 0)
        return -1;
    if (fstat(fd, &st) < 0 || st.st_size < 0) { close(fd); return -1; }
    n = (size_t)st.st_size;
    buf = malloc(n ? n : 1);
    if (!buf) { close(fd); return -1; }
    while (got < n) {
        ssize_t r = read(fd, buf + got, n - got);
        if (r < 0) { if (errno == EINTR) continue; free(buf); close(fd); return -1; }
        if (r == 0) break;
        got += (size_t)r;
    }
    close(fd);
    *out = buf;
    *outlen = got;
    return 0;
}

static void mkdir_p(const char *path)
{
    char tmp[256];
    size_t i;
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    for (i = 1; tmp[i]; i++) {
        if (tmp[i] == '/') { tmp[i] = '\0'; mkdir(tmp, 0755); tmp[i] = '/'; }
    }
    mkdir(tmp, 0755);
}

static void hex32(const unsigned char *d, char out[65])
{
    static const char h[] = "0123456789abcdef";
    int i;
    for (i = 0; i < 32; i++) { out[i*2] = h[(d[i]>>4)&0xf]; out[i*2+1] = h[d[i]&0xf]; }
    out[64] = '\0';
}

/* Copy the next '\n'-terminated line into `line` (CR-trimmed). 1 = got a line. */
static int next_line(const char *buf, size_t len, size_t *off, char *line, size_t linesz)
{
    size_t s, llen;
    if (*off >= len)
        return 0;
    s = *off;
    while (*off < len && buf[*off] != '\n')
        (*off)++;
    llen = *off - s;
    if (*off < len)
        (*off)++;
    if (llen >= linesz)
        llen = linesz - 1;
    memcpy(line, buf + s, llen);
    line[llen] = '\0';
    if (llen > 0 && line[llen - 1] == '\r')
        line[llen - 1] = '\0';
    return 1;
}

/* Compare dotted-numeric versions; 1 if a > b. */
static int version_gt(const char *a, const char *b)
{
    while (*a || *b) {
        long na = 0, nb = 0;
        while (*a && *a != '.') { if (*a >= '0' && *a <= '9') na = na*10 + (*a-'0'); a++; }
        while (*b && *b != '.') { if (*b >= '0' && *b <= '9') nb = nb*10 + (*b-'0'); b++; }
        if (na != nb) return na > nb;
        if (*a == '.') a++;
        if (*b == '.') b++;
    }
    return 0;
}

/* Sanitize "url suite component" into a filesystem-safe cache key. */
static void srckey(const herald_source_t *s, char *out, size_t outsz)
{
    char raw[400];
    size_t i;
    snprintf(raw, sizeof(raw), "%s_%s_%s", s->url, s->suite, s->component);
    for (i = 0; raw[i] && i < outsz - 1; i++) {
        char c = raw[i];
        out[i] = ((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')) ? c : '_';
    }
    out[i] = '\0';
}

static int sources_load(herald_source_t *out, int max)
{
    unsigned char *buf;
    size_t len, off = 0;
    int n = 0;
    if (read_file(HERALD_SOURCES, &buf, &len) != 0)
        return -1;
    while (n < max && off < len) {
        char line[400], *p;
        char url[256], suite[64], comp[64];
        size_t ul;
        next_line((const char *)buf, len, &off, line, sizeof(line));
        p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0' || *p == '#')
            continue;
        if (sscanf(p, "%255s %63s %63s", url, suite, comp) != 3)
            continue;
        ul = strlen(url);
        while (ul > 0 && url[ul-1] == '/') url[--ul] = '\0';   /* strip trailing / */
        strncpy(out[n].url, url, sizeof(out[n].url)-1);   out[n].url[sizeof(out[n].url)-1]='\0';
        strncpy(out[n].suite, suite, sizeof(out[n].suite)-1); out[n].suite[sizeof(out[n].suite)-1]='\0';
        strncpy(out[n].component, comp, sizeof(out[n].component)-1); out[n].component[sizeof(out[n].component)-1]='\0';
        n++;
    }
    free(buf);
    return n;
}

/* Find the SHA-256 of `wantpath` in a Release file's "SHA256:" section. */
static int release_find_hash(const char *buf, size_t len, const char *wantpath, char *hashout)
{
    size_t off = 0;
    char line[400];
    int in_sha = 0;
    while (next_line(buf, len, &off, line, sizeof(line))) {
        if (strncmp(line, "SHA256:", 7) == 0) { in_sha = 1; continue; }
        if (in_sha) {
            char hash[80], path[256];
            long size;
            const char *p = line;
            if (line[0] != ' ') { in_sha = 0; continue; }
            while (*p == ' ') p++;
            if (sscanf(p, "%79s %ld %255s", hash, &size, path) == 3 &&
                strcmp(path, wantpath) == 0) {
                strncpy(hashout, hash, 64);
                hashout[64] = '\0';
                return 1;
            }
        }
    }
    return 0;
}

/* Parse the next Packages stanza starting at *off. 1 = parsed (fills *s). */
static int parse_stanza(const char *buf, size_t len, size_t *off, herald_stanza_t *s)
{
    char line[400];
    int got = 0;
    memset(s, 0, sizeof(*s));
    while (next_line(buf, len, off, line, sizeof(line))) {
        char *c, *key, *val;
        if (line[0] == '\0') { if (got) break; else continue; }
        got = 1;
        c = strchr(line, ':');
        if (!c) continue;
        *c = '\0';
        key = line;
        val = c + 1;
        while (*val == ' ') val++;
        if      (!strcmp(key,"Package"))      strncpy(s->name, val, sizeof(s->name)-1);
        else if (!strcmp(key,"Version"))      strncpy(s->version, val, sizeof(s->version)-1);
        else if (!strcmp(key,"Architecture")) strncpy(s->arch, val, sizeof(s->arch)-1);
        else if (!strcmp(key,"Filename"))     strncpy(s->filename, val, sizeof(s->filename)-1);
        else if (!strcmp(key,"SHA256"))       strncpy(s->sha256, val, sizeof(s->sha256)-1);
        else if (!strcmp(key,"Depends"))      strncpy(s->depends, val, sizeof(s->depends)-1);
        else if (!strcmp(key,"Display-Name")) strncpy(s->display_name, val, sizeof(s->display_name)-1);
        else if (!strcmp(key,"Exec"))         strncpy(s->exec, val, sizeof(s->exec)-1);
        else if (!strcmp(key,"Caps"))         strncpy(s->caps, val, sizeof(s->caps)-1);
    }
    return got && s->name[0];
}

/* ---- public API ------------------------------------------------------- */

int repo_sync(void)
{
    herald_source_t srcs[16];
    int n, i, total = 0;

    n = sources_load(srcs, 16);
    if (n < 0) {
        fprintf(stderr, "herald: no sources configured (%s)\n", HERALD_SOURCES);
        return -1;
    }
    if (n == 0) {
        fprintf(stderr, "herald: %s is empty\n", HERALD_SOURCES);
        return -1;
    }
    mkdir_p(HERALD_LISTS_DIR);

    for (i = 0; i < n; i++) {
        herald_source_t *s = &srcs[i];
        char key[400], url[700], relpath[520], sigpath[540], pkgpath[520];
        char want[160], wanthash[65], gothash[65];
        unsigned char *rbuf, *sbuf, *pbuf, dg[32];
        size_t rlen, slen, plen, off;
        herald_stanza_t st;

        srckey(s, key, sizeof(key));

        /* Release + signature */
        snprintf(url, sizeof(url), "%s/dists/%s/Release", s->url, s->suite);
        snprintf(relpath, sizeof(relpath), "%s/%s.Release", HERALD_LISTS_DIR, key);
        if (herald_fetch(url, relpath) != 0) {
            fprintf(stderr, "herald: failed to fetch Release from %s (%s)\n", s->url, s->suite);
            return -1;
        }
        snprintf(url, sizeof(url), "%s/dists/%s/Release.sig", s->url, s->suite);
        snprintf(sigpath, sizeof(sigpath), "%s.sig", relpath);
        if (herald_fetch(url, sigpath) != 0) {
            fprintf(stderr, "herald: failed to fetch Release signature\n");
            return -1;
        }
        if (read_file(relpath, &rbuf, &rlen) != 0 || read_file(sigpath, &sbuf, &slen) != 0) {
            fprintf(stderr, "herald: cannot read fetched Release\n");
            return -1;
        }
        if (!herald_verify_p256_sha256(herald_trusted_key, sizeof(herald_trusted_key),
                                       rbuf, rlen, sbuf, slen)) {
            fprintf(stderr, "herald: RELEASE SIGNATURE VERIFICATION FAILED for %s %s — "
                            "repository not trusted\n", s->url, s->suite);
            free(rbuf); free(sbuf);
            return -1;
        }
        free(sbuf);

        /* Packages, pinned by the verified Release */
        snprintf(want, sizeof(want), "%s/binary-%s/Packages", s->component, HERALD_ARCH);
        if (!release_find_hash((char *)rbuf, rlen, want, wanthash)) {
            fprintf(stderr, "herald: Release for %s has no %s\n", s->suite, want);
            free(rbuf);
            return -1;
        }
        free(rbuf);

        snprintf(url, sizeof(url), "%s/dists/%s/%s/binary-%s/Packages",
                 s->url, s->suite, s->component, HERALD_ARCH);
        snprintf(pkgpath, sizeof(pkgpath), "%s/%s.Packages", HERALD_LISTS_DIR, key);
        if (herald_fetch(url, pkgpath) != 0) {
            fprintf(stderr, "herald: failed to fetch Packages\n");
            return -1;
        }
        if (read_file(pkgpath, &pbuf, &plen) != 0) {
            fprintf(stderr, "herald: cannot read fetched Packages\n");
            return -1;
        }
        herald_sha256(pbuf, plen, dg);
        hex32(dg, gothash);
        if (strcmp(gothash, wanthash) != 0) {
            fprintf(stderr, "herald: Packages hash mismatch for %s %s — possible tampering\n",
                    s->url, s->suite);
            free(pbuf);
            return -1;
        }

        off = 0;
        while (parse_stanza((char *)pbuf, plen, &off, &st))
            total++;
        free(pbuf);
    }
    return total;
}

int repo_find(const char *name, herald_stanza_t *out)
{
    herald_source_t srcs[16];
    int n, i, found = 0;

    n = sources_load(srcs, 16);
    if (n < 0)
        return -1;

    for (i = 0; i < n; i++) {
        char key[400], pkgpath[520];
        unsigned char *pbuf;
        size_t plen, off = 0;
        herald_stanza_t st;
        srckey(&srcs[i], key, sizeof(key));
        snprintf(pkgpath, sizeof(pkgpath), "%s/%s.Packages", HERALD_LISTS_DIR, key);
        if (read_file(pkgpath, &pbuf, &plen) != 0)
            continue;
        while (parse_stanza((char *)pbuf, plen, &off, &st)) {
            if (strcmp(st.name, name) != 0)
                continue;
            if (st.arch[0] && strcmp(st.arch, HERALD_ARCH) != 0)
                continue;
            if (!found || version_gt(st.version, out->version)) {
                strncpy(st.base_url, srcs[i].url, sizeof(st.base_url) - 1);
                st.base_url[sizeof(st.base_url) - 1] = '\0';
                *out = st;
                found = 1;
            }
        }
        free(pbuf);
    }
    return found ? 1 : 0;
}

int repo_search(const char *term)
{
    herald_source_t srcs[16];
    int n, i, hits = 0, any_list = 0;

    n = sources_load(srcs, 16);
    if (n < 0)
        return -1;

    for (i = 0; i < n; i++) {
        char key[400], pkgpath[520];
        unsigned char *pbuf;
        size_t plen, off = 0;
        herald_stanza_t st;
        srckey(&srcs[i], key, sizeof(key));
        snprintf(pkgpath, sizeof(pkgpath), "%s/%s.Packages", HERALD_LISTS_DIR, key);
        if (read_file(pkgpath, &pbuf, &plen) != 0)
            continue;
        any_list = 1;
        while (parse_stanza((char *)pbuf, plen, &off, &st)) {
            if (term[0] == '\0' || strstr(st.name, term) || strstr(st.display_name, term)) {
                printf("%-24s %-12s %s\n", st.name, st.version, st.display_name);
                hits++;
            }
        }
        free(pbuf);
    }
    if (!any_list) {
        fprintf(stderr, "herald: no package lists — run 'herald sync' first\n");
        return -1;
    }
    if (!hits)
        printf("no matching packages\n");
    return 0;
}
