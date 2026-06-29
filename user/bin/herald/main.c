/*
 * herald — Aegis package manager.
 *
 * A herald bears trusted, signed software from beyond the walls. It verifies a
 * package's ECDSA-P256/SHA-256 signature against a baked-in trusted key,
 * extracts its /apps bundle into place, installs its capability policy, and
 * records it in a local database. No software enters the system unless its
 * signature verifies — and the authority to mutate the system app tree is an
 * unforgeable kernel capability (CAP_KIND_INSTALL) held only by herald in an
 * authenticated admin session.
 *
 * Usage:
 *   herald install <file.hpkg>     verify, extract, register
 *   herald verify  <file.hpkg>     signature check only (no install)
 *   herald info    <file.hpkg|id>  print a package's manifest / installed info
 *   herald list                    list installed packages
 *   herald remove  <id>            uninstall a package
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/syscall.h>

#include "tar.h"
#include "verify.h"
#include "manifest.h"
#include "db.h"
#include "net.h"
#include "repo.h"
#include "trusted_key.h"   /* static const unsigned char herald_trusted_key[65] */

#define HERALD_CACHE_DIR "/var/lib/herald/cache"
#define HERALD_MAX_INSTALL 64
#define HERALD_ANCHORS_FILE "/etc/aegis/anchors"
#define SYS_INSTALL_COMMIT 516   /* kernel: reload cap policy + anchors, no reboot */

/* ---- helpers ---------------------------------------------------------- */

/* Read an entire file into a malloc'd buffer. Caller frees *out. */
static int read_file(const char *path, unsigned char **out, size_t *outlen)
{
    int fd;
    struct stat st;
    unsigned char *buf;
    size_t n, got = 0;

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }
    if (fstat(fd, &st) < 0 || st.st_size < 0) {
        close(fd);
        return -1;
    }
    n = (size_t)st.st_size;
    buf = malloc(n ? n : 1);
    if (buf == NULL) {
        close(fd);
        return -1;
    }
    while (got < n) {
        ssize_t r = read(fd, buf + got, n - got);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            free(buf);
            close(fd);
            return -1;
        }
        if (r == 0) {
            break;
        }
        got += (size_t)r;
    }
    close(fd);
    *out = buf;
    *outlen = got;
    return 0;
}

static void hex32(const unsigned char *d, char out[65])
{
    static const char h[] = "0123456789abcdef";
    int i;
    for (i = 0; i < 32; i++) {
        out[i * 2]     = h[(d[i] >> 4) & 0xf];
        out[i * 2 + 1] = h[d[i] & 0xf];
    }
    out[64] = '\0';
}

static int ends_with(const char *s, const char *suf)
{
    size_t ls = strlen(s), lf = strlen(suf);
    return ls >= lf && strcmp(s + ls - lf, suf) == 0;
}

/* Capabilities a third-party /apps bundle is permitted to request. Anything
 * outside this allow-list (POWER, INSTALL, DISK_ADMIN, SETUID, NET_ADMIN,
 * AUTH, CAP_GRANT/DELEGATE/QUERY, ...) would let a package escalate itself or
 * — since caps.d is keyed by exec basename — another binary, so herald refuses
 * to install it. Allow-list, not deny-list: unknown/future caps default-deny. */
static int caps_are_safe(const char *caps, char *bad, size_t badsz)
{
    static const char *allowed[] = {
        "FB", "IPC", "NET_SOCKET", "PROC_READ", "THREAD_CREATE",
        "VFS_OPEN", "VFS_READ", "VFS_WRITE", 0
    };
    const char *p = caps;
    while (*p) {
        const char *start;
        size_t len;
        int ok = 0, i;
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;
        start = p;
        while (*p && *p != ' ' && *p != '\t')
            p++;
        len = (size_t)(p - start);
        for (i = 0; allowed[i]; i++) {
            if (strlen(allowed[i]) == len &&
                strncmp(start, allowed[i], len) == 0) {
                ok = 1;
                break;
            }
        }
        if (!ok) {
            if (bad && badsz) {
                size_t n = len < badsz - 1 ? len : badsz - 1;
                memcpy(bad, start, n);
                bad[n] = '\0';
            }
            return 0;
        }
    }
    return 1;
}

