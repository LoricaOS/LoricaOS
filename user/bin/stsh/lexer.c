#include "stsh.h"

/*
 * lexer.c — quote-aware tokenizer + word expansion for stsh.
 *
 * Replaces the old flat, quote-blind parser. Produces a token stream where
 * WORD tokens are already fully expanded and unquoted; the parser (run.c)
 * only groups tokens into pipelines and and-or lists.
 *
 * Handled here: single quotes (literal), double quotes (expand, keep spaces),
 * backslash escapes, $VAR / ${VAR} / $? / $# / $@ / $* / $$ / $0..$9,
 * command substitution $(...) and `...`, operators (| || & && ; < > >> 2> 2>&1),
 * and # comments.
 *
 * ponytail: NO field-splitting of unquoted expansions and NO globbing yet —
 * a var holding spaces stays one word; `*.c` is literal. Both land in Layer 2
 * (control-flow pass), where the recursive parser + glob() belong.
 */

/* positional-parameter state lives in run.c */
extern const char *g_arg0;
extern char       *g_params[];
extern int         g_nparams;

/* ── arena append helpers ── */

static void
emit(char *a, int *used, int cap, char c)
{
    if (*used < cap - 1)
        a[(*used)++] = c;
}

static void
emit_str(char *a, int *used, int cap, const char *s)
{
    while (s && *s)
        emit(a, used, cap, *s++);
}

static int is_name_start(char c){ return c == '_' || (c>='a'&&c<='z') || (c>='A'&&c<='Z'); }
static int is_name(char c){ return is_name_start(c) || (c>='0'&&c<='9'); }

/*
 * expand_dollar — handle one $-expansion starting at *pp (which points at '$').
 * Appends the result to the arena and advances *pp past the construct.
 */
static void
expand_dollar(const char **pp, char *a, int *used, int cap)
{
    const char *p = *pp + 1;   /* skip '$' */
    char c = *p;
    char tmp[32];

    if (c == '?') {
        snprintf(tmp, sizeof tmp, "%d", g_last_exit);
        emit_str(a, used, cap, tmp); p++;
    } else if (c == '$') {
        snprintf(tmp, sizeof tmp, "%d", (int)getpid());
        emit_str(a, used, cap, tmp); p++;
    } else if (c == '#') {
        snprintf(tmp, sizeof tmp, "%d", g_nparams);
        emit_str(a, used, cap, tmp); p++;
    } else if (c == '@' || c == '*') {
        for (int i = 0; i < g_nparams; i++) {
            if (i) emit(a, used, cap, ' ');
            emit_str(a, used, cap, g_params[i]);
        }
        p++;
    } else if (c >= '0' && c <= '9') {
        int idx = c - '0';
        const char *v = (idx == 0) ? (g_arg0 ? g_arg0 : "")
                      : (idx <= g_nparams ? g_params[idx - 1] : "");
        emit_str(a, used, cap, v); p++;
    } else if (c == '(') {
        /* $(...) command substitution — find matching ) honoring nesting */
        p++;
        const char *start = p;
        int depth = 1;
        while (*p && depth) {
            if (*p == '(') depth++;
            else if (*p == ')') { if (--depth == 0) break; }
            p++;
        }
        int len = (int)(p - start);
        char cmd[LINE_SIZE];
        if (len > (int)sizeof(cmd) - 1) len = sizeof(cmd) - 1;
        memcpy(cmd, start, len); cmd[len] = '\0';
        if (*p == ')') p++;
        sh_capture(cmd, a, used, cap);
    } else if (c == '{') {
        p++;
        char name[128]; int n = 0;
        while (*p && *p != '}' && n < (int)sizeof(name) - 1) name[n++] = *p++;
        name[n] = '\0';
        if (*p == '}') p++;
        emit_str(a, used, cap, env_get(name));
    } else if (is_name_start(c)) {
        char name[128]; int n = 0;
        while (is_name(*p) && n < (int)sizeof(name) - 1) name[n++] = *p++;
        name[n] = '\0';
        emit_str(a, used, cap, env_get(name));
    } else {
        emit(a, used, cap, '$');   /* bare $ */
    }
    *pp = p;
}

