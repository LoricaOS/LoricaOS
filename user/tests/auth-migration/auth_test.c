#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <crypt.h>

static const char *passwd_line = "alice:x:1000:1000:Alice:/home/alice:/bin/stsh\n";
static const char *shadow_line = "alice:abJnggxhB/yWI:0::::::\n";
static int calls, fail_at;
static long ids[4], numbers[4];
static long test_syscall(long number, ...)
{
    va_list ap;
    va_start(ap, number);
    numbers[calls] = number;
    ids[calls] = va_arg(ap, long);
    va_end(ap);
    return ++calls == fail_at ? -1 : 0;
}
static FILE *test_fopen(const char *path, const char *mode)
{
    (void)mode;
    const char *text = !strcmp(path, "/etc/passwd") ? passwd_line : shadow_line;
    if (!text) return NULL;
    FILE *f = tmpfile();
    assert(f);
    fputs(text, f);
    rewind(f);
    return f;
}
#define syscall(...) test_syscall(__VA_ARGS__)
#define fopen test_fopen
#include "../../lib/libauth/auth.c"

int main(void)
{
    calls = 0; fail_at = 1;
    assert(auth_elevate_session(1000, 1000) == -1 && calls == 1);
    calls = 0; fail_at = 0;
    assert(auth_elevate_session(1000, 1000) == 0 && numbers[0] == 364);
    calls = 0; fail_at = 1;
    assert(auth_set_identity(1000, 1001) == -1 && calls == 1);
    calls = 0; fail_at = 2;
    assert(auth_set_identity(1000, 1001) == -1 && calls == 2);
    assert(numbers[0] == 106 && ids[0] == 1001);
    assert(numbers[1] == 105 && ids[1] == 1000);
    calls = 0; fail_at = 0;
    assert(auth_set_identity(1000, 1001) == 0);
    assert(auth_set_identity(-1, 0) == -1);
    assert(auth_elevate_session(-1, 0) == -1);
    assert(auth_verify("password", "abJnggxhB/yWI") == 0);
    assert(auth_verify("wrong", "abJnggxhB/yWI") == -1);
    assert(auth_verify("anything", "*0") == -1);
    assert(auth_verify("anything", "") == -1);
    int uid, gid; char home[128], shell[128];
    assert(auth_check("alice", "password", &uid, &gid, home, sizeof(home),
                      shell, sizeof(shell)) == 0);
    assert(uid == 1000 && gid == 1000);
    shadow_line = NULL;
    assert(auth_check("alice", "password", &uid, &gid, home, sizeof(home),
                      shell, sizeof(shell)) == -1);
    shadow_line = "alice:!:0::::::\n";
    assert(auth_check("alice", "password", &uid, &gid, home, sizeof(home),
                      shell, sizeof(shell)) == -1);
    passwd_line = "alice:x:nonnumeric:0:Alice:/home/alice:/bin/stsh\n";
    assert(auth_lookup_passwd("alice", &uid, &gid, home, sizeof(home),
                              shell, sizeof(shell)) == -1);
    puts("auth: checked kernel failures, real crypt mismatch/locked credential, malformed identity");
}
