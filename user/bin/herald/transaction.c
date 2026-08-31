#include "transaction.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define TXN_BACKUP  HERALD_TXN_DIR "/backup"
#define TXN_STATE   HERALD_TXN_DIR "/state"
#define TXN_JOURNAL HERALD_TXN_DIR "/journal"
#define TXN_DELETE  HERALD_TXN_DIR "/delete"
#define TXN_PATH_MAX 768

static int mkdir_p(const char *path)
{
    char tmp[TXN_PATH_MAX];
    size_t i;
    if (!path || strlen(path) >= sizeof(tmp)) return -1;
    strcpy(tmp, path);
    for (i = 1; tmp[i]; i++) {
        if (tmp[i] != '/') continue;
        tmp[i] = '\0';
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
        tmp[i] = '/';
    }
    return mkdir(tmp, 0755) == 0 || errno == EEXIST ? 0 : -1;
}

static int parent_dirs(const char *path)
{
    char tmp[TXN_PATH_MAX], *slash;
    if (strlen(path) >= sizeof(tmp)) return -1;
    strcpy(tmp, path);
    slash = strrchr(tmp, '/');
    if (!slash || slash == tmp) return 0;
    *slash = '\0';
    return mkdir_p(tmp);
}

static int safe_rel(const char *path)
{
    const char *p;
    if (!path || !path[0] || path[0] == '/') return 0;
    for (p = path; *p;) {
        const char *e = strchr(p, '/');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n == 0 || (n == 1 && p[0] == '.') ||
            (n == 2 && p[0] == '.' && p[1] == '.')) return 0;
        if (!e) break;
        p = e + 1;
    }
    return 1;
}

int txn_stage_path(char *out, size_t size, const char *path)
{
    if (!out || !safe_rel(path) ||
        snprintf(out, size, "%s/%s", HERALD_TXN_STAGE, path) >= (int)size)
        return -1;
    return 0;
}

static int copy_file(const char *src, const char *dst)
{
    char buf[16384];
    struct stat st;
    int in = open(src, O_RDONLY), out = -1, rc = -1;
    if (in < 0 || fstat(in, &st) != 0 || parent_dirs(dst) != 0) goto done;
    out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode & 0777);
    if (out < 0) goto done;
    for (;;) {
        ssize_t n = read(in, buf, sizeof(buf)), off = 0;
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) goto done;
        if (n == 0) break;
        while (off < n) {
            ssize_t w = write(out, buf + off, (size_t)(n - off));
            if (w < 0 && errno == EINTR) continue;
            if (w <= 0) goto done;
            off += w;
        }
    }
    if (fsync(out) != 0) goto done;
    rc = 0;
done:
    if (out >= 0 && close(out) != 0) rc = -1;
    if (in >= 0) close(in);
    if (rc != 0) unlink(dst);
    return rc;
}

static int remove_tree(const char *path)
{
    DIR *d = opendir(path);
    struct dirent *de;
    if (!d) return errno == ENOENT ? 0 : -1;
    while ((de = readdir(d)) != NULL) {
        char child[TXN_PATH_MAX];
        struct stat st;
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (snprintf(child, sizeof(child), "%s/%s", path, de->d_name) >=
            (int)sizeof(child) || lstat(child, &st) != 0) {
            closedir(d); return -1;
        }
        if (S_ISDIR(st.st_mode)) {
            if (remove_tree(child) != 0) { closedir(d); return -1; }
        } else if (unlink(child) != 0) { closedir(d); return -1; }
    }
    closedir(d);
    return rmdir(path) == 0 || errno == ENOENT ? 0 : -1;
}

static int write_state(const char *state)
{
    char tmp[] = TXN_STATE ".tmp";
    int fd, failed;
    if (mkdir_p(HERALD_TXN_DIR) != 0) return -1;
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    failed = write(fd, state, strlen(state)) != (ssize_t)strlen(state) ||
             write(fd, "\n", 1) != 1 || fsync(fd) != 0;
    if (close(fd) != 0) failed = 1;
    if (failed) {
        unlink(tmp);
        return -1;
    }
    if (rename(tmp, TXN_STATE) != 0) { unlink(tmp); return -1; }
    sync();
    return 0;
}

static int journal(int fd, char op, const char *path)
{
    char line[TXN_PATH_MAX + 4];
    int n = snprintf(line, sizeof(line), "%c\t%s\n", op, path);
    return n > 0 && n < (int)sizeof(line) && write(fd, line, (size_t)n) == n &&
           fsync(fd) == 0 ? 0 : -1;
}

