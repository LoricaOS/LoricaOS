#include "stsh.h"

/*
 * run.c — top-level execution: expand+tokenize a line, group tokens into
 * pipelines joined by ; && || &, honor short-circuit, apply VAR=val
 * assignments, and dispatch builtins. Also the script-file runner and the
 * $(...) capture primitive.
 */

int         g_last_exit = 0;
const char *g_arg0      = "stsh";
char       *g_params[MAX_PARAMS];
int         g_nparams   = 0;

void
run_set_params(const char *arg0, char **params, int nparams)
{
    if (arg0) g_arg0 = arg0;
    if (nparams > MAX_PARAMS) nparams = MAX_PARAMS;
    g_nparams = nparams;
    for (int i = 0; i < nparams; i++) g_params[i] = params[i];
}

/* is `w` a NAME=VALUE assignment word? */
static int
is_assignment(const char *w)
{
    if (!w || !(*w == '_' || (*w>='a'&&*w<='z') || (*w>='A'&&*w<='Z')))
        return 0;
    const char *p = w + 1;
    while (*p == '_' || (*p>='a'&&*p<='z') || (*p>='A'&&*p<='Z') || (*p>='0'&&*p<='9'))
        p++;
    return *p == '=';
}

/* Strip leading NAME=VAL assignments from a command, applying them to the
 * environment, and compact argv. ponytail: prefix assignments (VAR=x cmd) go
 * to the global env, not a per-command scope — good enough for build scripts;
 * true scoping lands with the AST in Layer 2. */
static void
apply_assignments(cmd_t *c)
{
    int k = 0;
    while (k < c->argc && is_assignment(c->argv[k])) {
        char *eq = strchr(c->argv[k], '=');
        *eq = '\0';
        env_set(c->argv[k], eq + 1);
        *eq = '=';
        k++;
    }
    if (k == 0) return;
    int j = 0;
    for (int i = k; i <= c->argc; i++) c->argv[j++] = c->argv[i]; /* incl NULL */
    c->argc -= k;
}

/* Builtins that must run in the shell process and aren't in exec.c's set. */
static int
local_builtin(cmd_t *c)
{
    char **argv = c->argv;
    if (!argv[0]) return 0;

    if (strcmp(argv[0], ":") == 0 || strcmp(argv[0], "true") == 0) {
        g_last_exit = 0; return 1;
    }
    if (strcmp(argv[0], "false") == 0) {
        g_last_exit = 1; return 1;
    }
    if (strcmp(argv[0], "unset") == 0) {
        for (int i = 1; argv[i]; i++) env_set(argv[i], "");  /* v1: blank, not delete */
        g_last_exit = 0; return 1;
    }
    if (strcmp(argv[0], ".") == 0 || strcmp(argv[0], "source") == 0) {
        if (!argv[1]) { fprintf(stderr, "source: filename required\n"); g_last_exit = 1; return 1; }
        run_script(argv[1], &argv[2], c->argc - 2 > 0 ? c->argc - 2 : 0);
        return 1;
    }
    if (strcmp(argv[0], "exec") == 0 && argv[1]) {
        char full[256];
        if (argv[1][0] != '/') {
            snprintf(full, sizeof full, "/bin/%s", argv[1]);
            execve(full, &argv[1], env_as_array());
        } else {
            execve(argv[1], &argv[1], env_as_array());
        }
        fprintf(stderr, "exec: %s: %s\n", argv[1], strerror(errno));
        g_last_exit = 127; return 1;
    }
    return 0;
}

/* Gather one pipeline (tokens up to a separator) into cmds[]. Advances *pi past
 * the separator. Returns the separator that ended it (T_SEMI at end-of-line). */