/* backtick command substitution: `...` (no nesting, POSIX-simple) */
static void
expand_backtick(const char **pp, char *a, int *used, int cap)
{
    const char *p = *pp + 1;   /* skip ` */
    const char *start = p;
    while (*p && *p != '`') p++;
    int len = (int)(p - start);
    char cmd[LINE_SIZE];
    if (len > (int)sizeof(cmd) - 1) len = sizeof(cmd) - 1;
    memcpy(cmd, start, len); cmd[len] = '\0';
    if (*p == '`') p++;
    sh_capture(cmd, a, used, cap);
    *pp = p;
}

/* Word-terminating (unquoted) metacharacters. */
static int
is_meta(char c)
{
    return c==' '||c=='\t'||c=='\n'||c=='|'||c=='&'||c==';'||c=='<'||c=='>';
}

/*
 * lex_word — read one word starting at *pp into the arena, fully expanded and
 * unquoted. Returns the arena offset of the word's first char (NUL-terminated),
 * advances *pp past the word.
 */
static char *
lex_word(const char **pp, char *a, int *used, int cap)
{
    int start = *used;
    const char *p = *pp;

    while (*p && !is_meta(*p)) {
        char c = *p;
        if (c == '\'') {                       /* single quote: literal */
            p++;
            while (*p && *p != '\'') emit(a, used, cap, *p++);
            if (*p == '\'') p++;
        } else if (c == '"') {                 /* double quote: expand, keep spaces */
            p++;
            while (*p && *p != '"') {
                if (*p == '$') expand_dollar(&p, a, used, cap);
                else if (*p == '`') expand_backtick(&p, a, used, cap);
                else if (*p == '\\' && (p[1]=='"'||p[1]=='\\'||p[1]=='$'||p[1]=='`')) {
                    emit(a, used, cap, p[1]); p += 2;
                } else emit(a, used, cap, *p++);
            }
            if (*p == '"') p++;
        } else if (c == '\\') {                /* backslash escape */
            if (p[1]) { emit(a, used, cap, p[1]); p += 2; }
            else p++;
        } else if (c == '$') {
            expand_dollar(&p, a, used, cap);
        } else if (c == '`') {
            expand_backtick(&p, a, used, cap);
        } else {
            emit(a, used, cap, c); p++;
        }
    }
    emit(a, used, cap, '\0');
    *pp = p;
    return &a[start];
}

int
tokenize(const char *line, tok_t *toks, int maxtoks, char *arena, int arenalen)
{
    int nt = 0, used = 0;
    const char *p = line;

#define PUSHOP(tt, adv) do { \
        if (nt >= maxtoks - 1) return -1; \
        toks[nt].type = (tt); toks[nt].text = NULL; nt++; p += (adv); \
    } while (0)

    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '\n' || *p == '#') break;   /* # starts a comment */

        char c = *p;
        if (c == '|')      { if (p[1]=='|') PUSHOP(T_OR,2);  else PUSHOP(T_PIPE,1); }
        else if (c == '&') { if (p[1]=='&') PUSHOP(T_AND,2); else PUSHOP(T_AMP,1); }
        else if (c == ';') { PUSHOP(T_SEMI,1); }
        else if (c == '<') { PUSHOP(T_LT,1); }
        else if (c == '>') { if (p[1]=='>') PUSHOP(T_GTGT,2); else PUSHOP(T_GT,1); }
        else if (c == '2' && p[1] == '>') {
            if (p[2]=='&' && p[3]=='1') PUSHOP(T_2GT1,4);
            else                        PUSHOP(T_2GT,2);
        } else {
            if (nt >= maxtoks - 1) return -1;
            toks[nt].type = T_WORD;
            toks[nt].text = lex_word(&p, arena, &used, arenalen);
            nt++;
            if (used >= arenalen - 1) return -1;
        }
    }
#undef PUSHOP
    toks[nt].type = T_EOF;
    toks[nt].text = NULL;
    return nt;
}