/*
 * Load a package file, verify its detached signature against the trusted key,
 * and parse its manifest. On success buf and len hold the (caller-freed) package
 * bytes and *m the manifest. Returns 0 on success, -1 on any failure (message
 * already printed).
 */
static int load_verified_pkg(const char *path, unsigned char **buf,
                             size_t *len, herald_manifest_t *m)
{
    char sigpath[1024];
    unsigned char *sig = NULL;
    size_t siglen = 0;
    const unsigned char *md = NULL;
    size_t ms = 0;
    int ok, found;

    if (read_file(path, buf, len) != 0) {
        fprintf(stderr, "herald: cannot read %s: %s\n", path, strerror(errno));
        return -1;
    }

    if (snprintf(sigpath, sizeof(sigpath), "%s.sig", path) >= (int)sizeof(sigpath)) {
        fprintf(stderr, "herald: path too long\n");
        free(*buf);
        return -1;
    }
    if (read_file(sigpath, &sig, &siglen) != 0) {
        fprintf(stderr, "herald: missing signature %s\n", sigpath);
        free(*buf);
        return -1;
    }

    ok = herald_verify_p256_sha256(herald_trusted_key, sizeof(herald_trusted_key),
                                   *buf, *len, sig, siglen);
    free(sig);
    if (!ok) {
        fprintf(stderr, "herald: SIGNATURE VERIFICATION FAILED for %s\n", path);
        free(*buf);
        return -1;
    }

    found = tar_find_mem(*buf, *len, "manifest", &md, &ms);
    if (found != 1) {
        fprintf(stderr, "herald: package has no manifest\n");
        free(*buf);
        return -1;
    }
    if (manifest_parse(md, ms, m) != 0) {
        fprintf(stderr, "herald: invalid manifest\n");
        free(*buf);
        return -1;
    }
    return 0;
}

/* ---- repository helpers ----------------------------------------------- */

/* mkdir -p: create `path` and any missing parents (errors but EEXIST ignored). */
static void mkdir_p(const char *path)
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

/* Register a non-/apps install prefix as a trusted-path anchor by appending its
 * absolute path to /etc/aegis/anchors (deduped). The kernel reloads the file on
 * sys_install_commit, after which it BOTH grants policy caps to and write-
 * protects that tree — preserving the "trusted-for-granting == write-protected"
 * invariant for an engine library (e.g. /lib/ladybird) shipped outside /apps.
 * `relprefix` is a relative dir like "lib/ladybird/"; it is normalized to an
 * absolute path with no trailing slash ("/lib/ladybird"). */
static void anchor_register(const char *relprefix)
{
    char abspath[128];
    char line[160];
    size_t n = strlen(relprefix);
    FILE *f;

    while (n > 0 && relprefix[n - 1] == '/')
        n--;
    if (n == 0 || n + 1 >= sizeof(abspath))
        return;
    abspath[0] = '/';
    memcpy(abspath + 1, relprefix, n);
    abspath[n + 1] = '\0';

    /* Dedup: skip if the exact path is already an anchor. */
    f = fopen(HERALD_ANCHORS_FILE, "r");
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            size_t l = strlen(line);
            while (l > 0 && (line[l - 1] == '\n' || line[l - 1] == '\r' ||
                             line[l - 1] == ' ' || line[l - 1] == '\t'))
                line[--l] = '\0';
            if (strcmp(line, abspath) == 0) {
                fclose(f);
                return;
            }
        }
        fclose(f);
    }

    f = fopen(HERALD_ANCHORS_FILE, "a");
    if (!f) {
        fprintf(stderr, "herald: warning: could not register anchor %s: %s\n",
                abspath, strerror(errno));
        return;
    }
    fprintf(f, "%s\n", abspath);
    fclose(f);
    printf("registered trusted-path anchor %s\n", abspath);
}