static int prepare_target(int jfd, const char *rel)
{
    char dst[TXN_PATH_MAX], bak[TXN_PATH_MAX];
    struct stat st;
    if (!safe_rel(rel) || snprintf(dst, sizeof(dst), "/%s", rel) >= (int)sizeof(dst) ||
        snprintf(bak, sizeof(bak), "%s/%s", TXN_BACKUP, rel) >= (int)sizeof(bak))
        return -1;
    if (lstat(dst, &st) == 0) {
        if (!S_ISREG(st.st_mode) || copy_file(dst, bak) != 0 || journal(jfd, 'R', rel) != 0)
            return -1;
    } else if (errno == ENOENT) {
        if (journal(jfd, 'N', rel) != 0) return -1;
    } else return -1;
    return parent_dirs(dst);
}

static int apply_delete(int jfd, const char *rel)
{
    char dst[TXN_PATH_MAX];
    struct stat st;
    if (!safe_rel(rel) || snprintf(dst, sizeof(dst), "/%s", rel) >= (int)sizeof(dst))
        return -1;
    if (lstat(dst, &st) != 0) return errno == ENOENT ? 0 : -1;
    if (!S_ISREG(st.st_mode) || prepare_target(jfd, rel) != 0) return -1;
    return unlink(dst);
}

static int apply_stage_dir(int jfd, const char *dir, const char *relbase)
{
    DIR *d = opendir(dir);
    struct dirent *de;
    if (!d) return -1;
    while ((de = readdir(d)) != NULL) {
        char src[TXN_PATH_MAX], rel[TXN_PATH_MAX], dst[TXN_PATH_MAX];
        struct stat st;
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (snprintf(src, sizeof(src), "%s/%s", dir, de->d_name) >= (int)sizeof(src) ||
            snprintf(rel, sizeof(rel), "%s%s%s", relbase,
                     relbase[0] ? "/" : "", de->d_name) >= (int)sizeof(rel) ||
            lstat(src, &st) != 0) { closedir(d); return -1; }
        if (S_ISDIR(st.st_mode)) {
            if (apply_stage_dir(jfd, src, rel) != 0) { closedir(d); return -1; }
            continue;
        }
        if (!S_ISREG(st.st_mode) || prepare_target(jfd, rel) != 0 ||
            snprintf(dst, sizeof(dst), "/%s", rel) >= (int)sizeof(dst) ||
            rename(src, dst) != 0) { closedir(d); return -1; }
    }
    closedir(d);
    return 0;
}

static int rollback(void)
{
    FILE *f = fopen(TXN_JOURNAL, "r");
    char line[TXN_PATH_MAX + 4];
    if (!f) return errno == ENOENT ? 0 : -1;
    while (fgets(line, sizeof(line), f)) {
        char *rel = line + 2, *nl = strchr(rel, '\n');
        char dst[TXN_PATH_MAX], bak[TXN_PATH_MAX];
        if (nl) *nl = '\0';
        if ((line[0] != 'R' && line[0] != 'N') || line[1] != '\t' || !safe_rel(rel) ||
            snprintf(dst, sizeof(dst), "/%s", rel) >= (int)sizeof(dst)) {
            fclose(f); return -1;
        }
        if (line[0] == 'N') {
            if (unlink(dst) != 0 && errno != ENOENT) { fclose(f); return -1; }
        } else {
            if (snprintf(bak, sizeof(bak), "%s/%s", TXN_BACKUP, rel) >= (int)sizeof(bak) ||
                parent_dirs(dst) != 0 || copy_file(bak, dst) != 0) {
                fclose(f); return -1;
            }
        }
    }
    fclose(f);
    sync();
    return 0;
}

int txn_recover(void)
{
    FILE *f = fopen(TXN_STATE, "r");
    char state[32] = "";
    int rc = 0;
    if (!f) return errno == ENOENT ? 0 : -1;
    fgets(state, sizeof(state), f);
    fclose(f);
    if (strncmp(state, "pending-kernel", 14) == 0)
        return 1;
    if (strncmp(state, "committed", 9) != 0 && rollback() != 0) rc = -1;
    if (strncmp(state, "committed", 9) == 0 &&
        unlink("/var/lib/lorica-update/pending") != 0 && errno != ENOENT)
        rc = -1;
    if (rc == 0 && remove_tree(HERALD_TXN_DIR) != 0) rc = -1;
    return rc;
}

int txn_finalize(void)
{
    FILE *f = fopen(TXN_STATE, "r");
    char state[32] = "";
    if (!f) return -1;
    fgets(state, sizeof(state), f);
    fclose(f);
    if (strncmp(state, "pending-kernel", 14) != 0) return -1;
    if (write_state("committed") != 0) return -1;
    if (unlink("/var/lib/lorica-update/pending") != 0 && errno != ENOENT)
        return -1;
    sync();
    return remove_tree(HERALD_TXN_DIR);
}

int txn_rollback(void)
{
    if (rollback() != 0) return -1;
    return remove_tree(HERALD_TXN_DIR);
}

