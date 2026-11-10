#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#include <termios.h>
#include <errno.h>

/* Small fallback shell for images without stsh. Deliberately no quoting,
 * expansion, compound commands, or shell state bootstrap. */
#define MAX_PIPELINE 6
#define MAX_ARGV 16
#define MAX_REDIR 16
#define LINE_SIZE 512

typedef struct { int kind; char *path; } redir_t;
enum { R_IN, R_OUT, R_APPEND, R_ERR_OUT };
typedef struct {
    char *argv[MAX_ARGV+1];
    redir_t redirs[MAX_REDIR];
    int nr;
} cmd_t;

static char **g_envp;
static int g_jobcontrol;

static int
parse_command(char *seg, cmd_t *cmd)
{
    memset(cmd,0,sizeof *cmd);
    int argc=0;
    char *p=seg;
    while (*p) {
        while (*p==' ' || *p=='\t') p++;
        if (!*p || *p=='\n') break;
        if (*p=='<' || *p=='>' || !strncmp(p,"2>&1",4)) {
            if (cmd->nr==MAX_REDIR) return -1;
            redir_t *r=&cmd->redirs[cmd->nr++];
            if (!strncmp(p,"2>&1",4)) { r->kind=R_ERR_OUT; p+=4; continue; }
            r->kind=*p=='<' ? R_IN : p[1]=='>' ? R_APPEND : R_OUT;
            p+=r->kind==R_APPEND ? 2 : 1;
            while (*p==' ' || *p=='\t') p++;
            if (!*p || *p=='\n' || *p=='<' || *p=='>') return -1;
            r->path=p;
        } else {
            if (argc==MAX_ARGV) return -1;
            cmd->argv[argc++]=p;
        }
        while (*p && *p!=' ' && *p!='\t' && *p!='\n') p++;
        if (*p) *p++='\0';
    }
    return argc;
}

static int
parse_pipeline(char *line, cmd_t *cmds)
{
    int n=0;
    char *seg=line;
    for (char *p=line;;p++) {
        if (*p!='|' && *p) continue;
        int more=*p=='|';
        *p='\0';
        if (n==MAX_PIPELINE) return -1;
        int argc=parse_command(seg,&cmds[n]);
        if (argc<0 || (!argc && (more || n))) return -1;
        if (argc) n++;
        if (!more) return n;
        seg=p+1;
    }
}

static void
close_fds(int fds[3])
{
    for (int i=0;i<3;i++) if (fds[i]>=0) { close(fds[i]); fds[i]=-1; }
}

/* All authority-gated opens and ordered snapshots happen before fork. */
static int
prepare(cmd_t *cmd, int fds[3], int input, int output)
{
    fds[0]=fds[1]=fds[2]=-1;
    if (input>=0 && (fds[0]=dup(input))<0) goto fail;
    if (output>=0 && (fds[1]=dup(output))<0) goto fail;
    for (int i=0;i<cmd->nr;i++) {
        redir_t *r=&cmd->redirs[i];
        int target=r->kind==R_IN ? 0 : r->kind==R_ERR_OUT ? 2 : 1;
        int fd=r->kind==R_ERR_OUT ? dup(fds[1]>=0 ? fds[1] : 1) :
            open(r->path,r->kind==R_IN ? O_RDONLY : O_WRONLY|O_CREAT|
                 (r->kind==R_APPEND ? O_APPEND : O_TRUNC),0644);
        if (fd<0) {
            dprintf(fds[2]>=0 ? fds[2] : 2,"shell: %s: %s\n",
                    r->path ? r->path : "2>&1",strerror(errno));
            close_fds(fds); return -1;
        }
        if (fds[target]>=0) close(fds[target]);
        fds[target]=fd;
    }
    return 0;
fail:
    perror("shell: descriptors"); close_fds(fds); return -1;
}

static int
install(int fds[3])
{
    for (int i=0;i<3;i++) if (fds[i]>=0 && dup2(fds[i],i)<0) return -1;
    return 0;
}

static void
exec_cmd(char **argv)
{
    char path[256];
    if (strchr(argv[0],'/')) execve(argv[0],argv,g_envp);
    else if (snprintf(path,sizeof path,"/bin/%s",argv[0])<(int)sizeof path)
        execve(path,argv,g_envp);
    /* Return only on failure. Fork callers must exit without writing; an
     * in-process `exec` builtin still has authority to report its own error. */
}

static int
wait_child(pid_t pid)
{
    int st;
    while (waitpid(pid,&st,0)<0) {
        if (errno==EINTR) continue;
        perror("shell: waitpid"); return 1;
    }
    int rc=WIFEXITED(st) ? WEXITSTATUS(st) :
           WIFSIGNALED(st) ? 128+WTERMSIG(st) : 1;
    if (rc==125 || rc==127)
        fprintf(stderr,"shell: child setup/exec failed or command returned %d\n",rc);
    return rc;
}

