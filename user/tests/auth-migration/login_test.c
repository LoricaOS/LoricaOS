#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <termios.h>
#include <unistd.h>
#include "../../lib/libauth/auth.h"

static int bind_fail, identity_fail, self_fail, active_fail, auth_fail, execs, verifies;
static int bound, switched, self_called, active_called;
static const char *input, *credential;
static int test_open(const char *path, int flags, ...)
{
    (void)flags;
    return !strcmp(path, "/etc/aegis/admin") ? 3 : -1;
}
static ssize_t test_read(int fd, void *out, size_t len)
{
    const char **p = fd == 3 ? &credential : &input;
    size_t n = strlen(*p);
    if (n > len) n = len;
    memcpy(out, *p, n); *p += n;
    return (ssize_t)n;
}
static ssize_t test_write(int fd, const void *out, size_t len)
{ (void)fd; (void)out; return (ssize_t)len; }
static int test_close(int fd) { (void)fd; return 0; }
static int test_chdir(const char *path) { (void)path; return 0; }
static unsigned test_sleep(unsigned n) { (void)n; return 0; }
static int test_tcgetattr(int fd, struct termios *t)
{ (void)fd; memset(t, 0, sizeof(*t)); return 0; }
static int test_tcsetattr(int fd, int action, const struct termios *t)
{ (void)fd; (void)action; (void)t; return 0; }
static int test_fstat(int fd, struct stat *st)
{ (void)fd; memset(st, 0, sizeof(*st)); st->st_mode = S_IFIFO; return 0; }
static long test_syscall(long number, ...)
{
    if (number == 521) {
        assert(bound && switched);
        ++self_called;
        return self_fail ? -1 : 0;
    }
    if (number == 519) { ++active_called; return active_fail ? 0 : 1; }
    /* The admin-exec path must never use old parent elevation 517. */
    assert(0);
    return -1;
}
static int test_execve(const char *target, char *const av[], char *const env[])
{
    ++execs;
    assert(!strcmp(target, "/bin/herald"));
    assert(bound && switched && self_called && active_called);
    assert(!strcmp(av[0], "/bin/herald") && !strcmp(av[1], "--admin-ready"));
    assert(!strcmp(av[2], "sync") && av[3] == NULL);
    for (int i = 0; env[i]; i++) assert(strncmp(env[i], "LD_", 3));
    return -1; /* Must terminate, not shell-fallback. */
}
int auth_verify(const char *password, const char *hash)
{ (void)hash; ++verifies; assert(!strcmp(password, "secret")); return auth_fail ? -1 : 0; }
int auth_check(const char *u, const char *p, int *uid, int *gid,
               char *h, int hn, char *s, int sn)
{
    (void)u; (void)p;
    *uid = *gid = 1000;
    snprintf(h, hn, "/home/alice"); snprintf(s, sn, "/bin/stsh");
    return auth_fail ? -1 : 0;
}
int auth_elevate_session(int uid, int gid)
{ assert(uid == gid); ++bound; return bind_fail ? -1 : 0; }
int auth_set_identity(int uid, int gid)
{ assert(uid == gid && bound); ++switched; return identity_fail ? -1 : 0; }
void auth_grant_shell_caps(void) {}
#define open test_open
#define read test_read
#define write test_write
#define close test_close
#define chdir test_chdir
#define sleep test_sleep
#define tcgetattr test_tcgetattr
#define tcsetattr test_tcsetattr
#define fstat test_fstat
#define syscall(...) test_syscall(__VA_ARGS__)
#define execve test_execve
#define main login_main
#include "../../bin/login/main.c"
#undef main

static void reset(void)
{
    bind_fail = identity_fail = self_fail = active_fail = auth_fail = 0;
    bound = switched = self_called = active_called = execs = verifies = 0;
    input = "secret\n"; credential = "$6$test$hash\n";
}
int main(void)
{
    char *av[] = {"login", "--admin-exec", "/bin/herald", "--", "sync", NULL};
    reset(); assert(login_main(5, av) == 1 && execs == 1 && verifies == 1);
    reset(); input = "secret"; /* Read EOF after a correct prefix is not success. */
    assert(login_main(5, av) == 1 && execs == 0 && verifies == 0);
    char *fdargs[] = {"login", "--admin-exec", "/bin/herald", "--password-fd",
                       "4", "--", "sync", NULL};
    reset(); assert(login_main(7, fdargs) == 1 && execs == 1 && verifies == 1);
    fdargs[4] = "not-a-fd";
    reset(); assert(login_main(7, fdargs) == 1 && execs == 0 && verifies == 0);
    for (int failure = 0; failure < 5; failure++) {
        reset();
        if (failure == 0) auth_fail = 1;
        if (failure == 1) bind_fail = 1;
        if (failure == 2) identity_fail = 1;
        if (failure == 3) self_fail = 1;
        if (failure == 4) active_fail = 1;
        assert(login_main(5, av) == 1 && execs == 0);
    }
    const char *denied[] = {"/bin/stsh", "/bin/sh", "/tmp/herald", "herald",
                            "/bin/../bin/herald", "/bin/passwd", "/bin/gui-installer"};
    for (unsigned i = 0; i < sizeof(denied)/sizeof(denied[0]); i++) {
        reset(); av[2] = (char *)denied[i];
        assert(login_main(5, av) == 1 && execs == 0 && verifies == 0);
    }
    char *text[] = {"login", NULL};
    reset(); bind_fail = 1; input = "alice\npassword\n";
    assert(login_main(1, text) == 1 && bound == 1 && !switched && !execs);
    reset(); identity_fail = 1; input = "alice\npassword\n";
    assert(login_main(1, text) == 1 && bound == 1 && switched == 1 && !execs);
    puts("login: text bind/switch fail closed; admin verify/bind/switch/SELF/query failures; no fallback or arbitrary targets");
}