/* ---- commands --------------------------------------------------------- */

static int cmd_verify(const char *path)
{
    unsigned char *buf;
    size_t len;
    herald_manifest_t m;

    if (load_verified_pkg(path, &buf, &len, &m) != 0) {
        return 1;
    }
    printf("OK: %s %s — signature valid\n", m.id, m.version);
    free(buf);
    return 0;
}

static int cmd_info(const char *arg)
{
    unsigned char *buf;
    size_t len;
    herald_manifest_t m;

    if (ends_with(arg, ".hpkg")) {
        if (load_verified_pkg(arg, &buf, &len, &m) != 0) {
            return 1;
        }
        printf("id:      %s\n", m.id);
        printf("name:    %s\n", m.name);
        printf("version: %s\n", m.version);
        printf("exec:    %s\n", m.exec);
        if (m.caps[0]) {
            printf("caps:    %s\n", m.caps);
        }
        if (m.depends[0]) {
            printf("depends: %s\n", m.depends);
        }
        free(buf);
        return 0;
    } else {
        herald_db_entry_t e;
        int f = db_find(arg, &e);
        if (f < 0) {
            fprintf(stderr, "herald: database error\n");
            return 1;
        }
        if (f == 0) {
            fprintf(stderr, "herald: %s is not installed\n", arg);
            return 1;
        }
        printf("id:      %s\n", e.id);
        printf("version: %s\n", e.version);
        printf("exec:    %s\n", e.exec);
        printf("sha256:  %s\n", e.sha256);
        return 0;
    }
}

static int cmd_list(void)
{
    /* static, not on the stack: 256 entries is ~56KB and Aegis user stacks
     * are small — an on-stack array of this size overflows the stack. */
    static herald_db_entry_t ents[256];
    int n, i;

    n = db_read(ents, 256);
    if (n < 0) {
        fprintf(stderr, "herald: database error\n");
        return 1;
    }
    if (n == 0) {
        printf("no packages installed\n");
        return 0;
    }
    for (i = 0; i < n; i++) {
        printf("%-24s %s\n", ents[i].id, ents[i].version);
    }
    return 0;
}

/* Install an already-AUTHENTICATED package (verified by signature for local
 * files, or by the signed-metadata hash for repo downloads). Parses the
 * manifest, enforces the capability allow-list, extracts the bundle into
 * /apps/<id>/, writes the cap policy, and records it in the db. Returns 0. */
