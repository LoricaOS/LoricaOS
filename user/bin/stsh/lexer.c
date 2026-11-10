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
    struct { char delim[64]; int strip, token; } pending[MAX_REDIR];
    int npending=0;

#define OP(tt, adv) do { \
        if (nt >= maxtoks - 1) return -1; \
        toks[nt].type = (tt); toks[nt].text = NULL; nt++; p += (adv); \
    } while (0)

    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        char c = *p;

        if (c == '#') { while (*p && *p != '\n') p++; continue; }  /* comment to EOL */
        if (c == '\n') {
            /* Finish tokenizing the ENTIRE opening line before consuming bodies.
             * Otherwise `<<END >out`, pipelines, '&', and later redirects
             * disappear. Each placeholder retains its lexical redirect order. */
            p++;
            for (int h=0;h<npending;h++) {
                int start=used, found=0;
                while (*p) {
                    const char *line=p;
                    if (pending[h].strip) while (*line=='\t') line++;
                    const char *end=line;
                    while (*end && *end!='\n') end++;
                    size_t len=end-line;
                    if (len==strlen(pending[h].delim) &&
                        !memcmp(line,pending[h].delim,len)) {
                        p=*end ? end+1 : end; found=1; break;
                    }
                    size_t add=len+(*end=='\n');
                    if (add>=(size_t)(arenalen-used)) return -1;
                    memcpy(arena+used,line,len); used+=len;
                    if (*end=='\n') arena[used++]='\n';
                    p=*end ? end+1 : end;
                }
                if (!found || used>=arenalen) return -1;
                arena[used++]='\0';
                toks[pending[h].token].text=arena+start;
            }
            npending=0;
            if (nt>=maxtoks-1) return -1;
            toks[nt++]=(tok_t){T_NEWLINE,NULL};
            continue;
        }
        if (c == '|')  { if (p[1]=='|') OP(T_OR,2);   else OP(T_PIPE,1);  continue; }
        if (c == '&')  { if (p[1]=='&') OP(T_AND,2);  else OP(T_AMP,1);   continue; }
        if (c == ';')  { OP(T_SEMI,1);  continue; }
        if (c == '(')  { OP(T_LPAREN,1); continue; }
        if (c == ')')  { OP(T_RPAREN,1); continue; }
        if (c == '<' && p[1] == '<') {
            /* Defer body consumption until the opening newline. Bodies remain
             * literal (no expansion); <<- strips leading tabs as before. */
            if (nt>=maxtoks-2 || npending==MAX_REDIR) return -1;
            toks[nt].type = T_DLESS; toks[nt].text = NULL; nt++;
            p += 2;
            int strip = 0;
            if (*p == '-') { strip = 1; p++; }
            while (*p == ' ' || *p == '\t') p++;
            int dl=0; char q=0;
            if (*p == '\'' || *p == '"') { q = *p; p++; }
            while (*p) {
                if (q) { if (*p == q) { p++; break; } }
                else if (*p==' '||*p=='\t'||*p=='\n'||strchr("|&;<>()",*p)) break;
                if (*p=='\n' || dl==63) return -1;
                pending[npending].delim[dl++]=*p++;
            }
            if (!dl) return -1;
            pending[npending].delim[dl]='\0';
            pending[npending].strip=strip;
            pending[npending].token=nt;
            npending++;
            toks[nt++]=(tok_t){T_WORD,NULL};
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
    if (npending) return -1;
    toks[nt].type = T_EOF;
    toks[nt].text = NULL;
    return nt;
}
