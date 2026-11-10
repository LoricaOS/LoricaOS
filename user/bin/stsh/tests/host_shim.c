/* Host-only authority simulation. Each successful exec starts with a fresh
 * image; old fork images are denied open/read/write/fopen. Real Aegis policy
 * and TTY/session enforcement still require VM validation. */
#include "../stsh.h"
#include <stdarg.h>

static int capless, forks, pipes;

pid_t test_fork(void)
{
    const char *fail=getenv("STSH_TEST_FORK_FAIL");
    if (fail && ++forks==atoi(fail)) { errno=EAGAIN; return -1; }
    pid_t p=fork();
    if (!p) capless=1;
    return p;
}

int test_pipe(int fd[2])
{
    const char *fail=getenv("STSH_TEST_PIPE_FAIL");
    if (fail && ++pipes==atoi(fail)) { errno=EMFILE; return -1; }
    return pipe(fd);
}

int test_open(const char *path, int flags, ...)
{
    if (capless) _exit(124);
    mode_t mode=0;
    if (flags&O_CREAT) {
        va_list ap; va_start(ap,flags); mode=va_arg(ap,int); va_end(ap);
    }
    return open(path,flags,mode);
}

ssize_t test_write(int fd, const void *buf, size_t n)
{
    if (capless) _exit(124);
    static int interrupted;
    if (getenv("STSH_TEST_WRITE_EINTR") && !interrupted++) { errno=EINTR; return -1; }
    if (getenv("STSH_TEST_WRITE_ZERO")) return 0;
    if (getenv("STSH_TEST_SHORT_WRITE") && n>2) n=2;
    return write(fd,buf,n);
}

ssize_t test_read(int fd, void *buf, size_t n)
{
    if (capless) _exit(124);
    return read(fd,buf,n);
}

FILE *test_fopen(const char *path, const char *mode)
{
    if (capless) _exit(124);
    return fopen(path,mode);
}

int test_dup2(int oldfd, int newfd)
{
    if (capless && getenv("STSH_TEST_DUP2_FAIL")) { errno=EBADF; return -1; }
    return dup2(oldfd,newfd);
}

int test_setpgid(pid_t pid, pid_t group)
{
    if (capless && getenv("STSH_TEST_SETPGID_FAIL")) { errno=EPERM; return -1; }
    return setpgid(pid,group);
}

void caps_init(void) {}
int has_cap_delegate(void) { return 0; }
int caps_builtin(int n, char **v) { (void)n;(void)v; if(capless)_exit(124); puts("fresh-caps"); return 0; }
int grant_builtin(int n, char **v) { (void)n;(void)v; return 1; }
int sandbox_builtin(int n, char **v, char **e) { (void)n;(void)v;(void)e; return 1; }
int admin_builtin(int n, char **v) { (void)n;(void)v; return 1; }
int deadmin_builtin(void) { return 0; }
void hist_init(int n) { (void)n; }
void hist_add(const char *s) { (void)s; }
void hist_save(void) {}
int editor_readline(const char *p, char *buf, int n)
{
    (void)p;
    if (capless) _exit(124);
    /* Aegis keeps waitable children even with SIGCHLD ignored. Host kernels
     * auto-reap them, so use their default ignored delivery instead. A real
     * canonical PTY supplies ISIG/Ctrl-C for job-control regression tests. */
    struct sigaction sa;
    memset(&sa,0,sizeof sa); sa.sa_handler=SIG_DFL;
    sigaction(SIGCHLD,&sa,NULL);
    puts("[HOST] prompt"); fflush(stdout);
    if (!fgets(buf,n,stdin)) return -1;
    size_t len=strlen(buf);
    if (len && buf[len-1]=='\n') buf[--len]='\0';
    return len;
}