static int install_bytes(const unsigned char *buf, size_t len)
{
    herald_manifest_t m;
    const unsigned char *md;
    size_t ms;
    const char *err = NULL;
    char prefix[80], badcap[64], cp[256], hex[65];
    unsigned char dg[32];
    herald_db_entry_t e;

    if (tar_find_mem(buf, len, "manifest", &md, &ms) != 1) {
        fprintf(stderr, "herald: package has no manifest\n");
        return -1;
    }
    if (manifest_parse(md, ms, &m) != 0) {
        fprintf(stderr, "herald: invalid manifest\n");
        return -1;
    }

    /* Capability escalation guard — checked before anything is written. */
    if (m.caps[0] && !caps_are_safe(m.caps, badcap, sizeof(badcap))) {
        fprintf(stderr, "herald: refusing %s: package requests disallowed "
                        "capability '%s'\n", m.id, badcap);
        return -1;
    }

    /* Cap-policy collision guard — checked before anything is written.
     *
     * The kernel keys /etc/aegis/caps.d/ by the installed binary's BASENAME
     * (cap_policy_lookup extracts the basename of /apps/<id>/<exec>, i.e.
     * m.exec). If herald let a package pick an exec basename different from its
     * own bundle id, a package "id=evil exec=reboot" would write
     * /etc/aegis/caps.d/reboot and clobber the system reboot binary's
     * `service POWER` policy with attacker-chosen contents — a policy-tamper /
     * privilege-by-filename attack independent of the cap allow-list above
     * (which only constrains the package's own caps, not which file it lands in).
     *
     * Require exec == id. The bundle is extracted to /apps/<id>/ and id is the
     * unique namespace key (db_put replaces by id; the dir is per-id), so this
     * makes the caps.d file a package writes provably equal to — and only — its
     * own bundle id. Every shipped /apps bundle already satisfies exec == id,
     * and the kernel only honours a policy whose basename matches the installed
     * binary, so this breaks no legitimate package. manifest_parse already
     * rejects a '/' in either field, so id is a safe bare filename. */
    if (strcmp(m.exec, m.id) != 0) {
        fprintf(stderr, "herald: refusing %s: manifest exec '%s' must equal the "
                        "bundle id '%s' (a package may only own its own cap "
                        "policy)\n", m.id, m.exec, m.id);
        return -1;
    }

    /* Refuse to overwrite a caps.d file we do not already own. A pre-existing
     * policy belongs to a system component or another package unless the db
     * records THIS id as already installed (an upgrade/reinstall, which is
     * allowed to replace its own file). Defends the system policies (reboot,
     * login, bastion, ...) — which are not in herald's db — and stays correct
     * even if the exec==id invariant above is ever relaxed. */
    if (m.caps[0]) {
        herald_db_entry_t prev;
        snprintf(cp, sizeof(cp), "/etc/aegis/caps.d/%s", m.exec);
        if (access(cp, F_OK) == 0 && db_find(m.id, &prev) != 1) {
            fprintf(stderr, "herald: refusing %s: cap policy %s already exists "
                            "and is not owned by this package\n", m.id, cp);
            return -1;
        }
    }

    /* Per-binary cap-policy guards — same allow-list + collision rules as the
     * primary caps= above. A package may ship policies for executables beyond
     * its launcher (e.g. an engine's RequestServer). The allow-list still
     * default-denies escalation caps, and the collision guard still refuses to
     * clobber a caps.d file we don't already own — so a package can only create
     * NEW policy files for its own (safe-capped) binaries, never overwrite a
     * system policy (reboot/login/...). Checked before anything is written. */
    {
        int bi, reinstall;
        herald_db_entry_t prev;
        reinstall = (db_find(m.id, &prev) == 1);
        for (bi = 0; bi < m.nbincaps; bi++) {
            herald_bincap_t *bc = &m.bincaps[bi];
            if (strchr(bc->binary, '/')) {
                fprintf(stderr, "herald: refusing %s: per-binary cap target "
                                "'%s' must be a bare filename\n",
                        m.id, bc->binary);
                return -1;
            }
            if (!caps_are_safe(bc->caps, badcap, sizeof(badcap))) {
                fprintf(stderr, "herald: refusing %s: per-binary policy for "
                                "'%s' requests disallowed capability '%s'\n",
                        m.id, bc->binary, badcap);
                return -1;
            }
            snprintf(cp, sizeof(cp), "/etc/aegis/caps.d/%s", bc->binary);
            if (access(cp, F_OK) == 0 && !reinstall) {
                fprintf(stderr, "herald: refusing %s: cap policy %s already "
                                "exists and is not owned by this package\n",
                        m.id, cp);
                return -1;
            }
        }
    }

    /* Build the allow-list of install prefixes. Default: just the package's own
     * bundle dir apps/<id>/. A manifest may declare extra prefixes via paths=
     * (space-separated) to also ship, e.g., an engine library under lib/ladybird/
     * — /apps is the default, but a package can install anywhere it declares.
     * Each declared prefix must be a relative dir ending in '/', contain no "..",
     * and stay out of the cap-policy tree (caps belong in caps=, written through
     * the audited path below, never as raw package file contents). */
    {
        const char *prefixes[18];
        int np = 0;
        char *s = m.paths;

        snprintf(prefix, sizeof(prefix), "apps/%s/", m.id);
        prefixes[np++] = prefix;
        while (*s && np < 16) {
            char *tok;
            size_t tl;
            while (*s == ' ' || *s == '\t')
                s++;
            if (!*s)
                break;
            tok = s;
            while (*s && *s != ' ' && *s != '\t')
                s++;
            if (*s)
                *s++ = '\0';
            tl = strlen(tok);
            if (tok[0] == '/' || tl == 0 || tok[tl - 1] != '/' ||
                strstr(tok, "..") != NULL ||
                strncmp(tok, "etc/aegis/", 10) == 0) {
                fprintf(stderr, "herald: refusing %s: illegal install path "
                                "'%s' (must be a relative dir ending in '/', "
                                "no '..', not under etc/aegis/)\n", m.id, tok);
                return -1;
            }
            prefixes[np++] = tok;
        }
        prefixes[np] = NULL;
        err = NULL;
        if (tar_extract_mem(buf, len, "/", prefixes, &err) != 0) {
            if (errno == EPERM || errno == EACCES) {
                fprintf(stderr, "herald: install denied — modifying the system "
                                "app tree requires an authenticated admin "
                                "session\n");
            } else {
                fprintf(stderr, "herald: install failed: %s\n",
                        err ? err : "extraction error");
            }
            return -1;
        }

        /* Register every install prefix outside apps/ as a trusted-path anchor.
         * The default apps/<id>/ (prefixes[0]) and anything else under apps/ is
         * already covered by the builtin /apps anchor; skip those. */
        {
            int pi;
            for (pi = 0; prefixes[pi]; pi++) {
                if (strncmp(prefixes[pi], "apps/", 5) == 0)
                    continue;
                anchor_register(prefixes[pi]);
            }
        }
    }

    if (m.caps[0]) {
        FILE *cf;
        snprintf(cp, sizeof(cp), "/etc/aegis/caps.d/%s", m.exec);
        cf = fopen(cp, "w");
        if (cf == NULL) {
            fprintf(stderr, "herald: warning: could not write cap policy %s: %s\n",
                    cp, strerror(errno));
        } else {
            fprintf(cf, "service %s\n", m.caps);
            fclose(cf);
        }
    }

    /* Per-binary cap policies (guards already enforced above). */
    {
        int bi;
        for (bi = 0; bi < m.nbincaps; bi++) {
            FILE *bcf;
            snprintf(cp, sizeof(cp), "/etc/aegis/caps.d/%s", m.bincaps[bi].binary);
            bcf = fopen(cp, "w");
            if (bcf == NULL) {
                fprintf(stderr, "herald: warning: could not write cap policy "
                                "%s: %s\n", cp, strerror(errno));
            } else {
                fprintf(bcf, "service %s\n", m.bincaps[bi].caps);
                fclose(bcf);
            }
        }
    }

    herald_sha256(buf, len, dg);
    hex32(dg, hex);
    memset(&e, 0, sizeof(e));
    strncpy(e.id, m.id, sizeof(e.id) - 1);
    strncpy(e.version, m.version, sizeof(e.version) - 1);
    strncpy(e.exec, m.exec, sizeof(e.exec) - 1);
    strncpy(e.sha256, hex, sizeof(e.sha256) - 1);
    if (db_put(&e) != 0) {
        fprintf(stderr, "herald: warning: installed but failed to record in database\n");
    }

    /* Reload kernel cap policy + trusted-path anchors so the caps.d files and
     * anchors written above take effect immediately, with no reboot. */
    if (syscall(SYS_INSTALL_COMMIT) != 0) {
        fprintf(stderr, "herald: warning: kernel policy reload failed — new "
                        "caps/anchors take effect on next boot\n");
    } else {
        printf("kernel cap policy + anchors reloaded\n");
    }

    printf("installed %s %s -> /apps/%s\n", m.id, m.version, m.id);
    return 0;
}