static toktype_t
build_pipeline(tok_t *toks, int nt, int *pi, cmd_t *cmds, int maxc, int *pncmds)
{
    int i = *pi, nc = 0;
    cmd_t *cur = NULL;
    toktype_t sep = T_SEMI;

    while (i < nt) {
        toktype_t tt = toks[i].type;
        if (tt==T_SEMI||tt==T_AMP||tt==T_AND||tt==T_OR) { sep = tt; i++; break; }

        if (tt == T_PIPE) {
            if (cur) { cur->argv[cur->argc] = NULL; }
            cur = NULL; i++; continue;
        }
        if (!cur) {
            if (nc >= maxc) { fprintf(stderr, "stsh: pipeline too long\n"); break; }
            cur = &cmds[nc++];
            memset(cur, 0, sizeof *cur);
        }
        switch (tt) {
        case T_WORD:
            if (cur->argc < MAX_ARGV) cur->argv[cur->argc++] = toks[i].text;
            i++; break;
        case T_LT:
            if (toks[i+1].type==T_WORD) { cur->stdin_file = toks[i+1].text; i += 2; } else i++;
            break;
        case T_GT:
            if (toks[i+1].type==T_WORD) { cur->stdout_file = toks[i+1].text; cur->stdout_append = 0; i += 2; } else i++;
            break;
        case T_GTGT:
            if (toks[i+1].type==T_WORD) { cur->stdout_file = toks[i+1].text; cur->stdout_append = 1; i += 2; } else i++;
            break;
        case T_2GT:
            if (toks[i+1].type==T_WORD) { cur->stderr_file = toks[i+1].text; i += 2; } else i++;
            break;
        case T_2GT1:
            cur->stderr_to_stdout = 1; i++; break;
        default:
            i++; break;
        }
    }
    if (cur) cur->argv[cur->argc] = NULL;
    *pi = i;
    *pncmds = nc;
    return sep;
}

/*
 * scan_segment — copy one pipeline segment from *pp into seg[], stopping at a
 * top-level (unquoted) ; && || & separator (or end / #comment). Quotes,
 * backslashes, backticks and $(...) are copied verbatim WITHOUT expansion so
 * the tokenizer can expand them later — this is what makes ordering correct
 * (e.g. `X=hi; echo $X` expands $X only after X is assigned). Advances *pp past
 * the separator; sets *sep to it (T_SEMI at end of line).
 */
static int
scan_segment(const char **pp, char *seg, int segcap, toktype_t *sep)
{
    const char *p = *pp;
    int n = 0;
    *sep = T_SEMI;

#define PUT(ch) do { if (n < segcap - 1) seg[n++] = (ch); } while (0)

    while (*p == ' ' || *p == '\t') p++;

    while (*p) {
        char c = *p;
        int at_boundary = (n == 0 || seg[n-1]==' ' || seg[n-1]=='\t');

        if (c == '#' && at_boundary) { while (*p) p++; break; }  /* comment */
        if (c == '\'') {
            PUT(c); p++;
            while (*p && *p != '\'') { PUT(*p); p++; }
            if (*p) { PUT(*p); p++; }
            continue;
        }
        if (c == '"') {
            PUT(c); p++;
            while (*p && *p != '"') {
                if (*p == '\\' && p[1]) { PUT(*p); PUT(p[1]); p += 2; continue; }
                PUT(*p); p++;
            }
            if (*p) { PUT(*p); p++; }
            continue;
        }
        if (c == '`') {
            PUT(c); p++;
            while (*p && *p != '`') { PUT(*p); p++; }
            if (*p) { PUT(*p); p++; }
            continue;
        }
        if (c == '\\') { PUT(c); if (p[1]) { PUT(p[1]); p += 2; } else p++; continue; }
        if (c == '$' && p[1] == '(') {       /* copy balanced $( ... ) */
            PUT(c); PUT('('); p += 2;
            int depth = 1;
            while (*p && depth) {
                if (*p == '(') depth++;
                else if (*p == ')') depth--;
                if (depth == 0) { PUT(*p); p++; break; }
                PUT(*p); p++;
            }
            continue;
        }
        /* top-level separators */
        if (c == ';') { *sep = T_SEMI; p++; break; }
        if (c == '&') { if (p[1]=='&') { *sep=T_AND; p+=2; } else { *sep=T_AMP; p++; } break; }
        if (c == '|' && p[1]=='|') { *sep = T_OR; p += 2; break; }

        PUT(c); p++;   /* ordinary char, incl. a single '|' pipe */
    }
#undef PUT
    seg[n] = '\0';
    *pp = p;
    return n;
}

