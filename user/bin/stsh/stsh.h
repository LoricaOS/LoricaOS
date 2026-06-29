#ifndef STSH_H
#define STSH_H

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <termios.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>

/* ── Constants ── */

#define MAX_PIPELINE    6     /* max pipeline stages */
#define MAX_ARGV        16    /* max args per command */
#define LINE_SIZE       512   /* input line buffer */
#define ENV_MAX         64    /* max environment variables */
#define HIST_SIZE       64    /* history ring buffer entries */
#define CAP_TABLE_SIZE  16    /* max capability slots to query */

/* ── Syscall numbers ── */

#define SYS_CAP_QUERY   362
#define SYS_SPAWN       514
#define SYS_SETFG       360

/* ── Types ── */

typedef struct {
    unsigned int kind;
    unsigned int rights;
} cap_slot_t;

typedef struct {
    char *argv[MAX_ARGV + 1]; /* NULL-terminated */
    char *stdin_file;         /* path for < redirect, NULL if none */
    char *stdout_file;        /* path for > redirect, NULL if none */
    int   stdout_append;      /* 1 if >> (append), 0 if > (truncate) */
    int   stderr_to_stdout;   /* 1 if 2>&1 was specified */
} cmd_t;

/* ── parser.c ── */

int parse_command(char *seg, cmd_t *cmd);
int parse_pipeline(char *line, cmd_t *cmds, int max);
int parse_pipeline_bg(char *line, cmd_t *cmds, int max, int *bg_out);

/* ── env.c ── */

void        env_init(char **envp);
const char *env_get(const char *key);
void        env_set(const char *key, const char *value);
void        env_print_all(void);
char      **env_as_array(void);
void        env_expand(const char *src, char *dst, int dstlen, int last_exit);

/* ── editor.c ── */

int editor_readline(const char *prompt, char *buf, int buflen);

/* ── history.c ── */

void        hist_init(int privileged);
void        hist_add(const char *line);
const char *hist_prev(void);
const char *hist_next(void);
void        hist_save(void);
void        hist_reset_cursor(void);

/* ── complete.c ── */

void complete(char *buf, int *pos, int *len, const char *prompt);

/* ── caps.c ── */

void caps_init(void);
int  has_cap_delegate(void);
int  caps_builtin(int argc, char **argv);
int  grant_builtin(int argc, char **argv);
int  sandbox_builtin(int argc, char **argv, char **envp);
int  admin_builtin(int argc, char **argv);
int  deadmin_builtin(void);
int  stsh_admin_active(void);   /* 1 if this shell session is elevated */

/* ── exec.c ── */

void run_pipeline(cmd_t *cmds, int n, char **envp, int *last_exit);
void run_pipeline_bg(cmd_t *cmds, int n, char **envp);
int  try_builtin(cmd_t *cmds, int n, int *last_exit);

#endif /* STSH_H */