/* Install a local .hpkg file: verify its detached signature, then install. */
static int cmd_install(const char *path)
{
    unsigned char *buf;
    size_t len;
    herald_manifest_t m;
    int r;

    if (load_verified_pkg(path, &buf, &len, &m) != 0) {
        return 1;
    }
    r = install_bytes(buf, len);
    free(buf);
    return r ? 1 : 0;
}

/* Fetch + verify the repository metadata (Release signature, then the per-arch
 * Packages pinned by the verified Release) for every configured source. */
static int cmd_sync(void)
{
    int n = repo_sync();
    if (n < 0) {
        return 1;
    }
    printf("synced %d package%s\n", n, n == 1 ? "" : "s");
    return 0;
}

static int cmd_search(const char *term)
{
    return repo_search(term) < 0 ? 1 : 0;
}

/* Build the install set for `name`, deps first, skipping already-installed and
 * already-queued packages. Recursive. Returns 0 on success, -1 on error. */
static int resolve(const char *name, herald_stanza_t *set, int *count, int max)
{
    herald_db_entry_t de;
    herald_stanza_t st;
    int i, f;

    if (db_find(name, &de) == 1) {
        return 0;   /* already installed — dependency satisfied */
    }
    for (i = 0; i < *count; i++) {
        if (strcmp(set[i].name, name) == 0) {
            return 0;   /* already queued */
        }
    }
    f = repo_find(name, &st);
    if (f < 0) {
        fprintf(stderr, "herald: no package lists — run 'herald sync' first\n");
        return -1;
    }
    if (f == 0) {
        fprintf(stderr, "herald: package '%s' not found in any repository\n", name);
        return -1;
    }
    if (st.depends[0]) {
        char deps[256], *save = NULL, *tok;
        strncpy(deps, st.depends, sizeof(deps) - 1);
        deps[sizeof(deps) - 1] = '\0';
        for (tok = strtok_r(deps, " ", &save); tok; tok = strtok_r(NULL, " ", &save)) {
            if (resolve(tok, set, count, max) != 0) {
                return -1;
            }
        }
    }
    if (*count >= max) {
        fprintf(stderr, "herald: dependency set too large\n");
        return -1;
    }
    set[*count] = st;
    (*count)++;
    return 0;
}