static int
run_pipeline(cmd_t *cmds, int n)
{
    int pipes[MAX_PIPELINE-1][2], np=0, launched=0;
    pid_t pids[MAX_PIPELINE];
    for (;np<n-1;np++) if (pipe(pipes[np])<0) { perror("shell: pipe"); goto fail; }
    fflush(NULL);
    for (int i=0;i<n;i++) {
        int fds[3];
        if (prepare(&cmds[i],fds,i ? pipes[i-1][0] : -1,
                    i<n-1 ? pipes[i][1] : -1)<0) goto fail;
        pid_t pid=fork();
        if (!pid) {
            if (install(fds)<0) _exit(125);
            close_fds(fds);
            for (int j=0;j<np;j++) { close(pipes[j][0]); close(pipes[j][1]); }
            if (g_jobcontrol && setpgid(0,i ? pids[0] : 0)<0) _exit(125);
            if (g_jobcontrol) {
                struct sigaction sa;
                memset(&sa,0,sizeof sa); sa.sa_handler=SIG_DFL;
                if (sigaction(SIGTTOU,&sa,NULL)<0) _exit(125);
            }
            exec_cmd(cmds[i].argv);
            _exit(127);
        }
        close_fds(fds);
        if (pid<0) { perror("shell: fork"); goto fail; }
        pids[launched++]=pid;
        if (g_jobcontrol && setpgid(pid,pids[0])<0 && errno!=EACCES && errno!=ESRCH) {
            perror("shell: setpgid"); goto fail;
        }
    }
    for (int j=0;j<np;j++) { close(pipes[j][0]); close(pipes[j][1]); }
    int tty=g_jobcontrol && isatty(0);
    pid_t oldfg=tty ? tcgetpgrp(0) : -1;
    if (tty && tcsetpgrp(0,pids[0])<0) perror("shell: foreground");
    int rc=1;
    for (int i=0;i<n;i++) rc=wait_child(pids[i]);
    if (oldfg>=0 && tcsetpgrp(0,oldfg)<0) perror("shell: restore foreground");
    return rc;
fail:
    for (int j=0;j<np;j++) { close(pipes[j][0]); close(pipes[j][1]); }
    for (int j=0;j<launched;j++)
        if (kill(pids[j],SIGKILL)<0 && errno!=ESRCH) perror("shell: kill");
    for (int j=0;j<launched;j++) wait_child(pids[j]);
    return 1;
}

static int
run_line(char *line)
{
    cmd_t cmds[MAX_PIPELINE];
    int n=parse_pipeline(line,cmds);
    if (n<0) { fprintf(stderr,"shell: invalid or excessive command/pipeline\n"); return 2; }
    if (!n) return 0;
    if (n==1 && !strcmp(cmds[0].argv[0],"exec") && cmds[0].argv[1]) {
        int fds[3];
        if (prepare(&cmds[0],fds,-1,-1)<0) return 1;
        if (install(fds)<0) { perror("shell: exec descriptors"); close_fds(fds); return 1; }
        close_fds(fds);
        exec_cmd(&cmds[0].argv[1]);
        perror("shell: exec");
        return 127;
    }
    if (n==1 && !cmds[0].nr) {
        char **av=cmds[0].argv;
        if (!strcmp(av[0],"exit")) exit(av[1] ? atoi(av[1]) : 0);
        if (!strcmp(av[0],"cd")) {
            if (chdir(av[1] ? av[1] : "/")<0) { perror("cd"); return 1; }
            return 0;
        }
        if (!strcmp(av[0],"help")) { puts("Builtins: cd [path], exit [n], exec command, help"); return 0; }
    }
    return run_pipeline(cmds,n);
}

int
main(int argc, char **argv, char **envp)
{
    g_envp=envp;
    /* Reserve standard descriptors before any internal allocation. If the
     * launcher closed one, restoring /dev/null here requires parent authority. */
    for (int i=0;i<3;i++) if (fcntl(i,F_GETFD)<0) {
        if (errno!=EBADF) { perror("shell: standard descriptor"); return 1; }
        int fd=open("/dev/null",i==0 ? O_RDONLY : O_WRONLY);
        if (fd<0 || (fd!=i && dup2(fd,i)<0)) { perror("shell: /dev/null"); return 1; }
        if (fd!=i) close(fd);
    }
    char line[LINE_SIZE];
    if (argc>=3 && !strcmp(argv[1],"-c")) {
        if (strlen(argv[2])>=sizeof line) { fprintf(stderr,"shell: command too large\n"); return 2; }
        strcpy(line,argv[2]);
        return run_line(line);
    }
    g_jobcontrol=isatty(0);
    if (g_jobcontrol) {
        struct sigaction sa;
        memset(&sa,0,sizeof sa); sa.sa_handler=SIG_IGN;
        sigaction(SIGTTOU,&sa,NULL);
    }
    puts("[SHELL] LoricaOS shell ready");
    int status=0;
    for (;;) {
        if (write(1,"# ",2)<0) break;
        if (!fgets(line,sizeof line,stdin)) break;
        status=run_line(line);
    }
    return status;
}
