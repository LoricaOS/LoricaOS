/* Exercise the real launch function with strict pre-exec syscall mocks. */
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <signal.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#include <stdio.h>
#include <assert.h>
#include <setjmp.h>

static int mock_open(const char *, int, ...);
static int mock_close(int);
static int mock_dup2(int, int);
static pid_t mock_fork(void);
static int mock_execv(const char *, char *const []);
static ssize_t mock_write(int, const void *, size_t);
static _Noreturn void mock_exit(int);

#define open mock_open
#define close mock_close
#define dup2 mock_dup2
#define fork mock_fork
#define execv mock_execv
#define write mock_write
#define _exit mock_exit
#define main vigil_program_main
#include "../user/bin/vigil/main.c"
#undef main
#undef open
#undef close
#undef dup2
#undef fork
#undef execv
#undef write
#undef _exit

static jmp_buf done;
static int in_child, open_fd, fork_result, dup_error, close_error;
static int opens, forks, dups, closes, execs, exit_status;
static char events[32], exec_path[256];
static char error_line[256];
static service_t svc;

static void event(char c)
{
    size_t n = strlen(events);
    assert(n + 1 < sizeof(events));
    events[n] = c;
    events[n + 1] = 0;
}

static int mock_open(const char *path, int flags, ...)
{
    assert(!in_child); /* A strict child must never open, even for diagnostics. */
    if (strcmp(path, "/dev/null")) return -1; /* Don't create real log files. */
    assert(flags == O_WRONLY); /* fd 1 itself must survive exec. */
    event('o');
    opens++;
    errno = ENOCAP;
    return open_fd;
}

static int mock_close(int fd)
{
    assert(fd == open_fd);
    event('c');
    closes++;
    errno = EIO; /* Cleanup must not replace the original fork errno. */
    return close_error ? -1 : 0;
}

static int mock_dup2(int old, int new)
{
    assert(in_child && old == open_fd && new == 1);
    event('d');
    dups++;
    return dup_error ? -1 : new;
}

static pid_t mock_fork(void)
{
    event('f');
    forks++;
    in_child = fork_result == 0;
    errno = EAGAIN;
    return fork_result;
}

static int mock_execv(const char *path, char *const argv[])
{
    assert(in_child);
    assert(dups == (open_fd >= 0 && opens != 0));
    assert(argv[0] && strcmp(argv[0], path) == 0);
    event('e');
    execs++;
    strcpy(exec_path, path);
    /* Return failure: the child must _exit without write/open/stdio repair. */
    errno = ENOENT;
    return -1;
}

static ssize_t mock_write(int fd, const void *buf, size_t len)
{
    assert(!in_child);
    if (fd == 2 && len && len < sizeof(error_line) &&
        ((const char *)buf)[len - 1] == '\n') {
        memcpy(error_line, buf, len);
        error_line[len] = 0;
    }
    return (ssize_t)len;
}

static _Noreturn void mock_exit(int status)
{
    exit_status = status;
    longjmp(done, 1);
}

static void reset(const char *cmd, int quiet, int fd, int pid)
{
    memset(&svc, 0, sizeof(svc));
    strcpy(svc.name, "test");
    strcpy(svc.run_cmd, cmd);
    svc.pid = -1;
    s_quiet = quiet;
    in_child = dup_error = close_error = 0;
    opens = forks = dups = closes = execs = exit_status = 0;
    events[0] = exec_path[0] = 0;
    error_line[0] = 0;
    open_fd = fd;
    fork_result = pid;
}

static void launch(void)
{
    if (!setjmp(done)) start_service(&svc);
}

int main(void)
{
    reset("/bin/service", 1, 7, 42);
    launch();
    assert(!strcmp(events, "ofc") && svc.pid == 42);

    reset("/bin/service", 1, 7, -1);
    launch();
    assert(!strcmp(events, "ofc") && svc.pid == -1);
    char expected[64];
    snprintf(expected, sizeof(expected), "fork failed (errno=%d)", EAGAIN);
    assert(strstr(error_line, expected));

    reset("/bin/service", 1, -1, 42);
    launch();
    assert(!strcmp(events, "o") && forks == 0 && svc.pid == -1);

    for (int fd = 0; fd <= 7; fd++) {
        reset("/bin/service", 1, fd, 42);
        launch();
        assert(!strcmp(events, "ofc") && svc.pid == 42);

        reset("/bin/service", 1, fd, 0);
        launch();
        assert(!strcmp(events, fd == 1 ? "ofde" : "ofdce"));
        assert(execs == 1 && exit_status == 127);
        assert(!strcmp(exec_path, "/bin/service"));
    }

    reset("/bin/service", 1, 7, 0);
    dup_error = 1;
    launch();
    assert(!strcmp(events, "ofd") && !execs && exit_status == 127);

    reset("/bin/service", 1, 7, 0);
    close_error = 1;
    launch();
    assert(!strcmp(events, "ofdc") && !execs && exit_status == 127);

    const char *interactive[] = { "/bin/login", "/bin/bastion" };
    for (unsigned i = 0; i < sizeof(interactive)/sizeof(interactive[0]); i++) {
        reset(interactive[i], 1, -1, 0);
        launch();
        assert(!strcmp(events, "fe") && !opens && !dups);
    }
    reset("/bin/service", 0, -1, 0);
    launch();
    assert(!strcmp(events, "fe") && !opens);

    reset("echo service", 1, 7, 0);
    launch();
    assert(!strcmp(events, "ofdce") && !strcmp(exec_path, "/bin/sh"));
    puts("vigil launch: PASS (strict child, fd 0/1/2, cleanup, failures)");
    return 0;
}