int
run_line(const char *line)
{
    const char *p = line;
    toktype_t conn = T_SEMI;   /* connector preceding the current segment */

    while (*p) {
        char      seg[LINE_SIZE];
        toktype_t sep;
        int sl = scan_segment(&p, seg, sizeof seg, &sep);
        int bg = (sep == T_AMP);

        int skip = (conn==T_AND && g_last_exit != 0) ||
                   (conn==T_OR  && g_last_exit == 0);

        if (sl > 0 && !skip) {
            tok_t toks[MAX_TOKENS];
            char  arena[LEX_ARENA];
            int nt = tokenize(seg, toks, MAX_TOKENS, arena, sizeof arena);
            if (nt < 0) { fprintf(stderr, "stsh: line too complex\n"); g_last_exit = 2; }
            else if (nt > 0) {
                cmd_t cmds[MAX_PIPELINE];
                int ncmds = 0, i = 0;
                build_pipeline(toks, nt, &i, cmds, MAX_PIPELINE, &ncmds);

                for (int c = 0; c < ncmds; c++) apply_assignments(&cmds[c]);
                int pure_assign = (ncmds == 1 && cmds[0].argv[0] == NULL);
                if (pure_assign) g_last_exit = 0;  /* a bare assignment succeeds */
                if (ncmds > 0 && !pure_assign) {
                    if (ncmds == 1 && local_builtin(&cmds[0]))
                        ;
                    else if (!try_builtin(cmds, ncmds, &g_last_exit)) {
                        if (bg) run_pipeline_bg(cmds, ncmds, env_as_array());
                        else    run_pipeline(cmds, ncmds, env_as_array(), &g_last_exit);
                    }
                }
            }
        }
        conn = sep;
    }
    return g_last_exit;
}

/* sh_capture — run `cmd`, append its stdout (trailing \n stripped) to arena. */
void
sh_capture(const char *cmd, char *a, int *used, int cap)
{
    int pfd[2];
    if (pipe(pfd) < 0) return;

    pid_t pid = fork();
    if (pid < 0) { close(pfd[0]); close(pfd[1]); return; }
    if (pid == 0) {
        dup2(pfd[1], STDOUT_FILENO);
        close(pfd[0]); close(pfd[1]);
        char buf[LINE_SIZE];
        strncpy(buf, cmd, sizeof buf - 1); buf[sizeof buf - 1] = '\0';
        run_line(buf);
        _exit(g_last_exit);
    }
    close(pfd[1]);
    int start = *used;
    char rb[512];
    ssize_t r;
    while ((r = read(pfd[0], rb, sizeof rb)) > 0)
        for (ssize_t k = 0; k < r; k++)
            if (*used < cap - 1) a[(*used)++] = rb[k];
    close(pfd[0]);
    waitpid(pid, NULL, 0);
    while (*used > start && a[*used - 1] == '\n') (*used)--;  /* strip trailing newlines */
}

int
run_script(const char *path, char **argv_from, int argc_from)
{
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "stsh: %s: %s\n", path, strerror(errno)); return (g_last_exit = 127); }

    /* Set positional params for the script body ($0 = path, $1.. = args). */
    const char *saved0 = g_arg0;
    char *saved_params[MAX_PARAMS]; int saved_n = g_nparams;
    for (int i = 0; i < g_nparams; i++) saved_params[i] = g_params[i];
    run_set_params(path, argv_from, argc_from);

    char line[LINE_SIZE];
    char joined[LINE_SIZE];
    while (fgets(line, sizeof line, f)) {
        /* Trailing-backslash line continuation. */
        int len = (int)strlen(line);
        while (len && (line[len-1]=='\n' || line[len-1]=='\r')) line[--len] = '\0';
        strncpy(joined, line, sizeof joined - 1); joined[sizeof joined - 1] = '\0';
        int jl = (int)strlen(joined);
        while (jl && joined[jl-1] == '\\') {
            joined[--jl] = '\0';
            if (!fgets(line, sizeof line, f)) break;
            int l2 = (int)strlen(line);
            while (l2 && (line[l2-1]=='\n'||line[l2-1]=='\r')) line[--l2] = '\0';
            strncat(joined, line, sizeof joined - jl - 1);
            jl = (int)strlen(joined);
        }
        run_line(joined);
    }
    fclose(f);

    run_set_params(saved0, saved_params, saved_n);
    return g_last_exit;
}
