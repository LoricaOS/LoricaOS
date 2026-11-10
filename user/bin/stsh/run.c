#include "stsh.h"
#include <glob.h>
#include <fnmatch.h>
#include <ctype.h>

/*
 * run.c — expansion + a tree-walking parser/executor for stsh.
 *
 * Layer 2: control flow (if/for/while/until/case), functions, brace groups,
 * subshells, pathname globbing, field-splitting of unquoted expansions.
 *
 * Design: the whole program is lexed once into a token array, then parsed and
 * executed in a single pass over the tokens. Loops re-execute their body by
 * saving the body's token index and re-running from it; not-taken branches and
 * short-circuited commands are "run" with exec disabled (run=0), which parses/
 * advances without side effects. Simple commands reuse cmd_t + exec.c's
 * run_pipeline(); expansion is done per-word at execution time.
 *
 * ponytail limits (Layer 3 if a real build needs them): heredoc bodies are
 * literal (like <<'EOF' — no $-expansion) and capped at the pipe buffer; a
 * compound command can't be a stage of a multi-stage pipeline; prefix
 * assignments (VAR=x cmd) and pipeline-stage assignments apply to the global
 * env, not a scoped one. Noted where they bite.
 */

/* ── positional parameters + control-flow state ── */

int         g_last_exit = 0;
int         g_jobcontrol = 0;
int         g_tty_fd = -1;
const char *g_arg0      = "stsh";
char       *g_params[MAX_PARAMS];
int         g_nparams   = 0;
static unsigned s_capture_serial;
static char *s_owned_arg0;

enum { FLOW_NONE = 0, FLOW_BREAK, FLOW_CONTINUE, FLOW_RETURN };
static int g_flow  = FLOW_NONE;
static int g_flow_n = 0;   /* break/continue level count */

/* current token stream (set by run_program) */
static tok_t *T;
static int    NT;

/* ── shell functions ──
 * A function owns a DEEP COPY of its body tokens (+ strdup'd word text) so it
 * survives the transient program buffer it was defined in — callable later,
 * across prompts or from a different file. */

#define MAX_FUNCS 64
struct func { char name[64]; tok_t *toks; int nt; };
static struct func s_funcs[MAX_FUNCS];
static int s_nfuncs;

static struct func *
func_find(const char *name)
{
    for (int i = 0; i < s_nfuncs; i++)
        if (strcmp(s_funcs[i].name, name) == 0) return &s_funcs[i];
    return NULL;
}

void
run_set_params(const char *arg0, char **params, int nparams)
{
    if (nparams > MAX_PARAMS) nparams = MAX_PARAMS;
    if (nparams < 0) nparams=0;
    char *new0=strdup(arg0 ? arg0 : g_arg0);
    char *next[MAX_PARAMS]={0};
    if (!new0) goto oom;
    for (int i=0;i<nparams;i++) if (!(next[i]=strdup(params[i]))) goto oom;
    free(s_owned_arg0); s_owned_arg0=new0; g_arg0=new0;
    for (int i=0;i<g_nparams;i++) free(g_params[i]);
    g_nparams = nparams;
    for (int i = 0; i < nparams; i++) g_params[i] = next[i];
    return;
oom:
    /* Continuing with a partially changed parameter vector would silently
     * corrupt the state we promise to hand off. */
    fprintf(stderr,"stsh: cannot allocate positional parameters\n");
    exit(2);
}

/* Versioned, NUL-free argv transport. Records are decimal length ':' raw bytes,
 * never executable shell source. This preserves quoted words and literal
 * heredocs exactly and creates no filesystem object or inherited authority.
 * All input is validated in a dry pass before allocating/applying any state.
 * The kernel still applies the ordinary /bin/stsh policy and caller ceiling. */
struct state_writer { char *buf; size_t used; int bad; };

static void
state_num(struct state_writer *w, unsigned n)
{
    char s[24];
    int len=snprintf(s,sizeof s,"%u:",n);
    if (w->bad || w->used+(size_t)len>=STSH_STATE_MAX) { w->bad=1; return; }
    memcpy(w->buf+w->used,s,len); w->used+=len;
}

static void
state_string(struct state_writer *w, const char *s)
{
    size_t n=strlen(s);
    if (n>=STSH_STATE_MAX) { w->bad=1; return; }
    state_num(w,(unsigned)n);
    if (w->bad || w->used+n>=STSH_STATE_MAX) { w->bad=1; return; }
    memcpy(w->buf+w->used,s,n); w->used+=n;
}

static void
state_tokens(struct state_writer *w, const tok_t *tokens, int nt)
{
    if (nt<0 || nt>=MAX_TOKENS) { w->bad=1; return; }
    state_num(w,nt);
    for (int i=0;i<nt;i++) {
        state_num(w,tokens[i].type);
        state_string(w,tokens[i].text ? tokens[i].text : "");
    }
}

static char *
state_make(const tok_t *tokens, int nt)
{
    char *buf=malloc(STSH_STATE_MAX);
    if (!buf) { perror("stsh: interpreter state"); return NULL; }
    struct state_writer w={buf,0,0};
    state_string(&w,"STSH1");
    state_num(&w,(unsigned)g_last_exit & 255);
    char **env=env_as_array(); int ne=0;
    while (env[ne]) ne++;
    state_num(&w,ne);
    for (int i=0;i<ne;i++) state_string(&w,env[i]);
    state_string(&w,g_arg0);
    state_num(&w,g_nparams);
    for (int i=0;i<g_nparams;i++) state_string(&w,g_params[i]);
    state_num(&w,s_nfuncs);
    for (int i=0;i<s_nfuncs;i++) {
        state_string(&w,s_funcs[i].name);
        state_tokens(&w,s_funcs[i].toks,s_funcs[i].nt);
    }
    state_tokens(&w,tokens,nt);
    if (w.bad) {
        fprintf(stderr,"stsh: interpreter state exceeds %d bytes\n",STSH_STATE_MAX-1);
        free(buf); return NULL;
    }
    buf[w.used]='\0';
    return buf;
}

struct state_reader { const char *p, *end; int bad, apply; };

static unsigned
state_read_num(struct state_reader *r, unsigned max)
{
    unsigned n=0;
    if (r->bad || r->p==r->end || !isdigit((unsigned char)*r->p)) { r->bad=1; return 0; }
    while (r->p<r->end && isdigit((unsigned char)*r->p)) {
        unsigned d=*r->p++-'0';
        if (d>max || n>(max-d)/10) { r->bad=1; return 0; }
        n=n*10+d;
    }
    if (r->p==r->end || *r->p++!=':') r->bad=1;
    return n;
}

static char *
state_read_string(struct state_reader *r, unsigned max, int require_equal)
{
    unsigned n=state_read_num(r,max);
    if (r->bad || (size_t)(r->end-r->p)<n) { r->bad=1; return NULL; }
    const char *p=r->p; r->p+=n;
    if (require_equal && (!n || !memchr(p,'=',n) || *p=='=')) r->bad=1;
    if (!r->apply || r->bad) return NULL;
    char *s=malloc(n+1);
    if (!s) { r->bad=1; return NULL; }
    memcpy(s,p,n); s[n]='\0'; return s;
}

static tok_t *
state_read_tokens(struct state_reader *r, int *nt)
{
    *nt=state_read_num(r,MAX_TOKENS-1);
    tok_t *tokens=r->apply && !r->bad ? calloc(*nt+1,sizeof(tok_t)) : NULL;
    if (r->apply && !tokens) r->bad=1;
    for (int i=0;i<*nt && !r->bad;i++) {
        unsigned type=state_read_num(r,T_EOF-1);
        char *text=state_read_string(r,type==T_WORD ? STSH_STATE_MAX-1 : 0,0);
        if (tokens) { tokens[i].type=type; tokens[i].text=text; }
    }
    if (tokens) tokens[*nt].type=T_EOF;
    return tokens;
}

static int run_list(int *pi, int run, int in_loop);

