#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static int active, query_count = 64, uid, gid, execs;
static uint32_t granted = (1u << 4) | (1u << 6) | (1u << 11) | (1u << 17);
static uint32_t rights = 3;
static int malformed;
static const char *expected_target = "/bin/herald", *expected_arg = "sync";
static long test_syscall(long n, ...)
{
    if (n == 519) return active;
    assert(n == 362);
    va_list ap; va_start(ap, n);
    assert(va_arg(ap, long) == 0);
    struct { uint32_t kind, rights; } *caps = va_arg(ap, void *);
    memset(caps, 0, va_arg(ap, size_t));
    va_end(ap);
    int out = 0;
    for (int i = 1; i < 32; i++)
        if (granted & (1u << i)) {
            caps[out].kind = i; caps[out++].rights = rights;
        }
    if (malformed) caps[63].kind = 100;
    return query_count;
}
static uid_t test_getuid(void) { return uid; }
static gid_t test_getgid(void) { return gid; }
static int test_execve(const char *path, char *const av[], char *const env[])
{
    (void)env;
    ++execs;
    assert(!strcmp(path, "/bin/login"));
    assert(!strcmp(av[1], "--admin-exec"));
    assert(!strcmp(av[2], expected_target));
    int i = 3;
    if (!strcmp(av[i], "--password-fd")) {
        char buf[128] = {0};
        assert(read(atoi(av[i + 1]), buf, sizeof(buf)) == 7);
        assert(!strcmp(buf, "secret\n"));
        i += 2;
    }
    assert(!strcmp(av[i++], "--"));
    if (expected_arg) assert(!strcmp(av[i++], expected_arg));
    assert(av[i] == NULL);
    return -1;
}
#define syscall(...) test_syscall(__VA_ARGS__)
#define getuid test_getuid
#define getgid test_getgid
#define execve test_execve
#include "../../lib/libinstall/install_elevate.c"

int main(void)
{
    char *args[] = {"herald", "sync", NULL}, **av = args;
    int ac = 2;
    active = 1;
    assert(install_admin_ready("/bin/herald") == 1);
    assert(install_admin_ready(ADMIN_GUI_INSTALLER_TARGET) == 1);
    assert(install_admin_ready("/bin/gui-installer") == -1);
    assert(install_admin_ready("/tmp/herald") == -1);
    assert(install_admin_ready("/bin/stsh") == -1);
    uid = 1000;
    assert(install_admin_ready("/bin/herald") == 0);
    uid = 0; gid = 1000;
    assert(install_admin_ready("/bin/herald") == 0);
    gid = 0; active = 0;
    assert(install_admin_ready("/bin/herald") == 0);
    assert(install_admin_enter("/bin/herald", &ac, &av, "secret") == -1 && execs == 1);
    active = -1;
    assert(install_admin_enter("/bin/herald", &ac, &av, NULL) == -1 && execs == 1);
    active = 1; query_count = 63;
    assert(install_admin_ready("/bin/herald") == -1);
    query_count = -1;
    assert(install_admin_ready("/bin/herald") == -1);
    query_count = 64; rights = 1;
    assert(install_admin_ready("/bin/herald") == 0);
    rights = 3; granted &= ~(1u << 17);
    assert(install_admin_ready("/bin/herald") == 0);
    granted |= 1u << 17;
    malformed = 1;
    assert(install_admin_ready("/bin/herald") == -1);
    malformed = 0; active = 2;
    assert(install_admin_ready("/bin/herald") == -1);
    char *marked[] = {"herald", "--admin-ready", "sync", NULL};
    av = marked; ac = 3; active = 0;
    assert(install_admin_enter("/bin/herald", &ac, &av, NULL) == -1 && execs == 1);
    active = 1;
    assert(install_admin_enter("/bin/herald", &ac, &av, NULL) == 0);
    assert(ac == 2 && !strcmp(av[1], "sync"));
    expected_target = "/bin/adminpw"; expected_arg = NULL;
    char *pwargs[] = {"adminpw", NULL};
    av = pwargs; ac = 1;
    assert(install_admin_enter("/bin/adminpw", &ac, &av, NULL) == -1 && execs == 2);
    puts("entry: active session + identity + live rights, forged marker denial, private-FD reexec");
}
