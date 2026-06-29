/* sshd — minimal TCP superserver front-end for tinysshd.
 *
 * TinySSH's tinysshd does not listen()/accept() itself; it speaks the SSH
 * protocol on stdin/stdout and is meant to be run per-connection by a TCP
 * superserver (tcpserver, tcpsvd, inetd, systemd socket activation). This is
 * that front-end: listen on :22, accept, fork, hand the connected socket to
 * tinysshd as fd 0/1, and let tinysshd do the SSH handshake + session.
 *
 * Modelled on user/bin/httpd. Children auto-reap via SIG_IGN on SIGCHLD.
 */
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>

extern char **environ;

static void log_str(const char *s) { write(2, s, strlen(s)); }

#define SSHKEYDIR "/etc/tinyssh/sshkeydir"

/* Bound the number of concurrent connection handlers. Each tinysshd does a
 * 256KB-stack post-quantum KEX, so an unbounded fork-per-connection accept loop
 * is a cheap remote memory-exhaustion / fork-storm amplifier. */
#define MAX_CHILDREN 16
static volatile sig_atomic_t g_children = 0;

/* Reap finished handlers and free their slot. WNOHANG loop drains all that
 * exited since the last signal. */
static void sigchld_handler(int sig)
{
    (void)sig;
    int st;
    while (waitpid(-1, &st, WNOHANG) > 0) {
        if (g_children > 0) g_children--;
    }
}

int main(void)
{
    int srv, cli, yes = 1;
    struct sockaddr_in addr;

    /* Tighten umask before generating host keys so tinysshd-makekey writes the
     * ed25519 SECRET host key restrictively (not world-readable). */
    umask(077);

    /* Ensure ed25519 host keys exist (first boot generates them). Done before
     * the SIGCHLD handler is installed so this waitpid() is unambiguous.
     * tinysshd-makekey is idempotent enough for our purposes; failure (keys
     * present) is harmless. */
    {
        pid_t kp = fork();
        if (kp == 0) {
            char *kargv[] = { "tinysshd-makekey", SSHKEYDIR, NULL };
            execve("/bin/tinysshd-makekey", kargv, environ);
            _exit(127);
        }
        if (kp > 0) { int st; waitpid(kp, &st, 0); }
    }

    /* Reap connection handlers via a real SIGCHLD handler (not SIG_IGN) so we
     * can keep an accurate live-child count for the concurrency cap. SA_RESTART
     * keeps accept() from spuriously returning EINTR. */
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = sigchld_handler;
        sa.sa_flags   = SA_RESTART | SA_NOCLDSTOP;
        sigaction(SIGCHLD, &sa, NULL);
    }

    srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { log_str("[SSHD] socket() failed\n"); return 1; }
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

    memset(&addr, 0, sizeof addr);
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(22);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(srv, (struct sockaddr *)&addr, sizeof addr) < 0) {
        log_str("[SSHD] bind :22 failed\n");
        return 1;
    }
    if (listen(srv, 8) < 0) { log_str("[SSHD] listen() failed\n"); return 1; }
    log_str("[SSHD] listening on :22\n");

    for (;;) {
        cli = accept(srv, NULL, NULL);
        if (cli < 0) continue;

        /* Concurrency cap: refuse new connections once MAX_CHILDREN handlers
         * are in flight. Dropping past the cap bounds memory/PID use under a
         * connection flood; legitimate clients retry. */
        if (g_children >= MAX_CHILDREN) {
            log_str("[SSHD] at capacity, dropping connection\n");
            close(cli);
            continue;
        }

        pid_t pid = fork();
        if (pid == 0) {
            /* Child: connected socket becomes tinysshd's stdin/stdout. */
            char *argv[] = { "tinysshd", "-v", SSHKEYDIR, NULL };
            /* Restore default SIGCHLD: the parent's handler is inherited across
             * execve, and tinysshd's signing/auth subprocesses rely on
             * waitpid() — an auto-reaping disposition would make the fast
             * signing child vanish before waitpid (ECHILD → kexdh failure). */
            signal(SIGCHLD, SIG_DFL);
            close(srv);
            if (cli != 0) dup2(cli, 0);
            if (cli != 1) dup2(cli, 1);
            if (cli > 1)  close(cli);
            execve("/bin/tinysshd", argv, environ);
            log_str("[SSHD] exec /bin/tinysshd failed\n");
            _exit(127);
        }
        if (pid > 0) g_children++;
        close(cli);
    }
    return 0;
}