int
run_bootstrap(const char *payload)
{
    size_t size=strnlen(payload,STSH_STATE_MAX);
    if (size==STSH_STATE_MAX) goto invalid;
    /* Pass one proves the entire schema and total consumption; pass two can
     * then allocate only bounded counts/strings. Nothing executes before EOF. */
    for (int apply=0;apply<2;apply++) {
        struct state_reader r={payload,payload+size,0,apply};
        unsigned magiclen=state_read_num(&r,5);
        if (r.bad || magiclen!=5 || r.end-r.p<5 || memcmp(r.p,"STSH1",5)) goto invalid;
        r.p+=5;
        int status=state_read_num(&r,255);
        unsigned ne=state_read_num(&r,ENV_MAX);
        char *env[ENV_MAX+1]={0};
        for (unsigned i=0;i<ne && !r.bad;i++) env[i]=state_read_string(&r,255,1);
        char *arg0=state_read_string(&r,STSH_STATE_MAX-1,0);
        unsigned np=state_read_num(&r,MAX_PARAMS);
        char *params[MAX_PARAMS];
        for (unsigned i=0;i<np && !r.bad;i++) params[i]=state_read_string(&r,STSH_STATE_MAX-1,0);
        unsigned nf=state_read_num(&r,MAX_FUNCS);
        const char *names[MAX_FUNCS]; size_t lengths[MAX_FUNCS];
        for (unsigned i=0;i<nf && !r.bad;i++) {
            const char *record=r.p;
            char *name=state_read_string(&r,63,0); int nt;
            if (r.bad) break;
            names[i]=memchr(record,':',r.p-record);
            if (!names[i]) { r.bad=1; break; }
            names[i]++;
            lengths[i]=r.p-names[i];
            if (!lengths[i]) { r.bad=1; break; }
            for (unsigned j=0;j<i;j++)
                if (lengths[i]==lengths[j] && !memcmp(names[i],names[j],lengths[i])) r.bad=1;
            tok_t *body=state_read_tokens(&r,&nt);
            if (apply && !r.bad) {
                if (!*name || func_find(name)) { r.bad=1; break; }
                struct func *f=&s_funcs[s_nfuncs++];
                strcpy(f->name,name); free(name); f->toks=body; f->nt=nt;
            }
        }
        int nt; tok_t *tokens=state_read_tokens(&r,&nt);
        if (r.bad || r.p!=r.end) goto invalid;
        if (apply) {
            env_init(env);
            for (unsigned i=0;i<ne;i++) free(env[i]);
            run_set_params(arg0,params,np); g_last_exit=status;
            free(arg0); for (unsigned i=0;i<np;i++) free(params[i]);
            T=tokens; NT=nt;
            int i=0; run_list(&i,1,0);
            return g_last_exit;
        }
    }
invalid:
    fprintf(stderr,"stsh: invalid, unsupported, oversized, or unallocatable interpreter state\n");
    return 2;
}

/* Only descriptor/process setup, exec, and _exit below fork. No parser,
 * builtin, I/O, or capability operation in the old image. An interactive
 * top-level state job owns its foreground group; fresh nested interpreters
 * have job control disabled and stay inside that job. */
static pid_t
state_spawn(char *payload, int output, int closefd, pid_t *oldfg)
{
    *oldfg=-1;
    if (g_jobcontrol) {
        *oldfg=tcgetpgrp(g_tty_fd);
        if (*oldfg<0) { perror("stsh: state foreground"); return -1; }
    }
    fflush(NULL);
    pid_t pid=fork();
    if (pid==0) {
        if (output>=0 && output!=STDOUT_FILENO && dup2(output,STDOUT_FILENO)<0) _exit(125);
        if (closefd>=0 && closefd!=STDOUT_FILENO) close(closefd);
        if (output>=0 && output!=STDOUT_FILENO) close(output);
        if (g_jobcontrol) {
            struct sigaction sa;
            memset(&sa,0,sizeof sa); sa.sa_handler=SIG_DFL;
            if (setpgid(0,0)<0 || sigaction(SIGTTOU,&sa,NULL)<0) _exit(125);
        }
        if (g_tty_fd>=0) close(g_tty_fd);
        char *av[]={STSH_PATH,"--state-v1",payload,NULL};
        execve(STSH_PATH,av,env_as_array());
        _exit(127);
    }
    if (pid<0) perror("stsh: fork interpreter");
    else if (g_jobcontrol) {
        if ((setpgid(pid,pid)<0 && errno!=EACCES && errno!=ESRCH) ||
            tcsetpgrp(g_tty_fd,pid)<0) {
            perror("stsh: state foreground");
            if (kill(pid,SIGKILL)<0 && errno!=ESRCH) perror("stsh: kill state job");
            sh_wait(pid,"aborted state job");
            if (tcsetpgrp(g_tty_fd,*oldfg)<0) perror("stsh: restore foreground");
            *oldfg=-1;
            return -1;
        }
    }
    return pid;
}

static void
state_restore_foreground(pid_t oldfg)
{
    if (oldfg>=0 && tcsetpgrp(g_tty_fd,oldfg)<0)
        perror("stsh: restore foreground");
}

/* ═══════════════════════════ arithmetic $(( )) ═══════════════════════════ */
/* Recursive-descent integer evaluator: + - * / % , unary - ! ~, comparisons,
 * && ||, ( ), bare names (env value), and assignment (= += -= *= /= %=). */

struct ap { const char *s; };
static long ar_expr(struct ap *a);
static void ar_ws(struct ap *a){ while (*a->s==' '||*a->s=='\t') a->s++; }

static long ar_prim(struct ap *a){
    ar_ws(a);
    if (*a->s=='('){ a->s++; long v=ar_expr(a); ar_ws(a); if(*a->s==')')a->s++; return v; }
    if (isdigit((unsigned char)*a->s)) return strtol(a->s,(char**)&a->s,0);
    if (isalpha((unsigned char)*a->s)||*a->s=='_'){
        char nm[64]; int n=0;
        while (isalnum((unsigned char)*a->s)||*a->s=='_'){ if(n<63)nm[n++]=*a->s; a->s++; }
        nm[n]='\0'; const char*v=env_get(nm); return v?strtol(v,NULL,0):0;
    }
    return 0;
}
static long ar_un(struct ap *a){
    ar_ws(a);
    if(*a->s=='-'){a->s++;return -ar_un(a);} if(*a->s=='+'){a->s++;return ar_un(a);}
    if(*a->s=='!'){a->s++;return !ar_un(a);} if(*a->s=='~'){a->s++;return ~ar_un(a);}
    return ar_prim(a);
}
static long ar_mul(struct ap *a){ long v=ar_un(a); for(;;){ar_ws(a);char c=*a->s;
    if(c=='*'){a->s++;v*=ar_un(a);} else if(c=='/'){a->s++;long d=ar_un(a);v=d?v/d:0;}
    else if(c=='%'){a->s++;long d=ar_un(a);v=d?v%d:0;} else break;} return v;}
static long ar_add(struct ap *a){ long v=ar_mul(a); for(;;){ar_ws(a);char c=*a->s;
    if(c=='+'){a->s++;v+=ar_mul(a);} else if(c=='-'){a->s++;v-=ar_mul(a);} else break;} return v;}
static long ar_cmp(struct ap *a){ long v=ar_add(a); for(;;){ar_ws(a);
    if(a->s[0]=='<'&&a->s[1]=='='){a->s+=2;v=(v<=ar_add(a));}
    else if(a->s[0]=='>'&&a->s[1]=='='){a->s+=2;v=(v>=ar_add(a));}
    else if(a->s[0]=='<'){a->s++;v=(v<ar_add(a));}
    else if(a->s[0]=='>'){a->s++;v=(v>ar_add(a));} else break;} return v;}
static long ar_eq(struct ap *a){ long v=ar_cmp(a); for(;;){ar_ws(a);
    if(a->s[0]=='='&&a->s[1]=='='){a->s+=2;v=(v==ar_cmp(a));}
    else if(a->s[0]=='!'&&a->s[1]=='='){a->s+=2;v=(v!=ar_cmp(a));} else break;} return v;}
static long ar_and(struct ap *a){ long v=ar_eq(a); for(;;){ar_ws(a);
    if(a->s[0]=='&'&&a->s[1]=='&'){a->s+=2;long r=ar_eq(a);v=(v&&r);} else break;} return v;}
static long ar_or(struct ap *a){ long v=ar_and(a); for(;;){ar_ws(a);
    if(a->s[0]=='|'&&a->s[1]=='|'){a->s+=2;long r=ar_and(a);v=(v||r);} else break;} return v;}
