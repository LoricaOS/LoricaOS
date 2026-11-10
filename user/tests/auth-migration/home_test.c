#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <crypt.h>

static int step, fail_at, owner, chowns, mkdirs, non_directory, symlink_path;
static char ownership_order[2][128];
static int next(void) { return ++step == fail_at ? -1 : 0; }
static int test_mkdir(const char *p, mode_t mode)
{
    (void)p; (void)mode;
    assert(chowns == 0);
    ++mkdirs;
    return next();
}
static int test_lstat(const char *p, struct stat *st)
{
    if (next()) return -1;
    memset(st, 0, sizeof(*st));
    st->st_mode = non_directory ? S_IFREG : S_IFDIR;
    if (symlink_path && strstr(p, "Pictures")) st->st_mode = S_IFLNK;
    st->st_uid = st->st_gid = owner;
    return 0;
}
static int test_chown(const char *p, uid_t uid, gid_t gid)
{
    (void)uid; (void)gid;
    assert(mkdirs == 3 && chowns < 2);
    snprintf(ownership_order[chowns++], 128, "%s", p);
    return next();
}
static int test_chmod(const char *p, mode_t mode)
{ (void)p; (void)mode; return next(); }
static int test_open(const char *p, int flags, ...)
{ (void)p; (void)flags; return next() ? -1 : 3; }
static ssize_t test_write(int fd, const void *p, size_t n)
{ (void)fd; (void)p; return next() ? -1 : (ssize_t)n; }
static int test_close(int fd) { (void)fd; return next(); }
static char *test_crypt(const char *p, const char *s)
{ (void)p; (void)s; return NULL; }
#define mkdir test_mkdir
#define lstat test_lstat
#define chown test_chown
#define chmod test_chmod
#define open test_open
#define write test_write
#define close test_close
#define crypt test_crypt
#include "../../lib/libinstall/credentials.c"

static void reset(void)
{ step = fail_at = chowns = mkdirs = non_directory = symlink_path = 0; owner = 0; errno = EIO; }
int main(void)
{
    reset();
    assert(install_make_home("/home/alice", 1000, 1000) == 0);
    assert(chowns == 2 && strstr(ownership_order[0], "/Pictures"));
    assert(!strcmp(ownership_order[1], "/home/alice"));
    int total = step;
    for (int i = 1; i <= total; i++) {
        reset(); fail_at = i;
        assert(install_make_home("/home/alice", 1000, 1000) == -1);
    }
    reset();
    assert(install_make_home("/home/alice", 0, 0) == 0 && chowns == 0);
    reset(); non_directory = 1;
    assert(install_make_home("/home/alice", 0, 0) == -1);
    reset(); symlink_path = 1;
    assert(install_make_home("/home/alice", 0, 0) == -1);
    reset();
    assert(install_write_credentials("alice", "$6$user", "$6$admin") == 0);
    total = step;
    for (int i = 1; i <= total; i++) {
        reset(); fail_at = i;
        assert(install_write_credentials("alice", "$6$user", "$6$admin") == -1);
    }
    puts("home: tree-first, children-first ownership, exact-owner no-op, every syscall failure");
}
