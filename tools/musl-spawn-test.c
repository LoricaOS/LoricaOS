/* Host model for the exact patched musl source, not a replacement spawn.
 * clone runs the callback while the parent is suspended. exec detaches or
 * dies post-PONR without returning; _exit publishes a pre-exec failure.
 * The real kernel's scheduling/VM guarantees still require a VM boot test. */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static int test_clone(int (*)(void *), void *, int, void *);
static int test_execve(const char *, char *const [], char *const []);
static _Noreturn void test_exit(int);
static pid_t test_waitpid(pid_t, int *, int);
static int test_cancel(int, int *);
static int test_sigmask(int, const sigset_t *, sigset_t *);
static long test_syscall(int, ...);
static int test_open(const char *, int, mode_t);
static void test_lock(volatile int *);
static void test_unlock(volatile int *);
static void test_handlers(sigset_t *);
static int test_sigaction(int, const struct sigaction *, struct sigaction *);

#define execve test_execve
#define _exit test_exit
#define waitpid test_waitpid
#define pthread_setcancelstate test_cancel
#define pthread_sigmask test_sigmask
#include "posix_spawn.c"

static jmp_buf child_done;
static int clone_error, exec_error, target_status, waits, reaps, zombies;
static int locked, cancel_state, interrupted, execs, child_status, calls;
static int valid_fd[1024], fd_flags[1024];
static sigset_t current_mask, initial_mask, test_all_signals;
static volatile int __abort_lock[1];

static int test_clone(int (*fn)(void *), void *stack, int flags, void *arg)
{
    (void)stack;
    assert(flags == (CLONE_VM|CLONE_VFORK|SIGCHLD));
    assert(locked && cancel_state == PTHREAD_CANCEL_DISABLE);
    assert(((struct args *)arg)->err == 0);
    if (clone_error) return -clone_error;
    if (!setjmp(child_done)) {
        fn(arg);
        assert(!"child callback returned");
    }
    zombies = 1;
    return 42;
}

static int test_execve(const char *path, char *const argv[], char *const envp[])
{
    (void)path;
    (void)argv;
    (void)envp;
    execs++;
    if (exec_error) {
        errno = exec_error;
        return -1;
    }
    /* A valid target can exit 127, or die after exec's point of no return.
     * Neither case returns into the callback to report an error. */
    child_status = target_status;
    longjmp(child_done, 1);
}

static _Noreturn void test_exit(int status)
{
    assert(status == 127);
    child_status = status << 8;
    longjmp(child_done, 1);
}

static pid_t test_waitpid(pid_t pid, int *status, int flags)
{
    assert(pid == 42 && flags == 0 && !locked);
    assert(cancel_state == PTHREAD_CANCEL_DISABLE);
    waits++;
    if (interrupted) {
        interrupted = 0;
        errno = EINTR;
        return -1;
    }
    assert(zombies == 1);
    zombies = 0;
    reaps++;
    *status = child_status;
    return pid;
}

static int test_cancel(int state, int *old)
{
    if (old) *old = cancel_state;
    cancel_state = state;
    return 0;
}

static int test_sigmask(int how, const sigset_t *set, sigset_t *old)
{
    if (old) *old = current_mask;
    if (how == SIG_BLOCK) {
        assert(set == SIGALL_SET);
        current_mask = *set;
    } else {
        assert(how == SIG_SETMASK);
        current_mask = *set;
    }
    return 0;
}

static long test_syscall(int nr, ...)
{
    va_list ap;
    va_start(ap, nr);
    long r = 0;
    calls++;
    switch (nr) {
    case SYS_dup2: {
        int old = va_arg(ap, int), new = va_arg(ap, int);
        assert(old >= 0 && old < 1024 && new >= 0 && new < 1024);
        if (!valid_fd[old]) r = -EBADF;
        else { valid_fd[new] = 1; fd_flags[new] = 0; }
        break;
    }
    case SYS_close:
        valid_fd[va_arg(ap, int)] = 0;
        break;
    case SYS_fcntl: {
        int fd = va_arg(ap, int), cmd = va_arg(ap, int);
        if (!valid_fd[fd]) r = -EBADF;
        else if (cmd == F_GETFD) r = fd_flags[fd];
        else {
            assert(cmd == F_SETFD);
            fd_flags[fd] = va_arg(ap, int);
        }
        break;
    }
    case SYS_setsid: case SYS_setpgid: case SYS_setgid: case SYS_setuid:
    case SYS_getgid: case SYS_getuid: case SYS_chdir: case SYS_fchdir:
        break;
    default:
        assert(!"unexpected syscall (pipe/read/write are forbidden)");
    }
    va_end(ap);
    return r;
}

