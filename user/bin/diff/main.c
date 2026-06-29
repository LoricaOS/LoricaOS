/* diff — compare two files line by line (Myers shortest edit script).
 *
 *   diff [-q] [-i] [-w] [-u] FILE1 FILE2
 *
 *     -q   report only whether the files differ
 *     -i   ignore case when comparing lines
 *     -w   ignore all whitespace when comparing lines
 *     -u   unified output format (3 context lines)
 *
 * Either FILE may be "-" for stdin (but not both). Default output is the
 * classic normal format (N1,N2cN3,N4 / NdN / NaN hunks). A file with a
 * NUL byte in its first 4096 bytes is treated as binary; binary pairs are
 * compared bytewise. A missing trailing newline is flagged with GNU's
 * "\ No newline at end of file" marker (and makes the line compare
 * unequal to its newline-terminated twin).
 *
 * The whole diff is done in memory. Caps: 8 MB per file, 16384 lines per
 * file, edit distance D <= 8192, and <= 64 MB of Myers V-array snapshots
 * (kept per D step for the traceback). Exceeding any cap prints
 * "diff: files differ (too large for full diff)" and exits 1.
 *
 * Exit status: 0 identical, 1 different, 2 trouble.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/stat.h>

#define MAX_BYTES      (8u * 1024 * 1024)   /* per file */
#define MAX_LINES      16384                /* per file */
#define MAX_D          8192                 /* edit-distance cap */
#define MAX_SNAP_BYTES (64u * 1024 * 1024)  /* traceback snapshot budget */
#define BIN_PROBE      4096                 /* NUL scan window */
#define CTX            3                    /* unified context lines */

typedef struct {
    const char *ptr;     /* into file data, excludes '\n' */
    int         len;
    uint32_t    hash;    /* of normalized content + noeol flag */
    uint8_t     noeol;   /* last line of a file with no trailing \n */
} line_t;

typedef struct {
    const char *name;
    char       *data;
    size_t      len;
    line_t     *ln;
    int         n;
    int         noeol;
} file_t;

typedef struct {
    int as, na;          /* deleted run in FILE1: lines [as, as+na) */
    int bs, nb;          /* inserted run in FILE2: lines [bs, bs+nb) */
} blk_t;

static file_t   s_a, s_b;
static uint8_t *s_ach, *s_bch;              /* per-line changed flags */
static int      s_icase, s_iws, s_unified, s_quiet;

static int     *s_snap[MAX_D];              /* V snapshot per D step */
static size_t   s_snap_bytes;

static void too_large(void)
{
    printf("diff: files differ (too large for full diff)\n");
    exit(1);
}

static void usage(void)
{
    fprintf(stderr, "usage: diff [-q] [-i] [-w] [-u] file1 file2\n");
    exit(2);
}

/* ---- input ---- */

static void read_all(file_t *f)
{
    int fd = 0;
    int is_stdin = (strcmp(f->name, "-") == 0);

    if (!is_stdin) {
        fd = open(f->name, O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "diff: %s: %s\n", f->name, strerror(errno));
            exit(2);
        }
    }
    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) {
        fprintf(stderr,
                "diff: %s: is a directory (recursive diff not supported)\n",
                f->name);
        exit(2);
    }

    size_t cap = 65536, len = 0;
    char *buf = malloc(cap);
    if (!buf) too_large();
    for (;;) {
        if (len == cap) {
            cap *= 2;
            if (cap > (size_t)MAX_BYTES + 1) cap = (size_t)MAX_BYTES + 1;
            char *nb = realloc(buf, cap);
            if (!nb) too_large();
            buf = nb;
        }
        ssize_t r = read(fd, buf + len, cap - len);
        if (r < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "diff: %s: %s\n", f->name, strerror(errno));
            exit(2);
        }
        if (r == 0) break;
        len += (size_t)r;
        if (len > MAX_BYTES) too_large();
    }
    if (!is_stdin) close(fd);
    f->data = buf;
    f->len = len;
}