/* Install a package (and its dependencies) by name from the repositories:
 * resolve the set, download each, verify each against the SHA-256 pinned by the
 * signed Release->Packages chain (no per-package signature needed), and install
 * deps first. */
static int cmd_install_named(const char *name)
{
    static herald_stanza_t set[HERALD_MAX_INSTALL];
    int count = 0, i;

    if (strchr(name, '/') != NULL) {
        fprintf(stderr, "herald: invalid package name\n");
        return 1;
    }
    if (resolve(name, set, &count, HERALD_MAX_INSTALL) != 0) {
        return 1;
    }
    if (count == 0) {
        printf("%s is already installed\n", name);
        return 0;
    }
    if (count > 1) {
        printf("installing %d packages:", count);
        for (i = 0; i < count; i++) {
            printf(" %s", set[i].name);
        }
        printf("\n");
    }

    mkdir_p(HERALD_CACHE_DIR);
    for (i = 0; i < count; i++) {
        herald_stanza_t *st = &set[i];
        char url[800], cache[256], hex[65];
        unsigned char *pb, dg[32];
        size_t pl;

        snprintf(cache, sizeof(cache), "%s/%s.hpkg", HERALD_CACHE_DIR, st->name);
        snprintf(url, sizeof(url), "%s/%s", st->base_url, st->filename);
        if (herald_fetch(url, cache) != 0) {
            fprintf(stderr, "herald: failed to download %s\n", st->filename);
            return 1;
        }
        if (read_file(cache, &pb, &pl) != 0) {
            fprintf(stderr, "herald: cannot read downloaded %s\n", st->name);
            return 1;
        }
        herald_sha256(pb, pl, dg);
        hex32(dg, hex);
        if (strcmp(hex, st->sha256) != 0) {
            fprintf(stderr, "herald: %s failed its hash check (signed metadata "
                            "mismatch) — refusing\n", st->name);
            free(pb);
            return 1;
        }
        if (install_bytes(pb, pl) != 0) {
            free(pb);
            return 1;
        }
        free(pb);
    }
    return 0;
}

