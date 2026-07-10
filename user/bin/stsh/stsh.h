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

#define MAX_PIPELINE    8     /* max pipeline stages */
#define MAX_ARGV        64    /* max args per command */
#define LINE_SIZE       4096  /* input line buffer (configure scripts are wide) */
#define ENV_MAX         64    /* max environment variables */
#define HIST_SIZE       64    /* history ring buffer entries */
#define CAP_TABLE_SIZE  16    /* max capability slots to query */
#define MAX_TOKENS      256   /* max lexer tokens per line */
#define LEX_ARENA       8192  /* expanded-word scratch arena per line */
#define MAX_PARAMS      64    /* positional parameters $1..$n */

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
    int   argc;
    char *stdin_file;         /* path for < redirect, NULL if none */
    char *stdout_file;        /* path for > redirect, NULL if none */
    int   stdout_append;      /* 1 if >> (append), 0 if > (truncate) */
    char *stderr_file;        /* path for 2> redirect, NULL if none */
    int   stderr_to_stdout;   /* 1 if 2>&1 was specified */
    int   stdout_dup_to;      /* target fd for >&N (0 = none) */
} cmd_t;

/* ── lexer.c ── */

typedef enum {
    T_WORD, T_PIPE, T_AND, T_OR, T_SEMI, T_AMP,
    T_LT, T_GT, T_GTGT, T_DGT, T_DGTAMP, T_GTAMP, T_DLESS,
    T_LPAREN, T_RPAREN, T_NEWLINE, T_EOF
} toktype_t;

typedef struct {
    toktype_t type;
    char     *text;   /* WORD tokens: RAW (unexpanded, still-quoted) word text */
} tok_t;

/* Structural tokenizer: splits raw text into tokens WITHOUT expanding words
 * (expansion is deferred to execution time in run.c so loop bodies re-expand
 * each pass). Returns token count (T_EOF appended), or -1 on overflow. */
int tokenize(const char *text, tok_t *toks, int maxtoks, char *arena, int arenalen);

/* Run `cmd` and append its stdout (trailing newlines stripped) to a buffer —
 * the $(...) / backtick capture primitive. Defined in run.c. */
void sh_capture(const char *cmd, char *buf, int *used, int cap);

/* ── run.c ── */

extern int g_last_exit;

void run_set_params(const char *arg0, char **params, int nparams);
int  run_program(const char *text);              /* lex+parse+exec a whole program */
int  run_line(const char *line);                 /* alias for run_program */
int  run_script(const char *path, char **argv_from, int argc_from);
int  sh_incomplete(const char *buf);             /* interactive: needs more input? */

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