/* ---- line table ---- */

static uint32_t line_hash(const char *s, int len, int noeol)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (s_iws && isspace(c)) continue;
        if (s_icase) c = (unsigned char)tolower(c);
        h = (h ^ c) * 16777619u;
    }
    if (noeol) h = (h ^ 1u) * 16777619u;
    return h;
}

static void split_lines(file_t *f)
{
    f->ln = malloc((size_t)MAX_LINES * sizeof(line_t));
    if (!f->ln) too_large();
    size_t pos = 0;
    while (pos < f->len) {
        if (f->n >= MAX_LINES) too_large();
        const char *start = f->data + pos;
        const char *nl = memchr(start, '\n', f->len - pos);
        size_t ll = nl ? (size_t)(nl - start) : f->len - pos;
        line_t *l = &f->ln[f->n++];
        l->ptr = start;
        l->len = (int)ll;
        l->noeol = 0;
        if (!nl) {
            l->noeol = 1;
            f->noeol = 1;
        }
        l->hash = line_hash(l->ptr, l->len, l->noeol);
        pos += ll + (nl ? 1 : 0);
    }
}

/* Hash first; memcmp (normalized walk under -i/-w) to confirm. */
static int line_eq(const line_t *x, const line_t *y)
{
    if (x->hash != y->hash || x->noeol != y->noeol)
        return 0;
    if (!s_iws && !s_icase)
        return x->len == y->len && memcmp(x->ptr, y->ptr, (size_t)x->len) == 0;
    int i = 0, j = 0;
    for (;;) {
        if (s_iws) {
            while (i < x->len && isspace((unsigned char)x->ptr[i])) i++;
            while (j < y->len && isspace((unsigned char)y->ptr[j])) j++;
        }
        if (i >= x->len || j >= y->len) break;
        unsigned char a = (unsigned char)x->ptr[i];
        unsigned char b = (unsigned char)y->ptr[j];
        if (s_icase) {
            a = (unsigned char)tolower(a);
            b = (unsigned char)tolower(b);
        }
        if (a != b) return 0;
        i++;
        j++;
    }
    return i >= x->len && j >= y->len;
}

/* ---- Myers O(ND), forward, V snapshot per D for the traceback ---- */

static int myers_run(const line_t *a, int N, const line_t *b, int M)
{
    int dmax = N + M;
    if (dmax > MAX_D) dmax = MAX_D;
    int off = dmax + 1;
    int *work = calloc((size_t)(2 * dmax + 3), sizeof(int));
    if (!work) too_large();

    for (int d = 0; d <= dmax; d++) {
        for (int k = -d; k <= d; k += 2) {
            int x;
            if (k == -d || (k != d && work[off + k - 1] < work[off + k + 1]))
                x = work[off + k + 1];           /* down: insertion */
            else
                x = work[off + k - 1] + 1;       /* right: deletion */
            int y = x - k;
            while (x < N && y < M && line_eq(&a[x], &b[y])) {
                x++;
                y++;
            }
            work[off + k] = x;
            if (x >= N && y >= M) {
                free(work);
                return d;
            }
        }
        if (d == dmax)
            break;                               /* D cap exceeded */
        size_t bytes = (size_t)(2 * d + 1) * sizeof(int);
        if (s_snap_bytes + bytes > MAX_SNAP_BYTES) too_large();
        s_snap[d] = malloc(bytes);
        if (!s_snap[d]) too_large();
        memcpy(s_snap[d], &work[off - d], bytes);
        s_snap_bytes += bytes;
    }
    too_large();
    return -1; /* unreached */
}

/* Walk back from (N,M) through the per-D snapshots, flagging each
 * deletion in s_ach[] and each insertion in s_bch[]. p = common-prefix
 * length stripped before myers_run (sub-coords -> file coords). */