static int cmd_remove(const char *id)
{
    herald_db_entry_t e;
    char p[256];
    int f;

    if (strchr(id, '/') != NULL) {
        fprintf(stderr, "herald: invalid package id\n");
        return 1;
    }
    f = db_find(id, &e);
    if (f < 0) {
        fprintf(stderr, "herald: database error\n");
        return 1;
    }
    if (f == 0) {
        fprintf(stderr, "herald: %s is not installed\n", id);
        return 1;
    }

    /* Re-validate the db-stored exec basename before it goes into any path.
     * manifest_parse rejects '/' in exec at install time, but the db file on
     * disk (/var/lib/herald/db) is plain text and could be tampered between
     * install and remove; db_load only length-bounds the field, it does not
     * re-check for '/'. A doctored exec like "../../etc/aegis/caps.d/reboot"
     * would otherwise let unlink() escape /apps/<id>/ and delete a system
     * binary or cap policy. The id is already validated above (strchr at the
     * top), so /apps/%s and the id segment are safe; only e.exec needs this. */
    if (strchr(e.exec, '/') != NULL) {
        fprintf(stderr, "herald: refusing %s: database entry has a malformed "
                        "exec field '%s' (contains '/')\n", id, e.exec);
        return 1;
    }

    snprintf(p, sizeof(p), "/apps/%s/%s", id, e.exec);
    unlink(p);
    snprintf(p, sizeof(p), "/apps/%s/app.ini", id);
    unlink(p);
    snprintf(p, sizeof(p), "/apps/%s", id);
    if (rmdir(p) != 0 && (errno == EPERM || errno == EACCES)) {
        fprintf(stderr, "herald: remove denied — requires an authenticated "
                        "admin session\n");
        return 1;
    }
    snprintf(p, sizeof(p), "/etc/aegis/caps.d/%s", e.exec);
    unlink(p);
    db_remove(id);

    printf("removed %s\n", id);
    return 0;
}

static void usage(void)
{
    fprintf(stderr,
        "usage:\n"
        "  herald sync                    fetch + verify repository metadata\n"
        "  herald search <term>           search the synced package lists\n"
        "  herald install <name>          install from a repository (with deps)\n"
        "  herald install <file.hpkg>     install a local package file\n"
        "  herald verify  <file.hpkg>     check a package's signature only\n"
        "  herald info    <file.hpkg|id>  show package / installed info\n"
        "  herald list                    list installed packages\n"
        "  herald remove  <id>            uninstall a package\n"
        "\n"
        "sources are configured in " HERALD_SOURCES " (lines: <url> <suite> <component>)\n");
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage();
        return 2;
    }
    if (strcmp(argv[1], "list") == 0) {
        return cmd_list();
    }
    if (strcmp(argv[1], "sync") == 0) {
        return cmd_sync();
    }
    if (argc < 3) {
        usage();
        return 2;
    }
    if (strcmp(argv[1], "install") == 0) {
        /* A path ending in .hpkg is a local file; anything else is a repo
         * package name resolved via the cached index. */
        return ends_with(argv[2], ".hpkg")
                   ? cmd_install(argv[2])
                   : cmd_install_named(argv[2]);
    }
    if (strcmp(argv[1], "verify") == 0) {
        return cmd_verify(argv[2]);
    }
    if (strcmp(argv[1], "info") == 0) {
        return cmd_info(argv[2]);
    }
    if (strcmp(argv[1], "remove") == 0) {
        return cmd_remove(argv[2]);
    }
    if (strcmp(argv[1], "search") == 0) {
        return cmd_search(argv[2]);
    }
    usage();
    return 2;
}