static long ar_assign(struct ap *a){
    ar_ws(a); const char *save=a->s;
    if (isalpha((unsigned char)*a->s)||*a->s=='_'){
        char nm[64]; int n=0; const char*q=a->s;
        while (isalnum((unsigned char)*q)||*q=='_'){ if(n<63)nm[n++]=*q; q++; }
        nm[n]='\0'; const char*r=q; while(*r==' '||*r=='\t')r++;
        char buf[32];
        if (*r=='='&&r[1]!='='){ a->s=r+1; long v=ar_assign(a); snprintf(buf,sizeof buf,"%ld",v); env_set(nm,buf); return v; }
        if ((*r=='+'||*r=='-'||*r=='*'||*r=='/'||*r=='%')&&r[1]=='='){
            char op=*r; a->s=r+2; long rhs=ar_assign(a);
            const char*cur=env_get(nm); long cv=cur?strtol(cur,NULL,0):0;
            long v = op=='+'?cv+rhs: op=='-'?cv-rhs: op=='*'?cv*rhs: op=='/'?(rhs?cv/rhs:0):(rhs?cv%rhs:0);
            snprintf(buf,sizeof buf,"%ld",v); env_set(nm,buf); return v;
        }
    }
    a->s=save; return ar_or(a);
}
static long ar_expr(struct ap *a){ return ar_assign(a); }
static long arith_eval(const char *expr){ struct ap a={expr}; return ar_expr(&a); }

/* ═══════════════════════════ expansion ═══════════════════════════ */

/* Append the value of one $-construct at *pp (points at '$') to a field
 * builder. `unq` = we're in an unquoted context (caller field-splits). Returns
 * the expanded value via emit_val callback semantics — here we return a static
 * buffer the caller splits. */
static const char *env_or_param(const char *name);
static char *expand_one(const char *raw, char *out, int outcap);

/* Read a $NAME / ${NAME} / $?/$#/$@/$0-9/$$ / $(...) / `...` starting at *pp
 * (points past nothing — *pp is at the char after '$' or '`'). Writes the raw
 * expansion into out (NUL-terminated), advances *pp. For $@/$* joins with ' '.
 * Command substitution recurses via sh_capture. */