static void mark_changes(int p, int N, int M, int D)
{
    int x = N, y = M;
    for (int d = D; d > 0; d--) {
        const int *V = s_snap[d - 1] + (d - 1);  /* V[k], |k| <= d-1 */
        int k = x - y;
        int prev_k;
        if (k == -d || (k != d && V[k - 1] < V[k + 1]))
            prev_k = k + 1;
        else
            prev_k = k - 1;
        int px = V[prev_k];
        int py = px - prev_k;
        if (prev_k == k + 1)
            s_bch[p + py] = 1;                   /* b[py] inserted */
        else
            s_ach[p + px] = 1;                   /* a[px] deleted */
        x = px;
        y = py;
    }
}

/* ---- output ---- */

static void put_line(const char *pfx, const file_t *f, int i)
{
    fputs(pfx, stdout);
    fwrite(f->ln[i].ptr, 1, (size_t)f->ln[i].len, stdout);
    putchar('\n');
    if (f->noeol && i == f->n - 1)
        fputs("\\ No newline at end of file\n", stdout);
}

static void print_range(int lo, int hi)      /* 1-based inclusive */
{
    if (lo == hi)
        printf("%d", lo);
    else
        printf("%d,%d", lo, hi);
}

static void emit_normal(void)
{
    int n = s_a.n, m = s_b.n;
    int i = 0, j = 0;
    while (i < n || j < m) {
        if (i < n && j < m && !s_ach[i] && !s_bch[j]) {
            i++;
            j++;
            continue;
        }
        int i0 = i, j0 = j;
        while (i < n && s_ach[i]) i++;
        while (j < m && s_bch[j]) j++;
        int na = i - i0, nb = j - j0;
        if (na == 0 && nb == 0)
            break;                               /* defensive */
        if (na && nb) {
            print_range(i0 + 1, i);
            putchar('c');
            print_range(j0 + 1, j);
        } else if (na) {
            print_range(i0 + 1, i);
            printf("d%d", j0);
        } else {
            printf("%da", i0);
            print_range(j0 + 1, j);
        }
        putchar('\n');
        for (int k = i0; k < i; k++) put_line("< ", &s_a, k);
        if (na && nb) fputs("---\n", stdout);
        for (int k = j0; k < j; k++) put_line("> ", &s_b, k);
    }
}

static int collect_blocks(blk_t *blk)
{
    int n = s_a.n, m = s_b.n;
    int i = 0, j = 0, nblk = 0;
    while (i < n || j < m) {
        if (i < n && j < m && !s_ach[i] && !s_bch[j]) {
            i++;
            j++;
            continue;
        }
        blk_t *bl = &blk[nblk];
        bl->as = i;
        bl->bs = j;
        while (i < n && s_ach[i]) i++;
        while (j < m && s_bch[j]) j++;
        bl->na = i - bl->as;
        bl->nb = j - bl->bs;
        if (bl->na == 0 && bl->nb == 0)
            break;                               /* defensive */
        nblk++;
    }
    return nblk;
}

/* unified @@ range: start is 1-based unless count == 0 (then it's the
 * line before, GNU style); ",count" omitted when count == 1. */
static void print_urange(int start0, int count)
{
    if (count == 1)
        printf("%d", start0 + 1);
    else
        printf("%d,%d", count ? start0 + 1 : start0, count);
}