int txn_begin(void)
{
    int fd;
    if (txn_recover() != 0 || mkdir_p(HERALD_TXN_STAGE) != 0 ||
        mkdir_p(TXN_BACKUP) != 0 || write_state("prepared") != 0)
        return -1;
    fd = open(TXN_JOURNAL, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0 || fsync(fd) != 0 || close(fd) != 0) return -1;
    fd = open(TXN_DELETE, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0 || fsync(fd) != 0 || close(fd) != 0) return -1;
    return txn_stage_existing("var/lib/herald/db");
}

void txn_abort(void)
{
    txn_recover();
}

int txn_stage_existing(const char *path)
{
    char src[TXN_PATH_MAX], dst[TXN_PATH_MAX];
    struct stat st;
    if (!safe_rel(path) || snprintf(src, sizeof(src), "/%s", path) >= (int)sizeof(src) ||
        txn_stage_path(dst, sizeof(dst), path) != 0) return -1;
    if (lstat(src, &st) != 0) return errno == ENOENT ? parent_dirs(dst) : -1;
    return S_ISREG(st.st_mode) ? copy_file(src, dst) : -1;
}

int txn_owner_open(const char *id)
{
    char rel[160], path[TXN_PATH_MAX];
    if (!id || !id[0] || strchr(id, '/') ||
        snprintf(rel, sizeof(rel), "var/lib/herald/owners/%s.list", id) >= (int)sizeof(rel) ||
        txn_stage_path(path, sizeof(path), rel) != 0 || parent_dirs(path) != 0)
        return -1;
    return open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
}

int txn_owner_add(int fd, const char *path)
{
    size_t n;
    if (fd < 0 || !safe_rel(path) || (n = strlen(path)) >= TXN_PATH_MAX) return -1;
    return write(fd, path, n) == (ssize_t)n && write(fd, "\n", 1) == 1 ? 0 : -1;
}

static int listed(FILE *f, const char *path)
{
    char line[TXN_PATH_MAX];
    rewind(f);
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (!strcmp(line, path)) return 1;
    }
    return 0;
}

static int queue_delete(const char *path)
{
    int fd;
    size_t n;
    if (!safe_rel(path)) return -1;
    fd = open(TXN_DELETE, O_WRONLY | O_APPEND);
    if (fd < 0) return -1;
    n = strlen(path);
    if (write(fd, path, n) != (ssize_t)n || write(fd, "\n", 1) != 1 || fsync(fd) != 0) {
        close(fd); return -1;
    }
    return close(fd);
}

int txn_remove_stale(const char *id, const char *new_owner_path)
{
    char old[TXN_PATH_MAX], line[TXN_PATH_MAX];
    FILE *of, *nf;
    if (!id || strchr(id, '/') || snprintf(old, sizeof(old),
        "/var/lib/herald/owners/%s.list", id) >= (int)sizeof(old)) return -1;
    of = fopen(old, "r");
    if (!of) return errno == ENOENT ? 0 : -1;
    nf = fopen(new_owner_path, "r");
    if (!nf) { fclose(of); return -1; }
    while (fgets(line, sizeof(line), of)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] && !listed(nf, line) && queue_delete(line) != 0) {
            fclose(nf); fclose(of); return -1;
        }
    }
    fclose(nf); fclose(of);
    return 0;
}

int txn_remove_owned(const char *id)
{
    char old[TXN_PATH_MAX], line[TXN_PATH_MAX];
    FILE *f;
    if (!id || strchr(id, '/') || snprintf(old, sizeof(old),
        "/var/lib/herald/owners/%s.list", id) >= (int)sizeof(old)) return -1;
    /* ponytail: packages own files, not directories; empty directories may
     * remain after removal. Track directory ownership only if that matters. */
    f = fopen(old, "r");
    if (!f) return errno == ENOENT ? 0 : -1;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] && queue_delete(line) != 0) { fclose(f); return -1; }
    }
    fclose(f);
    if (old[0] == '/') return queue_delete(old + 1);
    return -1;
}

int txn_commit(void)
{
    FILE *df;
    char line[TXN_PATH_MAX];
    int jfd, rc = -1;
    int pending = access(HERALD_TXN_STAGE "/var/lib/lorica-update/pending", F_OK) == 0;
    if (write_state("applying") != 0) return -1;
    jfd = open(TXN_JOURNAL, O_WRONLY | O_APPEND);
    if (jfd < 0) return -1;
    df = fopen(TXN_DELETE, "r");
    if (!df) goto done;
    while (fgets(line, sizeof(line), df)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] && apply_delete(jfd, line) != 0) { fclose(df); goto done; }
    }
    fclose(df);
    if (apply_stage_dir(jfd, HERALD_TXN_STAGE, "") != 0) goto done;
    sync();
    if (write_state(pending ? "pending-kernel" : "committed") != 0) goto done;
    rc = 0;
done:
    close(jfd);
    if (rc != 0) {
        rollback();
        return -1;
    }
    /* Kernel updates retain rollback data until lorica-update verifies the ESP. */
    if (!pending) remove_tree(HERALD_TXN_DIR);
    return 0;
}