static void
read_dollar(const char **pp, char *out, int outcap)
{
    const char *p = *pp;   /* points just past '$' */
    int o = 0;
#define OUT(c) do { if (o < outcap - 1) out[o++] = (c); } while (0)
#define OUTS(s) do { const char *_s=(s); while (_s && *_s) OUT(*_s++); } while (0)
    char c = *p;
    char tmp[32];

    if (c == '?') { snprintf(tmp,sizeof tmp,"%d",g_last_exit); OUTS(tmp); p++; }
    else if (c == '$') { snprintf(tmp,sizeof tmp,"%d",(int)getpid()); OUTS(tmp); p++; }
    else if (c == '#') { snprintf(tmp,sizeof tmp,"%d",g_nparams); OUTS(tmp); p++; }
    else if (c == '@' || c == '*') {
        for (int i=0;i<g_nparams;i++){ if(i)OUT(' '); OUTS(g_params[i]); } p++;
    }
    else if (c >= '0' && c <= '9') {
        int idx=c-'0';
        OUTS(idx==0 ? (g_arg0?g_arg0:"") : (idx<=g_nparams?g_params[idx-1]:"")); p++;
    }
    else if (c == '(' && p[1] == '(') {          /* $(( arithmetic )) */
        p += 2; const char *st=p; int depth=0;
        while (*p){ if(*p=='(')depth++; else if(*p==')'){ if(depth>0)depth--; else break; } p++; }
        int len=(int)(p-st); char ex[LINE_SIZE]; if(len>(int)sizeof(ex)-1)len=sizeof(ex)-1;
        memcpy(ex,st,len); ex[len]='\0';
        if(*p==')')p++; if(*p==')')p++;
        /* parameter-expand the expression first ($1, $x, $(cmd) → values),
         * then evaluate; bare names are resolved by arith_eval itself. */
        char ex2[LINE_SIZE]; expand_one(ex, ex2, sizeof ex2);
        char tmp[32]; snprintf(tmp,sizeof tmp,"%ld",arith_eval(ex2)); OUTS(tmp);
    }
    else if (c == '(') {                         /* $(...) command substitution */
        p++; const char *st=p; int d=1;
        while (*p && d){ if(*p=='(')d++; else if(*p==')'){d--; if(!d)break;} p++; }
        int len=(int)(p-st); char cmd[LINE_SIZE]; if(len>(int)sizeof(cmd)-1)len=sizeof(cmd)-1;
        memcpy(cmd,st,len); cmd[len]='\0'; if(*p==')')p++;
        int u=0; sh_capture(cmd,out,&u,outcap); o=u;
    }
    else if (c == '{') {
        p++; char nm[128]; int n=0;
        while (*p && *p!='}' && n<(int)sizeof(nm)-1) nm[n++]=*p++;
        nm[n]='\0'; if(*p=='}')p++;
        OUTS(env_or_param(nm));
    }
    else if (c=='_'||(c>='a'&&c<='z')||(c>='A'&&c<='Z')) {
        char nm[128]; int n=0;
        while ((*p=='_'||(*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9')) && n<(int)sizeof(nm)-1)
            nm[n++]=*p++;
        nm[n]='\0'; OUTS(env_or_param(nm));
    }
    else { OUT('$'); }   /* bare $ */
    out[o]='\0';
    *pp=p;
#undef OUT
#undef OUTS
}

static const char *
env_or_param(const char *name)
{
    const char *v = env_get(name);
    return v ? v : "";
}

/* Field builder used by expand_word. */
struct fb {
    char **argv; int *pargc; int maxargc;
    char  *arena; int *pused; int arenacap;
    int    fstart;   /* arena offset of current field */
    int    fhas;     /* current field has any char */
    int    fquoted;  /* current field had a quote/escape (suppresses glob) */
    int    noglob;   /* never pathname-glob (case patterns, assignment RHS) */
    int    nosplit;  /* single-word contexts preserve whitespace verbatim */
};

static void
fb_putc(struct fb *b, char c)
{
    if (*b->pused < b->arenacap - 1) { b->arena[(*b->pused)++] = c; b->fhas = 1; }
}

/* finish current field: glob if eligible, append to argv */
static void
fb_finish(struct fb *b)
{
    if (!b->fhas) return;
    if (*b->pused < b->arenacap) b->arena[(*b->pused)++] = '\0';
    char *field = &b->arena[b->fstart];

    int globbed = 0;
    if (!b->fquoted && !b->noglob && (strpbrk(field, "*?[") != NULL)) {
        glob_t g;
        if (glob(field, 0, NULL, &g) == 0 && g.gl_pathc > 0) {
            for (size_t k = 0; k < g.gl_pathc; k++) {
                if (*b->pargc < b->maxargc) {
                    /* copy match into arena (persist) */
                    int s = *b->pused;
                    for (const char *m = g.gl_pathv[k]; *m; m++)
                        if (*b->pused < b->arenacap - 1) b->arena[(*b->pused)++] = *m;
                    if (*b->pused < b->arenacap) b->arena[(*b->pused)++] = '\0';
                    b->argv[(*b->pargc)++] = &b->arena[s];
                }
            }
            globbed = 1;
        }
        globfree(&g);
    }
    if (!globbed && *b->pargc < b->maxargc)
        b->argv[(*b->pargc)++] = field;

    b->fstart = *b->pused; b->fhas = 0; b->fquoted = 0;
}

/* emit an expansion value; if unquoted, split on IFS whitespace into fields */
static void
fb_emit_value(struct fb *b, const char *val, int unquoted)
{
    if (!unquoted) { while (*val) fb_putc(b, *val++); return; }
    const char *v = val;
    while (*v) {
        if (*v==' '||*v=='\t'||*v=='\n') {
            fb_finish(b);
            while (*v==' '||*v=='\t'||*v=='\n') v++;
        } else { fb_putc(b, *v); v++; }
    }
}

/*
 * expand_word — expand one RAW word into 0+ argv entries (quote removal, var/
 * cmd substitution, field-splitting of unquoted expansions, globbing).
 */
static void
expand_word(const char *raw, struct fb *b)
{
    const char *p = raw;
    char val[LINE_SIZE];

    while (*p) {
        char c = *p;
        if (c == '\'') {                       /* single quote: literal, no split */
            b->fquoted = 1; b->fhas = 1; p++;
            while (*p && *p != '\'') fb_putc(b, *p++);
            if (*p) p++;
        } else if (c == '"') {                 /* double quote: expand, no split */
            b->fquoted = 1; b->fhas = 1; p++;
            while (*p && *p != '"') {
                if (*p == '$') { const char *q=p+1; read_dollar(&q,val,sizeof val); p=q; fb_emit_value(b,val,0); }
                else if (*p == '`') { const char *q=p+1; const char *st=q; while(*q&&*q!='`')q++; int len=(int)(q-st); char cmd[LINE_SIZE]; if(len>(int)sizeof(cmd)-1)len=sizeof(cmd)-1; memcpy(cmd,st,len); cmd[len]='\0'; if(*q=='`')q++; p=q; int u=0; char cap[LINE_SIZE]; sh_capture(cmd,cap,&u,sizeof cap); cap[u]='\0'; fb_emit_value(b,cap,0); }
                else if (*p=='\\' && (p[1]=='"'||p[1]=='\\'||p[1]=='$'||p[1]=='`')) { fb_putc(b,p[1]); p+=2; }
                else fb_putc(b, *p++);
            }
            if (*p) p++;
        } else if (c == '\\') {                /* escape: literal next, no split */
            b->fquoted = 1;
            if (p[1]) { fb_putc(b,p[1]); p+=2; } else p++;
        } else if (c == '$') {
            const char *q=p+1; read_dollar(&q,val,sizeof val); p=q;
            fb_emit_value(b, val, !b->nosplit);
        } else if (c == '`') {
            const char *q=p+1; const char *st=q; while(*q&&*q!='`')q++;
            int len=(int)(q-st); char cmd[LINE_SIZE]; if(len>(int)sizeof(cmd)-1)len=sizeof(cmd)-1;
            memcpy(cmd,st,len); cmd[len]='\0'; if(*q=='`')q++; p=q;
            int u=0; char cap[LINE_SIZE]; sh_capture(cmd,cap,&u,sizeof cap); cap[u]='\0';
            fb_emit_value(b, cap, !b->nosplit);
        } else {
            fb_putc(b, c); p++;
        }
    }
    fb_finish(b);
}

/* Expand a single word to exactly one string, written into `out` (returns it).
 * Used for assignment RHS, redirect targets, case word, `for` var value. Fields
 * in these contexts are not field-split or pathname-expanded. */
static char *
expand_one(const char *raw, char *out, int outcap)
{
    char  tmp[LINE_SIZE]; int used = 0;
    char *argv[16]; int argc = 0;
    struct fb b = { argv, &argc, 16, tmp, &used, sizeof tmp, 0, 0, 0, 1, 1 };
    expand_word(raw, &b);
    int j = 0;
    for (int i = 0; i < argc && j < outcap-1; i++) {
        if (i && j < outcap-1) out[j++] = ' ';
        for (const char *s = argv[i]; *s && j < outcap-1; s++) out[j++] = *s;
    }
    out[j] = '\0';
    return out;
}

/* ═══════════════════════════ capture ═══════════════════════════ */

void
sh_capture(const char *cmd, char *buf, int *used, int cap)
{
    s_capture_serial++;
    tok_t toks[MAX_TOKENS]; char arena[LEX_ARENA*4];
    int nt=tokenize(cmd,toks,MAX_TOKENS,arena,sizeof arena);
    if (nt<0) { fprintf(stderr,"stsh: substitution too large\n"); g_last_exit=2; return; }
    char *payload=state_make(toks,nt);
    if (!payload) { g_last_exit=2; return; }
    int pfd[2];
    if (pipe(pfd)<0) { perror("stsh: capture pipe"); free(payload); g_last_exit=1; return; }
    pid_t oldfg;
    pid_t pid=state_spawn(payload,pfd[1],pfd[0],&oldfg);
    free(payload);
    if (pid<0) {
        close(pfd[0]); close(pfd[1]); g_last_exit=1; return;
    }
    close(pfd[1]);
    int start = *used;
    char rb[512]; ssize_t r; int failed=0;
    for (;;) {
        r=read(pfd[0],rb,sizeof rb);
        if (r<0 && errno==EINTR) continue;
        if (r<=0) { if(r<0){perror("stsh: capture read");failed=1;} break; }
        for (ssize_t k = 0; k < r; k++)
            if (*used < cap - 1) buf[(*used)++] = rb[k];
    }
    close(pfd[0]);
    g_last_exit=sh_wait(pid,"command substitution");
    state_restore_foreground(oldfg);
    if (failed) g_last_exit=1;
    while (*used > start && buf[*used - 1] == '\n') (*used)--;
    if (*used < cap) buf[*used] = '\0';
}

/* ═══════════════════════════ builtins (shell-side) ═══════════════════════════ */

static int is_assignment(const char *w){
    if (!w || !(*w=='_'||(*w>='a'&&*w<='z')||(*w>='A'&&*w<='Z'))) return 0;
    const char *p=w+1;
    while (*p=='_'||(*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9')) p++;
    return *p=='=';
}

/* Resolve NAME to an executable path via PATH (or accept a slash-path). */
static int
find_in_path(const char *name, char *out, int outcap)
{
    if (strchr(name, '/')) {
        if (access(name, X_OK) == 0) { strncpy(out,name,outcap-1); out[outcap-1]='\0'; return 1; }
        return 0;
    }
    const char *path = env_get("PATH"); if (!path) path = "/bin";
    const char *p = path;
    while (*p) {
        const char *e = strchr(p, ':'); int len = e ? (int)(e-p) : (int)strlen(p);
        char cand[512];
        if (len > 0 && len < (int)sizeof(cand)-2-(int)strlen(name)) {
            memcpy(cand, p, len); cand[len]='/'; strcpy(cand+len+1, name);
            if (access(cand, X_OK) == 0) { strncpy(out,cand,outcap-1); out[outcap-1]='\0'; return 1; }
        }
        if (!e) break; p = e + 1;
    }
    return 0;
}

static int
is_builtin_name(const char *n)
{
    static const char *b[] = { "cd","exit","export","unset","set","read","shift",
        "return","break","continue",":","true","false",".","source","exec","env",
        "command","umask","type","caps","sandbox","grant","admin","deadmin","help",0 };
    for (int i=0;b[i];i++) if(!strcmp(n,b[i])) return 1;
    return 0;
}

/* Pipeline/background builtins and functions exec before interpreting. Quote
 * expanded argv into literal WORD tokens, never expand substitutions twice. */
int
command_bootstrap(cmd_t *cmd)
{
    if (cmd->external_only) return 0;
    if (!cmd->argv[0] || (!is_builtin_name(cmd->argv[0]) && !func_find(cmd->argv[0])))
        return 0;
    char *arena=malloc(STSH_STATE_MAX);
    if (!arena) { perror("stsh: command state"); return -1; }
    tok_t tokens[MAX_ARGV]; size_t used=0;
    for (int i=0;i<cmd->argc;i++) {
        tokens[i]=(tok_t){T_WORD,arena+used};
        if (used+3>=STSH_STATE_MAX) goto large;
        arena[used++]='\'';
        for (const char *p=cmd->argv[i];*p;p++) {
            if (used+5>=STSH_STATE_MAX) goto large;
            if (*p=='\'') { memcpy(arena+used,"'\\''",4); used+=4; }
            else arena[used++]=*p;
        }
        arena[used++]='\''; arena[used++]='\0';
    }
    cmd->bootstrap=state_make(tokens,cmd->argc);
    free(arena);
    return cmd->bootstrap ? 0 : -1;
large:
    free(arena);
    fprintf(stderr,"stsh: command interpreter state too large\n");
    return -1;
}

/* Returns 1 if handled as a shell-process builtin. */
static int
shell_builtin(char **argv, int argc)
{
    const char *c = argv[0];
    if (!c) return 0;

    if (!strcmp(c,"command")) {
        if (argc>=3 && (!strcmp(argv[1],"-v")||!strcmp(argv[1],"-V"))) {
            const char *nm=argv[2]; char rp[512];
            if (func_find(nm) || is_builtin_name(nm)) { puts(nm); g_last_exit=0; }
            else if (find_in_path(nm, rp, sizeof rp)) { puts(rp); g_last_exit=0; }
            else g_last_exit=1;
            return 1;
        }
        if (argc>=2) {   /* `command cmd args` — run externally, bypassing funcs */
            cmd_t cc; memset(&cc,0,sizeof cc);
            cc.external_only=1;
            for (int i=1;i<argc;i++) cc.argv[i-1]=argv[i];
            cc.argv[argc-1]=NULL; cc.argc=argc-1;
            run_pipeline(&cc,1,env_as_array(),&g_last_exit);
            return 1;
        }
        g_last_exit=0; return 1;
    }

    if (!strcmp(c,":") || !strcmp(c,"true"))  { g_last_exit=0; return 1; }
    if (!strcmp(c,"false"))                    { g_last_exit=1; return 1; }
    if (!strcmp(c,"break"))    { g_flow=FLOW_BREAK;    g_flow_n=argv[1]?atoi(argv[1]):1; g_last_exit=0; return 1; }
    if (!strcmp(c,"continue")) { g_flow=FLOW_CONTINUE; g_flow_n=argv[1]?atoi(argv[1]):1; g_last_exit=0; return 1; }
    if (!strcmp(c,"return"))   { g_flow=FLOW_RETURN;   g_last_exit=argv[1]?atoi(argv[1]):g_last_exit; return 1; }
    if (!strcmp(c,"shift")) {
        int n = argv[1]?atoi(argv[1]):1;
        if (n<0) { g_last_exit=2; return 1; }
        if (n>g_nparams) n=g_nparams;
        for (int i=0;i<n;i++) free(g_params[i]);
        for (int i=0;i+n<g_nparams;i++) g_params[i]=g_params[i+n];
        g_nparams-=n; g_last_exit=0; return 1;
    }
    if (!strcmp(c,"unset")) { for (int i=1;i<argc;i++) env_unset(argv[i]); g_last_exit=0; return 1; }
    if (!strcmp(c,"export")) {
        if (argc<2) { env_print_all(); }
        else for (int i=1;i<argc;i++){ char *eq=strchr(argv[i],'='); if(eq){*eq='\0';env_set(argv[i],eq+1);*eq='=';} }
        g_last_exit=0; return 1;
    }
    if (!strcmp(c,"set")) {   /* only `set -- args` (reset positional params) */
        if (argc>=2 && !strcmp(argv[1],"--")) run_set_params(g_arg0, &argv[2], argc-2);
        g_last_exit=0; return 1;
    }
    if (!strcmp(c,"read")) {  /* read [var...]; splits a line on IFS whitespace */
        char line[LINE_SIZE]; int n=0, ch;
        while ((ch=getchar())!=EOF && ch!='\n') if(n<(int)sizeof(line)-1) line[n++]=(char)ch;
        line[n]='\0';
        if (ch==EOF && n==0) { g_last_exit=1; return 1; }
        char *save; char *tok=strtok_r(line," \t",&save);
        for (int i=1;i<argc;i++){
            if (i==argc-1 && tok) {   /* last var gets the rest */
                env_set(argv[i], tok);
                /* append remaining tokens */
                char rest[LINE_SIZE]; strncpy(rest,tok,sizeof rest-1); rest[sizeof rest-1]='\0';
                char *t2; int rl=strlen(rest);
                while ((t2=strtok_r(NULL," \t",&save))){ if(rl<(int)sizeof(rest)-2){rest[rl++]=' ';rest[rl]='\0';} strncat(rest,t2,sizeof rest-rl-1); rl=strlen(rest); }
                env_set(argv[i], rest);
            } else { env_set(argv[i], tok?tok:""); tok=strtok_r(NULL," \t",&save); }
        }
        g_last_exit=0; return 1;
    }
    if (!strcmp(c,"cd")) {
        const char *path = argv[1] ? argv[1] : env_get("HOME"); if(!path)path="/";
        if (chdir(path)!=0){ perror(path); g_last_exit=1; } else g_last_exit=0;
        return 1;
    }
    if (!strcmp(c,"exit")) { hist_save(); exit(argv[1]?atoi(argv[1]):g_last_exit); }
    if (!strcmp(c,".") || !strcmp(c,"source")) {
        if (argv[1]) run_script(argv[1], &argv[2], argc-2>0?argc-2:0);
        else { fprintf(stderr,"source: filename required\n"); g_last_exit=1; }
        return 1;
    }
    if (!strcmp(c,"exec") && argv[1]) {
        char full[512];
        if (argv[1][0]!='/') {
            if (!find_in_path(argv[1], full, sizeof full)) {
                fprintf(stderr,"exec: %s: not found\n",argv[1]); g_last_exit=127; return 1;
            }
            execve(full, &argv[1], env_as_array());
        } else
            execve(argv[1], &argv[1], env_as_array());
        fprintf(stderr,"exec: %s: %s\n",argv[1],strerror(errno)); g_last_exit=127; return 1;
    }
    if (!strcmp(c,"umask")) {
        /* umask [mode] — set or print the file-creation mask.  No arg prints the
         * current umask as an octal string.  A numeric arg (octal by POSIX
         * convention) sets it.  Returns 0 on success. */
        if (argc < 2) {
            mode_t m = umask(0);
            umask(m);
            printf("%03o\n", (unsigned)m);
        } else {
            mode_t m = (mode_t)strtol(argv[1], NULL, 8);
            umask(m);
        }
        g_last_exit=0; return 1;
    }
    if (!strcmp(c,"type")) {
        /* type name... — print how each name would be resolved.  Exit 0 if all
         * found, 1 if any not found. */
        int rc = 0;
        for (int i = 1; i < argc; i++) {
            const char *nm = argv[i];
            char rp[512];
            if (func_find(nm))
                printf("%s is a function\n", nm);
            else if (is_builtin_name(nm))
                printf("%s is a shell builtin\n", nm);
            else if (find_in_path(nm, rp, sizeof rp))
                printf("%s is %s\n", nm, rp);
            else {
                fprintf(stderr,"type: %s: not found\n", nm);
                rc = 1;
            }
        }
        g_last_exit = rc;
        return 1;
    }
    if (!strcmp(c,"env"))  { env_print_all(); g_last_exit=0; return 1; }
    if (!strcmp(c,"help")) {
        puts("stsh: POSIX-ish shell (if/for/while/case, functions, globbing)");
        puts("admin command [args...]: authenticate one allowlisted /bin command");
        puts("Commands do not inherit the shell's authentication or capabilities.");
        g_last_exit=0; return 1;
    }

    /* cap-model builtins (from caps.c) */
    if (!strcmp(c,"caps"))    { g_last_exit=caps_builtin(argc,argv); return 1; }
    if (!strcmp(c,"sandbox")) { g_last_exit=sandbox_builtin(argc,argv,env_as_array()); return 1; }
    if (!strcmp(c,"grant"))   { g_last_exit=grant_builtin(argc,argv); return 1; }
    if (!strcmp(c,"admin"))   { g_last_exit=admin_builtin(argc,argv); return 1; }
    if (!strcmp(c,"deadmin")) { g_last_exit=deadmin_builtin(); return 1; }

    return 0;
}

/* ═══════════════════════════ parser / executor ═══════════════════════════ */

static int run_list(int *pi, int run, int in_loop);
static int run_and_or(int *pi, int run, int in_loop);

static int is_kw(int i, const char *w){ return T[i].type==T_WORD && T[i].text && !strcmp(T[i].text,w); }

/* skip newlines and semicolons (statement separators) */
static void skip_seps(int *pi){
    while (T[*pi].type==T_NEWLINE || T[*pi].type==T_SEMI) (*pi)++;
}
static void skip_newlines(int *pi){ while (T[*pi].type==T_NEWLINE) (*pi)++; }

/* Does a WORD token close the current list context? */
static int at_block_end(int i){
    if (T[i].type==T_EOF || T[i].type==T_RPAREN) return 1;
    if (T[i].type==T_WORD) {
        const char *w=T[i].text;
        return !strcmp(w,"then")||!strcmp(w,"else")||!strcmp(w,"elif")||!strcmp(w,"fi")
             ||!strcmp(w,"do")||!strcmp(w,"done")||!strcmp(w,"esac")||!strcmp(w,"}");
    }
    return 0;
}

static int is_compound_start(int i){
    if (T[i].type==T_LPAREN) return 1;
    if (T[i].type==T_WORD) {
        const char *w=T[i].text;
        return !strcmp(w,"if")||!strcmp(w,"for")||!strcmp(w,"while")||!strcmp(w,"until")
             ||!strcmp(w,"case")||!strcmp(w,"{");
    }
    return 0;
}

/* ── redirections for in-process builtins/functions ── */

static void redir_restore(int saved[3]);

static int
redir_apply(cmd_t *c, int saved[3])
{
    int fds[3];
    if (redir_prepare(c, fds, -1, -1) < 0) return -1;
    fflush(NULL);
    saved[0]=saved[1]=saved[2]=-1;
    for (int i=0; i<3; i++) {
        saved[i]=sh_save_fd(i);
        if (saved[i]<0) {
            if (errno==EBADF) { saved[i]=-2; continue; } /* originally closed */
            perror("stsh: save descriptors");
            redir_close(saved); redir_close(fds);
            return -1;
        }
    }
    if (redir_install(fds)<0) {
        perror("stsh: install descriptors");
        redir_close(fds); redir_restore(saved);
        return -1;
    }
    redir_close(fds);
    clearerr(stdin); clearerr(stdout); clearerr(stderr);
    return 0;
}

static void
redir_restore(int saved[3])
{
    fflush(stdout); fflush(stderr);
    for (int i=0;i<3;i++) {
        if (saved[i]>=0) {
            if (dup2(saved[i],i)<0) perror("stsh: restore descriptor");
            close(saved[i]);
        } else if (saved[i]==-2) close(i);
    }
    clearerr(stdin); clearerr(stdout); clearerr(stderr);
}

/* Both simple commands and pipeline stages use the same ordered syntax. */
static int
parse_redir(int *pi, cmd_t *cmd)
{
    int i=*pi, operand=1;
    redir_kind kind;
    switch (T[i].type) {
    case T_LT: kind=R_IN; break;
    case T_DLESS: kind=R_HERE; break;
    case T_GT: kind=R_OUT; break;
    case T_GTGT: kind=R_APPEND; break;
    case T_DGT: kind=R_ERR; break;
    case T_DGTAMP: kind=R_ERR_OUT; operand=0; break;
    case T_GTAMP: kind=R_DUP; break;
    default: return 0;
    }
    *pi=i+1;
    if ((operand && T[i+1].type!=T_WORD) || cmd->nredirs==MAX_REDIR)
        return -1;
    cmd->redirs[cmd->nredirs++]=(redir_t){kind, operand ? T[i+1].text : NULL};
    if (operand) (*pi)++;
    return 1;
}

int
run_expand_redirect(const char *raw, char *out, int cap)
{
    expand_one(raw,out,cap);
    if (strlen(out)>=(size_t)cap-1) {
        errno=E2BIG;
        return -1;
    }
    return 0;
}

/* ── simple command ── */

static int
run_simple(int *pi, int run)
{
    int i = *pi;
    char *raw[MAX_ARGV]; int nraw = 0;
    cmd_t cmd; memset(&cmd, 0, sizeof cmd);
    int bad_redir=0;

    while (T[i].type != T_EOF) {
        toktype_t tt = T[i].type;
        if (tt==T_WORD) {
            /* reserved words are only special in command position (first word);
             * as an argument (`echo done`) they are literal. */
            if (nraw==0 && at_block_end(i)) break;
            if (nraw < MAX_ARGV) raw[nraw++] = T[i].text;
            i++;
        } else {
            int r=parse_redir(&i,&cmd);
            if (!r) break;
            if (r<0) bad_redir=1;
        }
    }
    *pi = i;
    if (!run) return g_last_exit;
    if (bad_redir) { fprintf(stderr,"stsh: invalid or excessive redirections\n"); return (g_last_exit=2); }

    /* expand: assignments first (RHS expanded, no split), then argv words */
    unsigned capture_before=s_capture_serial;
    char argbuf[LINE_SIZE*2]; int used=0; char *argv[MAX_ARGV+1]; int argc=0;
    int k = 0;
    while (k < nraw && is_assignment(raw[k])) {
        char *eq = strchr(raw[k],'='); *eq='\0';
        char tmp[LINE_SIZE];
        char *val = expand_one(eq+1, tmp, sizeof tmp);
        env_set(raw[k], val);
        *eq='=';
        k++;
    }
    for (; k < nraw; k++) {
        struct fb b = { argv, &argc, MAX_ARGV, argbuf, &used, sizeof argbuf, used, 0, 0 };
        b.fstart = used;
        expand_word(raw[k], &b);
    }
    argv[argc] = NULL;

    if (argc==0) {
        int fds[3];
        if (redir_prepare(&cmd,fds,-1,-1)<0) return (g_last_exit=1);
        redir_close(fds);
        if (s_capture_serial==capture_before) g_last_exit=0;
        return g_last_exit;
    }

    int need_redir = cmd.nredirs!=0;
    int rsaved[3];

    for (int a=0;a<=argc;a++) cmd.argv[a]=argv[a];
    cmd.argc=argc;
    if (T[*pi].type==T_AMP) {
        g_last_exit=run_pipeline_bg(&cmd,1,env_as_array());
        free(cmd.bootstrap);
        return g_last_exit;
    }

    /* function call? runs against the function's private token copy */
    struct func *fn = func_find(argv[0]);
    if (fn) {
        if (need_redir && redir_apply(&cmd,rsaved)<0){ g_last_exit=1; return 1; }
        char *saved[MAX_PARAMS]; int sn=g_nparams; char *s0=strdup(g_arg0);
        if (!s0) { fprintf(stderr,"stsh: cannot save parameters\n"); exit(2); }
        for (int x=0;x<g_nparams;x++) {
            saved[x]=strdup(g_params[x]);
            if (!saved[x]) { fprintf(stderr,"stsh: cannot save parameters\n"); exit(2); }
        }
        run_set_params(g_arg0, &argv[1], argc-1);
        tok_t *sT=T; int sNT=NT;
        T=fn->toks; NT=fn->nt;
        int idx=0;
        run_and_or(&idx, 1, 0);            /* body is a compound (brace group) */
        T=sT; NT=sNT;
        run_set_params(s0, saved, sn);
        free(s0); for (int x=0;x<sn;x++) free(saved[x]);
        if (g_flow==FLOW_RETURN) g_flow=FLOW_NONE;
        if (need_redir) redir_restore(rsaved);
        return g_last_exit;
    }

    for (int a=0;a<argc;a++) cmd.argv[a]=argv[a];
    cmd.argv[argc]=NULL; cmd.argc=argc;

    /* in-process builtins honor redirections too (e.g. command -v x >/dev/null) */
    if (is_builtin_name(argv[0])) {
        if (need_redir && redir_apply(&cmd,rsaved)<0){ g_last_exit=1; return 1; }
        shell_builtin(argv, argc);
        if (need_redir) redir_restore(rsaved);
        return g_last_exit;
    }

    run_pipeline(&cmd, 1, env_as_array(), &g_last_exit);
    return g_last_exit;
}

/* ── control-flow constructs ── */

/* advance *pi past a full list without executing (run=0), to reach the closer */
static void skip_to(int *pi, const char *closer){
    while (T[*pi].type != T_EOF && !is_kw(*pi, closer)) {
        int before=*pi;
        run_and_or(pi, 0, 0);
        skip_seps(pi);
        if (*pi==before) (*pi)++;   /* guard: never stall on a stray token */
    }
}

static int
run_if(int *pi, int run, int in_loop)
{
    int i=*pi+1;                    /* past 'if' */
    int taken=0, result=g_last_exit;
    for (;;) {
        int cond = run_list(&i, run && !taken, in_loop);
        skip_seps(&i);
        if (is_kw(i,"then")) i++;
        int dobody = run && !taken && cond==0;
        result = run_list(&i, dobody, in_loop);
        if (dobody) taken=1;
        skip_seps(&i);
        if (is_kw(i,"elif")) { i++; continue; }
        if (is_kw(i,"else")) { i++; int db = run && !taken; result=run_list(&i,db,in_loop); if(db)taken=1; skip_seps(&i); }
        break;
    }
    if (is_kw(i,"fi")) i++;
    *pi=i;
    return taken?result:0;
}

static int
run_for(int *pi, int run, int in_loop)
{
    (void)in_loop;
    int i=*pi+1;                    /* past 'for' */
    char var[64]="";
    if (T[i].type==T_WORD){ strncpy(var,T[i].text,sizeof var-1); var[sizeof var-1]='\0'; i++; }

    /* optional `in words...`; expand the list (split+glob) only when running */
    char *words[256]; int nw=0; static char wbuf[LINE_SIZE*8]; int wu=0;
    int use_params=1;
    if (is_kw(i,"in")) {
        use_params=0; i++;
        while (T[i].type==T_WORD && !is_kw(i,"do")) {
            if (run) { struct fb b={words,&nw,256,wbuf,&wu,sizeof wbuf,wu,0,0}; b.fstart=wu; expand_word(T[i].text,&b); }
            i++;
        }
    }
    skip_seps(&i);
    if (is_kw(i,"do")) i++;
    int body=i;
    int result=g_last_exit;

    if (!run) { int idx=body; skip_to(&idx,"done"); if(is_kw(idx,"done"))idx++; *pi=idx; return g_last_exit; }

    int count = use_params ? g_nparams : nw;
    for (int n=0; n<count; n++) {
        const char *val = use_params ? g_params[n] : words[n];
        if (var[0]) env_set(var, val);
        int idx=body;
        result=run_list(&idx,1,1);
        if (g_flow==FLOW_BREAK)    { if(--g_flow_n<=0)g_flow=FLOW_NONE; break; }
        if (g_flow==FLOW_CONTINUE) { if(--g_flow_n<=0){g_flow=FLOW_NONE; continue;} else break; }
        if (g_flow==FLOW_RETURN) break;
    }
    int idx=body; skip_to(&idx,"done"); if(is_kw(idx,"done"))idx++;
    *pi=idx;
    return result;
}

static int
run_while(int *pi, int run, int is_until)
{
    int cond_at=*pi+1;             /* past while/until */
    /* locate 'do' by parsing the condition list without executing */
    int scan=cond_at; run_list(&scan,0,0);   /* stops at 'do' (block-ender) */
    int body = is_kw(scan,"do") ? scan+1 : scan;
    int result=g_last_exit;

    if (!run) { int idx=body; skip_to(&idx,"done"); if(is_kw(idx,"done"))idx++; *pi=idx; return g_last_exit; }

    for (int guard=0; guard<1000000; guard++) {
        int ci=cond_at;
        int cond=run_list(&ci,1,0);
        int go = is_until ? (cond!=0) : (cond==0);
        if (!go) break;
        int idx=body;
        result=run_list(&idx,1,1);
        if (g_flow==FLOW_BREAK)    { if(--g_flow_n<=0)g_flow=FLOW_NONE; break; }
        if (g_flow==FLOW_CONTINUE) { if(--g_flow_n<=0){g_flow=FLOW_NONE; continue;} else break; }
        if (g_flow==FLOW_RETURN) break;
    }
    int idx=body; skip_to(&idx,"done"); if(is_kw(idx,"done"))idx++;
    *pi=idx;
    return result;
}

static int
run_case(int *pi, int run)
{
    int i=*pi+1;                    /* past 'case' */
    char wbuf[LINE_SIZE]; char *word = (run && T[i].type==T_WORD)? expand_one(T[i].text,wbuf,sizeof wbuf):"";
    if (T[i].type==T_WORD) i++;
    if (is_kw(i,"in")) i++;
    skip_seps(&i);
    int matched=0, result=g_last_exit;

    while (T[i].type!=T_EOF && !is_kw(i,"esac")) {
        /* optional leading '(' */
        if (T[i].type==T_LPAREN) i++;
        /* collect patterns until ')' , separated by '|' */
        int thismatch=0;
        for (;;) {
            if (T[i].type==T_WORD) {
                char pb[LINE_SIZE]; char *pat=run ? expand_one(T[i].text,pb,sizeof pb) : "";
                if (!matched && fnmatch(pat, word, 0)==0) thismatch=1;
                i++;
            }
            if (T[i].type==T_PIPE) { i++; continue; }
            break;
        }
        if (T[i].type==T_RPAREN) i++;
        /* body runs until ';;' or esac — can't use run_list (it eats ';;'). */
        int dobody = run && !matched && thismatch;
        for (;;) {
            skip_newlines(&i);
            if (T[i].type==T_SEMI && T[i+1].type==T_SEMI) break;
            if (T[i].type==T_EOF || is_kw(i,"esac")) break;
            int before=i;
            int s=run_and_or(&i, dobody, 0);
            if (dobody) result=s;
            if (T[i].type==T_SEMI && T[i+1].type==T_SEMI) break;
            if (T[i].type==T_SEMI || T[i].type==T_NEWLINE) i++;
            if (i==before) i++;                     /* never stall */
        }
        if (dobody) matched=1;
        if (T[i].type==T_SEMI && T[i+1].type==T_SEMI) i+=2;   /* consume ';;' */
        skip_seps(&i);
    }
    if (is_kw(i,"esac")) i++;
    *pi=i;
    return matched?result:0;
}

/* brace group { ...; } — run in current shell; subshell ( ... ) — run in child */
static int
run_brace(int *pi, int run)
{
    int i=*pi+1;                    /* past '{' */
    int r=run_list(&i, run, 0);
    if (is_kw(i,"}")) i++;
    *pi=i; return r;
}

static int
run_subshell(int *pi, int run)
{
    int i=*pi+1;                    /* past '(' */
    int body=i;
    run_list(&i,0,0);
    int end=i;
    int closed=T[i].type==T_RPAREN;
    if(closed)i++;
    *pi=i;
    if (!run) return g_last_exit;
    if (!closed) { fprintf(stderr,"stsh: unclosed subshell\n"); return (g_last_exit=2); }
    char *payload=state_make(T+body,end-body);
    if (!payload) return (g_last_exit=2);
    pid_t oldfg;
    pid_t pid=state_spawn(payload,-1,-1,&oldfg);
    free(payload);
    g_last_exit=pid<0 ? 1 : sh_wait(pid,"subshell");
    state_restore_foreground(oldfg);
    return g_last_exit;
}

/* function definition: `name () compound` — deep-copy the body tokens so the
 * function outlives the program buffer it was defined in. */
static int
try_funcdef(int *pi, int run)
{
    int i=*pi;
    if (T[i].type==T_WORD && T[i].text && T[i+1].type==T_LPAREN && T[i+2].type==T_RPAREN) {
        char name[64]; strncpy(name,T[i].text,sizeof name-1); name[sizeof name-1]='\0';
        i+=3; skip_newlines(&i);
        int body=i;
        run_and_or(&i, 0, 0);              /* skip the body compound to find its end */
        int len=i-body;
        if (!run) { *pi=i; return 1; }

        struct func *fn = func_find(name);
        tok_t *copy=calloc(len+1,sizeof(tok_t));
        if (!copy || (!fn && s_nfuncs==MAX_FUNCS)) {
            free(copy); fprintf(stderr,"stsh: function storage exhausted\n");
            *pi=i; g_last_exit=2; return 1;
        }
        for (int k=0;k<len;k++) {
            copy[k].type=T[body+k].type;
            if (T[body+k].text && !(copy[k].text=strdup(T[body+k].text))) {
                for (int x=0;x<k;x++) free(copy[x].text);
                free(copy); fprintf(stderr,"stsh: cannot copy function\n");
                *pi=i; g_last_exit=2; return 1;
            }
        }
        copy[len].type=T_EOF;
        if (!fn) fn=&s_funcs[s_nfuncs++];
        /* As before, prior definitions remain allocated: an active invocation
         * (including a caller of the current function) may still use them. */
        strcpy(fn->name,name); fn->toks=copy; fn->nt=len;
        *pi=i; g_last_exit=0;
        return 1;
    }
    return 0;
}

/* run one command (compound or simple), honoring a following pipe. */
static int
run_command(int *pi, int run, int in_loop)
{
    if (try_funcdef(pi,run)) return g_last_exit;

    int i=*pi;
    if (is_compound_start(i)) {
        int r;
        if (is_kw(i,"if"))          r=run_if(&i,run,in_loop);
        else if (is_kw(i,"for"))    r=run_for(&i,run,in_loop);
        else if (is_kw(i,"while"))  r=run_while(&i,run,0);
        else if (is_kw(i,"until"))  r=run_while(&i,run,1);
        else if (is_kw(i,"case"))   r=run_case(&i,run);
        else if (is_kw(i,"{"))      r=run_brace(&i,run);
        else if (T[i].type==T_LPAREN) r=run_subshell(&i,run);
        else r=g_last_exit;
        *pi=i;
        if (run) g_last_exit=r;
        return r;
    }
    return run_simple(pi, run);
}

/* pipeline: cmd (| cmd)*  — multi-stage requires all simple commands. */
static int
run_pipeline_node(int *pi, int run, int in_loop)
{
    int i=*pi;
    /* peek: is there a top-level pipe in this command? Gather simple stages. */
    if (is_compound_start(i) || (T[i].type==T_WORD && T[i+1].type==T_LPAREN && T[i+2].type==T_RPAREN)) {
        /* compound or funcdef: no multi-stage piping in v1 */
        return run_command(pi, run, in_loop);
    }

    cmd_t cmds[MAX_PIPELINE]; int nc=0, bad=0;
    char argbuf[MAX_PIPELINE][LINE_SIZE*2]; int argused[MAX_PIPELINE];
    /* parse+expand each stage */
    for (;;) {
        /* parse one simple command's raw tokens */
        int save=i;
        char *raw[MAX_ARGV]; int nraw=0; cmd_t cmd; memset(&cmd,0,sizeof cmd);
        while (T[i].type!=T_EOF) {
            toktype_t tt=T[i].type;
            if (tt==T_WORD){ if(nraw==0&&at_block_end(i))break; if(nraw<MAX_ARGV)raw[nraw++]=T[i].text; i++; }
            else {
                int r=parse_redir(&i,&cmd);
                if (!r) break;
                if (r<0) bad=1;
            }
        }
        if (nc==0 && T[i].type!=T_PIPE) { /* single stage — delegate to run_simple for builtins/functions */
            i=save; return run_simple(pi, run);
        }
        /* expand this stage into cmds[nc] */
        if (run && nc<MAX_PIPELINE) {
            int used=0; char *argv[MAX_ARGV+1]; int argc=0;
            int kk=0;
            while (kk<nraw && is_assignment(raw[kk])) { char*eq=strchr(raw[kk],'=');*eq='\0';char tmp[LINE_SIZE];env_set(raw[kk],expand_one(eq+1,tmp,sizeof tmp));*eq='=';kk++; }
            for (; kk<nraw; kk++){ struct fb b={argv,&argc,MAX_ARGV,argbuf[nc],&used,LINE_SIZE*2,used,0,0}; b.fstart=used; expand_word(raw[kk],&b); }
            argv[argc]=NULL;
            for (int a=0;a<=argc;a++) cmd.argv[a]=argv[a];
            cmd.argc=argc;
            argused[nc]=used;
            if (!argc) bad=1;
            cmds[nc++]=cmd;
        } else { nc++; if (nc>MAX_PIPELINE) bad=1; }

        if (T[i].type==T_PIPE) { i++; skip_newlines(&i); continue; }
        break;
    }
    *pi=i;
    (void)argused;
    if (!run) return g_last_exit;
    if (bad) {
        for (int x=0;x<nc && x<MAX_PIPELINE;x++) free(cmds[x].bootstrap);
        fprintf(stderr,"stsh: invalid or excessive pipeline/redirections\n");
        return (g_last_exit=2);
    }
    /* Background '&': run the pipeline without waiting (run_list's per-
     * statement loop reaps it with WNOHANG once it exits).  Same peek-after-
     * parse pattern as run_simple. */
    if (T[i].type == T_AMP) {
        g_last_exit=run_pipeline_bg(cmds, nc, env_as_array());
    } else {
        run_pipeline(cmds, nc, env_as_array(), &g_last_exit);
    }
    for (int x=0;x<nc;x++) free(cmds[x].bootstrap);
    return g_last_exit;
}

/* and-or list: pipeline (( && | || ) pipeline)* ; consumes one and-or list. */
static int
run_and_or(int *pi, int run, int in_loop)
{
    int i=*pi;
    int status = run_pipeline_node(&i, run, in_loop);
    while (T[i].type==T_AND || T[i].type==T_OR) {
        int op=T[i].type; i++; skip_newlines(&i);
        int dorun = run && ((op==T_AND && status==0)||(op==T_OR && status!=0));
        int s2 = run_pipeline_node(&i, dorun, in_loop);
        if (dorun) status=s2;
    }
    *pi=i;
    return status;
}

/* list: and-or (separator and-or)*  until a block-ender. */
static int
run_list(int *pi, int run, int in_loop)
{
    int i=*pi; int status=g_last_exit;
    for (;;) {
        skip_seps(&i);
        if (at_block_end(i) || T[i].type==T_EOF) break;
        int before=i;
        status = run_and_or(&i, run, in_loop);
        if (i==before) {
            if (run) fprintf(stderr,"stsh: unexpected token\n");
            *pi=i+1;
            return run ? (g_last_exit=2) : g_last_exit;
        }
        if (g_flow!=FLOW_NONE) break;                 /* unwind loops/functions */
        /* Reap finished background jobs between statements. This is the one
         * loop every entry point (REPL, -c, run_script, source, loop bodies)
         * funnels through, so it's the only place a WNOHANG drain is needed —
         * SIGCHLD=SIG_IGN does NOT auto-reap on Aegis (zombies persist until
         * waitpid), so without this a script that backgrounds jobs leaks one
         * zombie per job for its whole runtime. */
        if (run) sh_reap_background();
        /* background '&': run_and_or already peeked at the trailing T_AMP and
         * dispatched the pipeline via run_pipeline_bg (non-waiting).  Consume
         * the token here so the next command is its own statement. */
        if (T[i].type==T_AMP) i++;
        else if (T[i].type==T_SEMI || T[i].type==T_NEWLINE) { /* consumed by skip_seps */ }
        else if (at_block_end(i)) break;
    }
    *pi=i; return status;
}

/* ═══════════════════════════ entry points ═══════════════════════════ */

int
run_program(const char *text)
{
    /* Stack-local token buffers so an in-process `source` (nested run_program)
     * doesn't clobber the parent's in-flight tokens. Functions deep-copy their
     * bodies, so they safely outlive this frame. ~40K frame — fine. */
    tok_t toks[MAX_TOKENS];
    char  arena[LEX_ARENA*4];
    tok_t *saveT=T; int saveNT=NT;

    int nt = tokenize(text, toks, MAX_TOKENS, arena, sizeof arena);
    if (nt < 0) { fprintf(stderr,"stsh: program too large\n"); g_last_exit=2; }
    else {
        T = toks; NT = nt;
        int i=0;
        run_list(&i, 1, 0);
        if (g_flow==FLOW_RETURN) g_flow=FLOW_NONE;
    }
    T=saveT; NT=saveNT;
    return g_last_exit;
}

int run_line(const char *line){ return run_program(line); }

int
run_script(const char *path, char **argv_from, int argc_from)
{
    FILE *f=fopen(path,"r");
    if (!f){ fprintf(stderr,"stsh: %s: %s\n",path,strerror(errno)); return (g_last_exit=127); }

    char *s0=strdup(g_arg0); char *sp[MAX_PARAMS]; int sn=g_nparams;
    if (!s0) { fprintf(stderr,"stsh: cannot save parameters\n"); exit(2); }
    for (int i=0;i<g_nparams;i++) {
        sp[i]=strdup(g_params[i]);
        if (!sp[i]) { fprintf(stderr,"stsh: cannot save parameters\n"); exit(2); }
    }
    run_set_params(path, argv_from, argc_from);

    /* read whole file */
    static char buf[1<<18];   /* 256K max script */
    size_t n=fread(buf,1,sizeof buf-1,f); buf[n]='\0'; fclose(f);
    run_program(buf);

    run_set_params(s0,sp,sn);
    free(s0); for (int i=0;i<sn;i++) free(sp[i]);
    return g_last_exit;
}

/*
 * sh_incomplete — interactive continuation check: return 1 if buf has an open
 * construct (unbalanced if/for/while/until/case, {, (, quote, or trailing \).
 */
int
sh_incomplete(const char *buf)
{
    int depth=0; int sq=0,dq=0; const char *p=buf; char last=0;
    while (*p) {
        char c=*p;
        if (sq){ if(c=='\'')sq=0; p++; continue; }
        if (dq){ if(c=='"')dq=0; else if(c=='\\'&&p[1])p++; p++; continue; }
        if (c=='\''){ sq=1; p++; continue; }
        if (c=='"'){ dq=1; p++; continue; }
        if (c=='\\' && p[1]=='\n'){ return 1; }
        p++;
    }
    if (sq||dq) return 1;
    /* trailing backslash */
    { const char *e=buf+strlen(buf); if(e>buf && e[-1]=='\\') return 1; }
    /* reserved-word depth (word-boundary scan) */
    const char *q=buf;
    while (*q) {
        while (*q==' '||*q=='\t'||*q=='\n'||*q==';') q++;
        if (!*q) break;
        const char *s=q; while (*q && *q!=' '&&*q!='\t'&&*q!='\n'&&*q!=';') q++;
        int len=(int)(q-s);
        #define KW(w) (len==(int)strlen(w) && !strncmp(s,w,len))
        if (KW("if")||KW("for")||KW("while")||KW("until")||KW("case")) depth++;
        else if (KW("fi")||KW("done")||KW("esac")) { if(depth>0)depth--; }
        #undef KW
        (void)last;
    }
    return depth>0;
}