static void emit_unified(void)
{
    blk_t *blk = malloc((size_t)(s_a.n + s_b.n + 1) * sizeof(blk_t));
    if (!blk) too_large();
    int nblk = collect_blocks(blk);
    if (nblk == 0) return;

    printf("--- %s\n+++ %s\n", s_a.name, s_b.name);
    int g = 0;
    while (g < nblk) {
        /* merge blocks whose 3-line contexts would touch or overlap */
        int e = g;
        while (e + 1 < nblk &&
               blk[e + 1].as - (blk[e].as + blk[e].na) <= 2 * CTX)
            e++;
        int hs = blk[g].as - CTX;
        if (hs < 0) hs = 0;
        int he = blk[e].as + blk[e].na + CTX;
        if (he > s_a.n) he = s_a.n;
        int hbs = blk[g].bs - (blk[g].as - hs);
        int hbe = blk[e].bs + blk[e].nb + (he - (blk[e].as + blk[e].na));

        fputs("@@ -", stdout);
        print_urange(hs, he - hs);
        fputs(" +", stdout);
        print_urange(hbs, hbe - hbs);
        fputs(" @@\n", stdout);

        int i = hs;
        for (int x = g; x <= e; x++) {
            while (i < blk[x].as) {
                put_line(" ", &s_a, i);
                i++;
            }
            for (int k = 0; k < blk[x].na; k++)
                put_line("-", &s_a, blk[x].as + k);
            for (int k = 0; k < blk[x].nb; k++)
                put_line("+", &s_b, blk[x].bs + k);
            i = blk[x].as + blk[x].na;
        }
        while (i < he) {
            put_line(" ", &s_a, i);
            i++;
        }
        g = e + 1;
    }
}

/* ---- main ---- */

int main(int argc, char **argv)
{
    const char *files[2];
    int nf = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '-' && a[1] != '\0') {
            for (int c = 1; a[c]; c++) {
                switch (a[c]) {
                case 'q': s_quiet = 1; break;
                case 'i': s_icase = 1; break;
                case 'w': s_iws = 1; break;
                case 'u': s_unified = 1; break;
                default: usage();
                }
            }
        } else {
            if (nf >= 2) usage();
            files[nf++] = a;
        }
    }
    if (nf != 2) usage();
    if (strcmp(files[0], "-") == 0 && strcmp(files[1], "-") == 0) {
        fprintf(stderr, "diff: only one file argument may be -\n");
        return 2;
    }
    s_a.name = files[0];
    s_b.name = files[1];
    read_all(&s_a);
    read_all(&s_b);

    /* binary: NUL in the first 4096 bytes of either side */
    size_t pa = s_a.len < BIN_PROBE ? s_a.len : BIN_PROBE;
    size_t pb = s_b.len < BIN_PROBE ? s_b.len : BIN_PROBE;
    if ((pa && memchr(s_a.data, 0, pa)) || (pb && memchr(s_b.data, 0, pb))) {
        if (s_a.len == s_b.len &&
            (s_a.len == 0 || memcmp(s_a.data, s_b.data, s_a.len) == 0))
            return 0;
        if (s_quiet)
            printf("Files %s and %s differ\n", s_a.name, s_b.name);
        else
            printf("Binary files %s and %s differ\n", s_a.name, s_b.name);
        return 1;
    }

    split_lines(&s_a);
    split_lines(&s_b);

    if (s_quiet) {
        int same = (s_a.n == s_b.n);
        for (int i = 0; same && i < s_a.n; i++)
            if (!line_eq(&s_a.ln[i], &s_b.ln[i]))
                same = 0;
        if (same) return 0;
        printf("Files %s and %s differ\n", s_a.name, s_b.name);
        return 1;
    }

    /* strip common prefix/suffix; Myers runs on the middle only */
    int n = s_a.n, m = s_b.n;
    int p = 0;
    while (p < n && p < m && line_eq(&s_a.ln[p], &s_b.ln[p]))
        p++;
    int s = 0;
    while (s < n - p && s < m - p &&
           line_eq(&s_a.ln[n - 1 - s], &s_b.ln[m - 1 - s]))
        s++;
    int N = n - p - s, M = m - p - s;
    if (N == 0 && M == 0)
        return 0;                                /* identical */

    int D = myers_run(s_a.ln + p, N, s_b.ln + p, M);

    s_ach = calloc((size_t)n + 1, 1);
    s_bch = calloc((size_t)m + 1, 1);
    if (!s_ach || !s_bch) too_large();
    mark_changes(p, N, M, D);

    if (s_unified)
        emit_unified();
    else
        emit_normal();
    return 1;
}