static int test_open(const char *path, int flags, mode_t mode)
{
    (void)path;
    (void)flags;
    (void)mode;
    return -EPERM; /* ENOCAP: independent children have no open authority. */
}

static void test_lock(volatile int *lock)
{
    assert(lock == __abort_lock && !locked);
    locked = 1;
}

static void test_unlock(volatile int *lock)
{
    assert(lock == __abort_lock && locked);
    locked = 0;
}

static void test_handlers(sigset_t *set) { sigemptyset(set); }
static int test_sigaction(int sig, const struct sigaction *sa, struct sigaction *old)
{
    (void)sig;
    (void)old;
    assert(sa && (sa->sa_handler == SIG_DFL || sa->sa_handler == SIG_IGN));
    return 0;
}

static void reset(void)
{
    clone_error = exec_error = waits = reaps = zombies = locked = 0;
    cancel_state = PTHREAD_CANCEL_ENABLE;
    interrupted = execs = calls = 0;
    target_status = 0;
    memset(valid_fd, 0, sizeof(valid_fd));
    memset(fd_flags, 0, sizeof(fd_flags));
    valid_fd[0] = valid_fd[1] = valid_fd[2] = valid_fd[4] = 1;
    sigemptyset(&initial_mask);
    sigaddset(&initial_mask, SIGUSR1);
    current_mask = initial_mask;
    sigfillset(&test_all_signals);
}

static void check(const posix_spawn_file_actions_t *fa, int error)
{
    pid_t pid = -99;
    char *argv[] = { (char *)"target", 0 };
    assert(posix_spawn(&pid, "target", fa, 0, argv, argv) == error);
    assert(pid == (error ? -99 : 42));
    assert(!locked && cancel_state == PTHREAD_CANCEL_ENABLE);
    for (int i = 1; i < NSIG; i++)
        assert(sigismember(&current_mask, i) == sigismember(&initial_mask, i));
    assert(zombies == (!error));
    if (!error) {
        int st;
        test_cancel(PTHREAD_CANCEL_DISABLE, 0);
        assert(test_waitpid(pid, &st, 0) == pid && st == target_status);
    }
    assert(!zombies);
}

int main(void)
{
    reset();
    check(0, 0);
    assert(execs == 1 && reaps == 1);

    reset();
    clone_error = EAGAIN;
    check(0, EAGAIN);
    assert(!waits && !execs);

    reset();
    exec_error = ENOENT;
    interrupted = 1;
    check(0, ENOENT);
    assert(waits == 2 && reaps == 1);

    reset();
    struct fdop bad = { .cmd = FDOP_DUP2, .srcfd = 999, .fd = 1 };
    posix_spawn_file_actions_t fa = { .__actions = &bad };
    check(&fa, EBADF);
    assert(!execs && reaps == 1);

    reset();
    struct fdop opening = { .cmd = FDOP_OPEN, .fd = 1 };
    fa.__actions = &opening;
    check(&fa, EPERM);
    assert(!execs && reaps == 1);

    reset();
    /* Fds formerly used by the pipe are ordinary caller fds. Registration
     * order is tail -> prev: dup 4->5, close 4, dup 5->1, same-fd dup 1->1. */
    struct fdop same = { .cmd = FDOP_DUP2, .srcfd = 1, .fd = 1 };
    struct fdop last = { .cmd = FDOP_DUP2, .srcfd = 5, .fd = 1, .prev = &same };
    struct fdop closing = { .cmd = FDOP_CLOSE, .fd = 4, .prev = &last };
    struct fdop first = { .cmd = FDOP_DUP2, .srcfd = 4, .fd = 5, .prev = &closing };
    same.next = &last;
    last.next = &closing;
    closing.next = &first;
    fa.__actions = &same;
    check(&fa, 0);
    assert(!valid_fd[4] && valid_fd[5] && valid_fd[1] && calls == 5);

    reset();
    target_status = 127 << 8;
    check(0, 0);
    assert(reaps == 1);

    reset();
    target_status = SIGKILL; /* Post-PONR death is still a spawned child. */
    check(0, 0);
    assert(reaps == 1);

    puts("musl Aegis spawn: PASS (errors, exit127, PONR, actions, reaping, state)");
    return 0;
}
