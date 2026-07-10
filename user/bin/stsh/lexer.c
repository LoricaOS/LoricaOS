#include "stsh.h"

/*
 * lexer.c — structural tokenizer for stsh.
 *
 * Splits raw shell text into a token stream. WORD tokens carry the RAW word
 * text (quotes and $-constructs preserved verbatim); expansion happens later,
 * at execution time (run.c), so a loop body re-expands its variables on every
 * iteration. Newlines are significant tokens (statement separators / construct
 * boundaries). Reserved words (if/for/while/case/{/}/…) are just WORD tokens
 * that the parser recognizes by text.
 */

/* Copy one raw word (verbatim, quotes preserved) up to an unquoted
 * metacharacter or whitespace. */
static char *
lex_word(const char **pp, char *a, int *used, int cap)
{
    int start = *used;
    const char *p = *pp;
#define PUT(c) do { if (*used < cap - 1) a[(*used)++] = (c); } while (0)

    while (*p) {
        char c = *p;
        if (c==' '||c=='\t'||c=='\n') break;
        if (c=='|'||c=='&'||c==';'||c=='<'||c=='>'||c=='('||c==')') break;

        if (c == '\'') {                       /* single quotes: verbatim */
            PUT(c); p++;
            while (*p && *p != '\'') { PUT(*p); p++; }
            if (*p) { PUT(*p); p++; }
        } else if (c == '"') {                 /* double quotes: verbatim (with \) */
            PUT(c); p++;
            while (*p && *p != '"') {
                if (*p=='\\' && p[1]) { PUT(*p); PUT(p[1]); p += 2; continue; }
                PUT(*p); p++;
            }
            if (*p) { PUT(*p); p++; }
        } else if (c == '`') {                 /* backticks: verbatim */
            PUT(c); p++;
            while (*p && *p != '`') { PUT(*p); p++; }
            if (*p) { PUT(*p); p++; }
        } else if (c == '$' && p[1] == '(') {  /* $( ... ) balanced, verbatim */
            PUT(c); PUT('('); p += 2;
            int depth = 1;
            while (*p && depth) {
                if (*p == '(') depth++;
                else if (*p == ')') { depth--; if (!depth) { PUT(*p); p++; break; } }
                PUT(*p); p++;
            }
        } else if (c == '\\') {                /* escape: keep backslash + char */
            PUT(c); if (p[1]) { PUT(p[1]); p += 2; } else p++;
        } else {
            PUT(c); p++;
        }
    }
    PUT('\0');
#undef PUT
    *pp = p;
    return &a[start];
}

int
tokenize(const char *text, tok_t *toks, int maxtoks, char *arena, int arenalen)
{
    int nt = 0, used = 0;
    const char *p = text;

#define OP(tt, adv) do { \
        if (nt >= maxtoks - 1) return -1; \
        toks[nt].type = (tt); toks[nt].text = NULL; nt++; p += (adv); \
    } while (0)

    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        char c = *p;

        if (c == '#') { while (*p && *p != '\n') p++; continue; }  /* comment to EOL */
        if (c == '\n') { OP(T_NEWLINE, 1); continue; }
        if (c == '|')  { if (p[1]=='|') OP(T_OR,2);   else OP(T_PIPE,1);  continue; }
        if (c == '&')  { if (p[1]=='&') OP(T_AND,2);  else OP(T_AMP,1);   continue; }
        if (c == ';')  { OP(T_SEMI,1);  continue; }
        if (c == '(')  { OP(T_LPAREN,1); continue; }
        if (c == ')')  { OP(T_RPAREN,1); continue; }
        if (c == '<' && p[1] == '<') {
            /* heredoc: <<[-]DELIM \n body... \n DELIM. Emit T_DLESS then a
             * T_WORD carrying the (literal) body. <<- strips leading tabs from
             * body lines and the closing delimiter. Body is always literal —
             * like <<'EOF' (no $-expansion); that's the ponytail ceiling. */
            if (nt >= maxtoks - 1) return -1;
            toks[nt].type = T_DLESS; toks[nt].text = NULL; nt++;
            p += 2;
            int strip = 0;
            if (*p == '-') { strip = 1; p++; }
            while (*p == ' ' || *p == '\t') p++;
            char delim[64]; int dl = 0; char q = 0;
            if (*p == '\'' || *p == '"') { q = *p; p++; }
            while (*p && dl < 63) {
                if (q) { if (*p == q) { p++; break; } }
                else if (*p==' '||*p=='\t'||*p=='\n') break;
                delim[dl++] = *p++;
            }
            delim[dl] = '\0';
            while (*p && *p != '\n') p++;      /* skip rest of the opening line */
            if (*p == '\n') p++;
            int bstart = used;
            while (*p) {
                const char *ls = p;
                if (strip) while (*ls == '\t') ls++;
                const char *e = ls; while (*e && *e != '\n') e++;
                if ((int)(e - ls) == dl && !strncmp(ls, delim, dl)) {
                    p = (*e == '\n') ? e + 1 : e; break;   /* closing delimiter */
                }
                for (const char *s = ls; s < e && used < arenalen - 1; s++)
                    arena[used++] = *s;
                if (used < arenalen - 1) arena[used++] = '\n';
                p = (*e == '\n') ? e + 1 : e;
            }
            if (used < arenalen - 1) arena[used++] = '\0';
            if (nt >= maxtoks - 2) return -1;
            toks[nt].type = T_WORD; toks[nt].text = &arena[bstart]; nt++;
            /* The command ends at the opening line's newline (which we consumed
             * while slurping the body); emit it so the next line is its own
             * statement, not merged into this command's argv. */
            toks[nt].type = T_NEWLINE; toks[nt].text = NULL; nt++;
            continue;
        }
        if (c == '<')  { OP(T_LT,1); continue; }
        if (c == '>')  { if (p[1]=='>') OP(T_GTGT,2); else if (p[1]=='&') OP(T_GTAMP,2); else OP(T_GT,1); continue; }
        if (c == '2' && p[1] == '>') {
            if (p[2]=='&' && p[3]=='1') OP(T_DGTAMP,4);
            else                        OP(T_DGT,2);
            continue;
        }
        /* word */
        if (nt >= maxtoks - 1) return -1;
        toks[nt].type = T_WORD;
        toks[nt].text = lex_word(&p, arena, &used, arenalen);
        nt++;
        if (used >= arenalen - 1) return -1;
    }
#undef OP
    toks[nt].type = T_EOF;
    toks[nt].text = NULL;
    return nt;
}
