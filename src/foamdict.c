/* foamdict.c -- span-preserving OpenFOAM dictionary parser */
#define _GNU_SOURCE
#include "foamdict.h"

#include <sys/stat.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================================ tokenizer */
typedef enum { T_EOF, T_PUNCT, T_WORD, T_STRING, T_VERBATIM, T_DIRECTIVE } TokType;
typedef struct { TokType t; int a, b; char c; } Tok;

typedef struct {
    const char *s;
    int n;
    int errPos;
    char err[160];
} Lexer;

static int skip_ws(const Lexer *L, int p)
{
    for (;;) {
        while (p < L->n && isspace((unsigned char)L->s[p])) p++;
        if (p + 1 < L->n && L->s[p] == '/' && L->s[p + 1] == '/') {
            while (p < L->n && L->s[p] != '\n') p++;
            continue;
        }
        if (p + 1 < L->n && L->s[p] == '/' && L->s[p + 1] == '*') {
            const char *e = strstr(L->s + p + 2, "*/");
            p = e ? (int)(e - L->s) + 2 : L->n;
            continue;
        }
        return p;
    }
}

static bool is_delim(char c) { return isspace((unsigned char)c) || c == ';' || c == '{' || c == '}' || c == '"'; }

/* read one token starting at p (p must be at a non-space char) */
static Tok lex(const Lexer *L, int p)
{
    Tok t = { T_EOF, p, p, 0 };
    if (p >= L->n) return t;
    char c = L->s[p];
    if (c == '#' && p + 1 < L->n && L->s[p + 1] == '{') {
        const char *e = strstr(L->s + p + 2, "#}");
        t.t = T_VERBATIM; t.b = e ? (int)(e - L->s) + 2 : L->n;
        return t;
    }
    if (c == '$' && p + 1 < L->n && L->s[p + 1] == '{') {      /* ${scoped/macro}, may nest: ${_${FOAM_EXECUTABLE}} */
        int q = p + 1, d = 0;
        for (; q < L->n; q++) { if (L->s[q] == '{') d++; else if (L->s[q] == '}' && --d == 0) { q++; break; } }
        t.t = T_WORD; t.b = q;
        return t;
    }
    if (strchr("{};()[]", c)) { t.t = T_PUNCT; t.c = c; t.b = p + 1; return t; }
    if (c == '"') {
        int q = p + 1;
        while (q < L->n && L->s[q] != '"') { if (L->s[q] == '\\' && q + 1 < L->n) q++; q++; }
        t.t = T_STRING; t.b = q < L->n ? q + 1 : L->n;
        return t;
    }
    /* word: may contain balanced parentheses, e.g. div(phi,U) or List<scalar> */
    int q = p, depth = 0;
    while (q < L->n) {
        char d = L->s[q];
        if (is_delim(d)) break;
        if (d == '(') depth++;
        else if (d == ')') { if (depth == 0) break; depth--; }
        else if ((d == '[' || d == ']') && depth == 0 && q > p) break;
        q++;
    }
    if (q == p) q = p + 1;
    t.t = (c == '#') ? T_DIRECTIVE : T_WORD;
    t.b = q;
    return t;
}

/* ============================================================================ tree building */
static void add_child(FDEntry *parent, const FDEntry *c)
{
    if (parent->nChild == parent->capChild) {
        parent->capChild = parent->capChild ? parent->capChild * 2 : 8;
        parent->child = realloc(parent->child, sizeof(FDEntry) * (size_t)parent->capChild);
    }
    parent->child[parent->nChild++] = *c;
}

static void free_entry(FDEntry *e)
{
    for (int i = 0; i < e->nChild; i++) free_entry(&e->child[i]);
    free(e->child);
    free(e->key);
    memset(e, 0, sizeof *e);
}

static int line_of(const Lexer *L, int p)
{
    int ln = 1;
    for (int i = 0; i < p && i < L->n; i++) if (L->s[i] == '\n') ln++;
    return ln;
}

static int parse_entries(Lexer *L, int p, char endc, FDEntry *parent, int depthGuard);
static int parse_top_list(Lexer *L, Tok open, FDEntry *parent, int depthGuard);

/* scan value tokens from p until ';' at bracket depth 0, or an unmatched closer.
   returns position of ';' (or of the closer / EOF); *lastEnd = end of last value token */
static int scan_value(Lexer *L, int p, int *firstStart, int *lastEnd)
{
    int depth = 0;
    *firstStart = -1; *lastEnd = p;
    for (;;) {
        p = skip_ws(L, p);
        Tok t = lex(L, p);
        if (t.t == T_EOF) return L->n;
        if (t.t == T_PUNCT) {
            if (t.c == ';' && depth == 0) return t.a;
            if (t.c == '(' || t.c == '{' || t.c == '[') depth++;
            else if (t.c == ')' || t.c == '}' || t.c == ']') {
                if (depth == 0) return t.a;                 /* missing ';' before a closer */
                depth--;
            }
        } else if (t.t == T_WORD) {
            /* a word can open a bracket that a later token closes: "2(wall viewFactorWall)",
               "round($x / $dx)" inside #eval{ }, "1((100 0))"                              */
            for (int k = t.a; k < t.b; k++) {
                if (L->s[k] == '$' && k + 1 < t.b && L->s[k + 1] == '{') break;   /* ${...} is balanced */
                if (L->s[k] == '(') depth++;
                else if (L->s[k] == ')' && depth > 0) depth--;
            }
        }
        if (*firstStart < 0) *firstStart = t.a;
        *lastEnd = t.b;
        p = t.b;
    }
}

/* if the value is "( ... )" try to read "name { ... }" items inside it */
static void parse_list_dicts(Lexer *L, FDEntry *e, int depthGuard)
{
    if (e->valEnd - e->valStart < 2 || L->s[e->valStart] != '(') return;
    int p = e->valStart + 1;
    p = skip_ws(L, p);
    Tok t = lex(L, p);
    if (t.t != T_WORD && t.t != T_STRING) return;
    int q = skip_ws(L, t.b);
    Tok t2 = lex(L, q);
    if (!(t2.t == T_PUNCT && t2.c == '{')) return;
    Lexer sub = *L;
    sub.n = e->valEnd - 1;                                  /* stop before the final ')' */
    FDEntry tmp; memset(&tmp, 0, sizeof tmp);
    parse_entries(&sub, e->valStart + 1, 0, &tmp, depthGuard + 1);
    bool allDicts = tmp.nChild > 0;
    for (int i = 0; i < tmp.nChild; i++) if (tmp.child[i].kind != FD_DICT) allDicts = false;
    if (allDicts) { e->child = tmp.child; e->nChild = tmp.nChild; e->capChild = tmp.capChild; tmp.child = NULL; tmp.nChild = 0; }
    free_entry(&tmp);
}

/* top-level list such as polyMesh/boundary:  N ( name { ... } ... )  -> items become root children */
static int parse_top_list(Lexer *L, Tok open, FDEntry *parent, int depthGuard)
{
    int close = L->n, d = 1, q = open.b;
    while (q < L->n) {
        q = skip_ws(L, q);
        if (q >= L->n) break;
        Tok u = lex(L, q);
        if (u.t == T_PUNCT && u.c == '(') d++;
        if (u.t == T_PUNCT && u.c == ')' && --d == 0) { close = u.a; break; }
        if (u.t == T_WORD)                                  /* "2(sideWalls" opens a bracket that ")" closes (bug hunt H-04) */
            for (int k = u.a; k < u.b; k++) { if (L->s[k] == '(') d++; else if (L->s[k] == ')') d--; }
        q = u.b;
    }
    /* only lists of "name { ... }" items hold entries (polyMesh/boundary); point lists, injection
       tables and the like are plain data */
    int f0 = skip_ws(L, open.b);
    Tok a = lex(L, f0), b = lex(L, skip_ws(L, a.b));
    if (!((a.t == T_WORD || a.t == T_STRING) && b.t == T_PUNCT && b.c == '{')) return close < L->n ? close + 1 : L->n;
    Lexer sub = *L; sub.n = close;
    parse_entries(&sub, open.b, 0, parent, depthGuard + 1);
    if (sub.err[0] && !L->err[0]) { memcpy(L->err, sub.err, sizeof L->err); L->errPos = sub.errPos; }
    return close < L->n ? close + 1 : L->n;
}

static int parse_entries(Lexer *L, int p, char endc, FDEntry *parent, int depthGuard)
{
    if (depthGuard > 64) { snprintf(L->err, sizeof L->err, "nesting too deep"); L->errPos = p; return L->n; }
    for (;;) {
        p = skip_ws(L, p);
        if (p >= L->n) {
            if (endc && !L->err[0]) { snprintf(L->err, sizeof L->err, "missing '%c'", endc); L->errPos = p; }
            return L->n;
        }
        Tok t = lex(L, p);
        if (t.t == T_PUNCT) {
            if (t.c == endc) return t.a;
            if (t.c == ';') { p = t.b; continue; }
            if (t.c == '(' && endc == 0) { p = parse_top_list(L, t, parent, depthGuard); continue; }
            if (t.c == '}' || t.c == ')') {
                if (!L->err[0]) { snprintf(L->err, sizeof L->err, "unexpected '%c'", t.c); L->errPos = t.a; }
                p = t.b;
                continue;
            }
            p = t.b;                                         /* stray bracket: skip */
            continue;
        }
        FDEntry e; memset(&e, 0, sizeof e);
        e.start = t.a; e.keyStart = t.a; e.keyEnd = t.b;
        e.key = pl_strndup(L->s + t.a, (size_t)(t.b - t.a));
        e.line = 0;

        if (t.t == T_DIRECTIVE) {
            /* #include "file", #includeEtc ..., #remove (a b), #inputMode merge ...: rest of the line */
            e.kind = FD_DIRECTIVE;
            /* rest of the line, plus any "( ... )" argument list, which may span lines:
               #includeFunc streamlinesLine
               ( name=streamlines, start=(0 0 0), ... )                                   */
            int q = t.b, depth = 0;
            for (;;) {
                while (q < L->n && (L->s[q] != '\n' || depth > 0)) {
                    char ch = L->s[q];
                    if (ch == '/' && q + 1 < L->n && L->s[q + 1] == '/' && depth == 0) break;
                    if (ch == '"') { q++; while (q < L->n && L->s[q] != '"') q++; }
                    else if (ch == '(') depth++;
                    else if (ch == ')' && depth > 0) depth--;
                    q++;
                }
                if (q > L->n) q = L->n;                      /* an unterminated "string" ran to the end of the text */
                int nx = skip_ws(L, q);
                bool takesArgs = strncmp(L->s + t.a, "#includeFunc", 12) == 0 || strncmp(L->s + t.a, "#includeModel", 13) == 0;
                if (takesArgs && depth == 0 && nx < L->n && L->s[nx] == '(') {
                    bool argsOnLine = false;               /* only if the call had no args yet */
                    for (int k = t.b; k < q; k++) if (L->s[k] == '(') argsOnLine = true;
                    if (!argsOnLine) { q = nx; continue; }
                }
                if (depth == 0 && nx < L->n && L->s[nx] == '{') {
                    /* "#codeStream { code #{ ... #}; };" or "#word \"name\" { ... }" in keyword position */
                    int d = 0, k = nx;
                    while (k < L->n) {
                        k = skip_ws(L, k);
                        Tok u = lex(L, k);
                        if (u.t == T_EOF) break;
                        if (u.t == T_PUNCT && u.c == '{') d++;
                        if (u.t == T_PUNCT && u.c == '}' && --d == 0) { k = u.b; break; }
                        k = u.b;
                    }
                    q = k;
                    int sc = skip_ws(L, q);
                    if (sc < L->n && L->s[sc] == ';') q = sc + 1;
                }
                break;
            }
            int v0 = skip_ws(L, t.b);
            if (v0 > q) v0 = q;
            e.valStart = v0;
            int v1 = q;
            while (v1 > v0 && isspace((unsigned char)L->s[v1 - 1])) v1--;
            e.valEnd = v1;
            e.end = v1;
            add_child(parent, &e);
            p = q;
            continue;
        }
        if (t.t == T_VERBATIM) { e.kind = FD_DIRECTIVE; e.valStart = e.valEnd = t.b; e.end = t.b; add_child(parent, &e); p = t.b; continue; }
        if (t.t == T_WORD && L->s[t.a] == '$') {
            /* macro expansion in keyword position: "$p_GAMG", "${../vessel};" -- the ';' is optional */
            e.kind = FD_DIRECTIVE;
            e.valStart = e.valEnd = e.end = t.b;
            int q = skip_ws(L, t.b);
            if (q < L->n && L->s[q] == ';') e.end = q + 1;
            add_child(parent, &e);
            p = e.end;
            continue;
        }

        int q = skip_ws(L, t.b);
        Tok n2 = lex(L, q);
        if (endc == 0 && t.t == T_WORD && n2.t == T_PUNCT && n2.c == '(') {
            char num[32]; int kl = t.b - t.a < 31 ? t.b - t.a : 31;
            memcpy(num, L->s + t.a, (size_t)kl); num[kl] = 0;
            if (pl_is_number(num)) {                          /* "N ( ... )" top-level list */
                free(e.key);
                p = parse_top_list(L, n2, parent, depthGuard);
                continue;
            }
        }
        if (n2.t == T_PUNCT && n2.c == '{') {
            e.kind = FD_DICT;
            e.valStart = n2.b;
            int close = parse_entries(L, n2.b, '}', &e, depthGuard + 1);
            e.valEnd = close;
            e.end = close < L->n ? close + 1 : L->n;
            add_child(parent, &e);
            p = e.end;
            continue;
        }
        e.kind = FD_VALUE;
        int fs, le;
        int semi = scan_value(L, t.b, &fs, &le);
        if (fs < 0) { e.valStart = e.valEnd = t.b; }
        else { e.valStart = fs; e.valEnd = le; }
        if (semi < L->n && L->s[semi] == ';') e.end = semi + 1;
        else {
            e.end = e.valEnd;
            if (!L->err[0]) { snprintf(L->err, sizeof L->err, "missing ';' after '%.40s'", e.key); L->errPos = t.a; }
        }
        parse_list_dicts(L, &e, depthGuard);
        add_child(parent, &e);
        p = (semi < L->n && L->s[semi] == ';') ? semi + 1 : semi;
        if (p == t.a) p = t.b;                               /* guarantee progress */
    }
}

/* entries come in document order (pre-order), so one cursor walks the text once: linear even for
   files with tens of thousands of entries (point lists, kinematicCloudPositions) */
static void fill_lines(const char *s, FDEntry *e, int *pos, int *ln)
{
    for (int i = 0; i < e->nChild; i++) {
        int st = e->child[i].start;
        if (st < *pos) { *pos = 0; *ln = 1; }               /* out of order: restart (never expected) */
        for (; *pos < st; (*pos)++) if (s[*pos] == '\n') (*ln)++;
        e->child[i].line = *ln;
        fill_lines(s, &e->child[i], pos, ln);
    }
}

static void reparse(FDFile *f)
{
    free_entry(&f->root);
    Lexer L = { f->text, f->len, -1, "" };
    f->root.kind = FD_DICT;
    f->root.valStart = 0; f->root.valEnd = f->len;
    f->root.start = 0; f->root.end = f->len;
    parse_entries(&L, 0, 0, &f->root, 0);
    f->ok = L.err[0] == 0;
    if (!f->ok) snprintf(f->err, sizeof f->err, "line %d: %s", line_of(&L, L.errPos), L.err);
    else f->err[0] = 0;
    int pos = 0, ln = 1;
    fill_lines(f->text, &f->root, &pos, &ln);
}

/* ============================================================================ file API */
FDFile *fd_from_text(const char *path, const char *text)
{
    FDFile *f = calloc(1, sizeof *f);
    if (!f) return NULL;
    pl_strlcpy(f->path, path ? path : "", sizeof f->path);
    f->text = pl_strdup(text ? text : "");
    f->len = (int)strlen(f->text);
    f->base = pl_strdup(f->text);
    if (path && *path) { fd_sig(path, &f->sig); f->mtime = (time_t)f->sig.sec; }
    reparse(f);
    return f;
}

FDFile *fd_load(const char *path)
{
    size_t n;
    char *t = pl_read_file(path, &n);
    if (!t) return NULL;
    FDFile *f = calloc(1, sizeof *f);
    if (!f) { free(t); return NULL; }
    pl_strlcpy(f->path, path, sizeof f->path);
    f->text = t;
    f->len = (int)strlen(t);                                 /* stop at an embedded NUL */
    f->binary = (size_t)f->len != n;
    f->mtime = pl_mtime(path);
    fd_sig(path, &f->sig);
    f->base = pl_strdup(f->text);
    reparse(f);
    return f;
}

static void clear_conflict(FDFile *f)
{
    free(f->diskText); f->diskText = NULL;
    f->conflict = false;
}

void fd_free(FDFile *f)
{
    if (!f) return;
    free_entry(&f->root);
    free(f->text);
    free(f->base);
    free(f->diskText);
    free(f);
}

bool fd_sig(const char *path, FDSig *out)
{
    struct stat st;
    memset(out, 0, sizeof *out);
    if (stat(path, &st) != 0) return false;
    *out = (FDSig){ st.st_mtim.tv_sec, st.st_mtim.tv_nsec, st.st_size, (long long)st.st_ino };
    return true;
}

static bool sig_eq(const FDSig *a, const FDSig *b) { return !memcmp(a, b, sizeof *a); }

/* the file now holds text t (malloc'd, taken over), which is also what is on disk */
static void set_disk_text(FDFile *f, char *t, size_t n, const FDSig *sig)
{
    free(f->text);
    f->text = t;
    f->len = (int)strlen(t);
    f->binary = (size_t)f->len != n;
    free(f->base);
    f->base = pl_strdup(t);
    f->sig = *sig;
    f->mtime = (time_t)sig->sec;
    f->dirty = false;
    clear_conflict(f);
    reparse(f);
}

bool fd_reload(FDFile *f)
{
    size_t n;
    FDSig sig; fd_sig(f->path, &sig);
    char *t = pl_read_file(f->path, &n);
    if (!t) return false;
    set_disk_text(f, t, n, &sig);
    return true;
}

bool fd_save(FDFile *f)
{
    if (f->binary) return false;                             /* would cut the file at the first NUL */
    if (f->conflict) return false;                           /* the user has to choose first */
    if (fd_changed_on_disk(f)) {                            /* someone else wrote it since: take their edits in */
        int r = fd_sync(f);
        if (r == FD_SYNC_CONFLICT) return false;
        if (!f->dirty) return true;                          /* nothing of ours left to write */
    }
    if (!pl_write_file(f->path, f->text, (size_t)f->len)) return false;
    fd_sig(f->path, &f->sig);
    f->mtime = (time_t)f->sig.sec;
    free(f->base);
    f->base = pl_strdup(f->text);
    f->dirty = false;
    f->gone = false;
    return true;
}

bool fd_save_as(const FDFile *f, const char *path)
{
    if (f->binary) return false;
    return pl_write_file(path, f->text, (size_t)f->len);
}

bool fd_changed_on_disk(const FDFile *f)
{
    FDSig s;
    if (!fd_sig(f->path, &s)) return !f->gone;                /* deleted outside: a change, once (bug hunt H-17) */
    if (f->conflict) return !sig_eq(&s, &f->diskSig);        /* already shown: only a newer disk version counts */
    return !sig_eq(&s, &f->sig);
}

/* ---------------------------------------------------------------- rebase
   The user's edits are the entries whose value differs between the base (the disk version the edits started from)
   and the text in memory, plus entries added and removed. They are replayed onto the new disk version with the same
   span edits the app uses. The replay is first checked on the base: it must give exactly the text in memory, else
   the edit holds something that isn't an entry edit (a comment, layout, a raw text edit) and it is a conflict. */
typedef struct { char *path; char *val; FDKind kind; } Leaf;
typedef struct { Leaf *v; int n, cap; } Leaves;

static void leaves_add(Leaves *L, const char *path, const char *val, FDKind k)
{
    if (L->n == L->cap) { L->cap = L->cap ? L->cap * 2 : 64; L->v = realloc(L->v, sizeof(Leaf) * (size_t)L->cap); }
    L->v[L->n++] = (Leaf){ pl_strdup(path), pl_strdup(val), k };
}

static void leaves_of(const FDFile *f, const FDEntry *d, const char *prefix, Leaves *L)
{
    for (int i = 0; i < d->nChild; i++) {
        const FDEntry *e = &d->child[i];
        if (e->kind == FD_DIRECTIVE) continue;               /* not replayable: the verification catches changes to them */
        char path[1024];
        snprintf(path, sizeof path, "%s%s%s", prefix, prefix[0] ? "/" : "", e->key);
        if (e->kind == FD_DICT) { leaves_add(L, path, "", FD_DICT); leaves_of(f, e, path, L); continue; }
        int n = e->valEnd - e->valStart;
        char *v = malloc((size_t)(n > 0 ? n : 0) + 1);
        if (n > 0) memcpy(v, f->text + e->valStart, (size_t)n);
        v[n > 0 ? n : 0] = 0;
        leaves_add(L, path, v, FD_VALUE);
        free(v);
    }
}

static void leaves_free(Leaves *L)
{
    for (int i = 0; i < L->n; i++) { free(L->v[i].path); free(L->v[i].val); }
    free(L->v);
    memset(L, 0, sizeof *L);
}

static const Leaf *leaf_find(const Leaves *L, const char *path)
{
    for (int i = 0; i < L->n; i++) if (!strcmp(L->v[i].path, path)) return &L->v[i];
    return NULL;
}

typedef enum { OP_SET, OP_ADD, OP_DEL } OpKind;
typedef struct { OpKind k; const Leaf *mine; const Leaf *base; } Op;

static bool apply_ops(FDFile *t, const Op *ops, int n)
{
    for (int i = 0; i < n; i++) {
        const Op *o = &ops[i];
        if (o->k == OP_DEL) { if (fd_find(t, o->base->path)) fd_remove(t, o->base->path); continue; }
        if (o->mine->kind == FD_DICT) { if (!fd_ensure_dict(t, o->mine->path)) return false; continue; }
        FDEntry *e = fd_find(t, o->mine->path);
        bool ok = e ? fd_put_entry(t, e, o->mine->val) : fd_put(t, o->mine->path, o->mine->val);
        if (!ok) return false;
    }
    return true;
}

/* 0 rebased (f->text = disk + the user's edits), -1 conflict (f unchanged) */
static int rebase(FDFile *f, const char *disk)
{
    if (!f->base || f->binary) return -1;
    FDFile *B = fd_from_text(f->path, f->base), *D = fd_from_text(f->path, disk);
    Leaves LB = { 0 }, LC = { 0 }, LD = { 0 };
    leaves_of(B, &B->root, "", &LB);
    leaves_of(f, &f->root, "", &LC);
    leaves_of(D, &D->root, "", &LD);
    Op *ops = malloc(sizeof(Op) * (size_t)(LB.n + LC.n + 1));
    int nOps = 0, rc = 0;
    for (int i = 0; i < LC.n; i++) {
        const Leaf *b = leaf_find(&LB, LC.v[i].path);
        if (!b) ops[nOps++] = (Op){ OP_ADD, &LC.v[i], NULL };
        else if (b->kind != LC.v[i].kind || strcmp(b->val, LC.v[i].val)) ops[nOps++] = (Op){ OP_SET, &LC.v[i], b };
    }
    for (int i = 0; i < LB.n; i++) if (!leaf_find(&LC, LB.v[i].path)) ops[nOps++] = (Op){ OP_DEL, NULL, &LB.v[i] };
    /* the replay must reproduce the user's text exactly from the base */
    FDFile *T = fd_from_text(f->path, f->base);
    if (!apply_ops(T, ops, nOps) || strcmp(T->text, f->text)) rc = -1;
    /* the disk may not have changed what the user changed, unless to the same value */
    for (int i = 0; i < nOps && rc == 0; i++) {
        const Op *o = &ops[i];
        const char *path = o->k == OP_DEL ? o->base->path : o->mine->path;
        const Leaf *d = leaf_find(&LD, path);
        if (o->k == OP_ADD) { if (d && strcmp(d->val, o->mine->val)) rc = -1; continue; }
        if (!d) { if (o->k == OP_SET) rc = -1; continue; }   /* deleted on disk, changed here */
        bool diskSame = d->kind == o->base->kind && !strcmp(d->val, o->base->val);
        bool diskMine = o->k == OP_SET && d->kind == o->mine->kind && !strcmp(d->val, o->mine->val);
        if (!diskSame && !diskMine) rc = -1;
    }
    if (rc == 0) {
        FDFile *M = fd_from_text(f->path, disk);
        if (apply_ops(M, ops, nOps)) {
            free(f->text); f->text = M->text; M->text = NULL; f->len = (int)strlen(f->text);
            reparse(f);
        } else rc = -1;
        fd_free(M);
    }
    fd_free(T); free(ops);
    leaves_free(&LB); leaves_free(&LC); leaves_free(&LD);
    fd_free(B); fd_free(D);
    return rc;
}

int fd_sync(FDFile *f)
{
    FDSig s;
    if (!fd_sig(f->path, &s)) return FD_SYNC_GONE;
    if (sig_eq(&s, &f->sig) || (f->conflict && sig_eq(&s, &f->diskSig))) return FD_SYNC_NONE;
    size_t n;
    char *disk = pl_read_file(f->path, &n);
    if (!disk) return FD_SYNC_GONE;
    if (f->base && !strcmp(disk, f->base) && strlen(disk) == n) {   /* touched, same content */
        f->sig = s; f->mtime = (time_t)s.sec; free(disk);
        return FD_SYNC_NONE;
    }
    if (!f->dirty) { set_disk_text(f, disk, n, &s); return FD_SYNC_RELOADED; }
    if (strlen(disk) == n && rebase(f, disk) == 0) {
        free(f->base); f->base = disk;
        f->sig = s; f->mtime = (time_t)s.sec;
        clear_conflict(f);
        f->dirty = strcmp(f->text, f->base) != 0;
        return FD_SYNC_REBASED;
    }
    free(f->diskText);
    f->diskText = disk;
    f->diskSig = s;
    f->conflict = true;
    return FD_SYNC_CONFLICT;
}

void fd_resolve(FDFile *f, bool keepMine)
{
    if (!f->conflict) return;
    if (!keepMine) {                                         /* take the disk version */
        char *t = f->diskText; f->diskText = NULL;
        FDSig s = f->diskSig;
        set_disk_text(f, t, strlen(t), &s);
        return;
    }
    free(f->base);                                           /* mine on top of what is on disk now */
    f->base = f->diskText; f->diskText = NULL;
    f->sig = f->diskSig; f->mtime = (time_t)f->sig.sec;
    f->conflict = false;
    f->dirty = true;
}

void (*fd_edit_hook)(FDFile *f, const char *before);
unsigned long fd_edit_count;

void fd_set_text(FDFile *f, const char *text)
{
    if (f->binary) return;
    char *before = f->text;
    f->text = pl_strdup(text);
    f->len = (int)strlen(f->text);
    f->dirty = true;
    reparse(f);
    fd_edit_count++;
    if (fd_edit_hook) fd_edit_hook(f, before);
    free(before);
}

bool fd_replace_span(FDFile *f, int a, int b, const char *text)
{
    if (f->binary || a < 0 || b < a || b > f->len) return false;
    size_t tl = strlen(text);
    size_t nl = (size_t)f->len - (size_t)(b - a) + tl;
    char *nt = malloc(nl + 1);
    if (!nt) return false;
    memcpy(nt, f->text, (size_t)a);
    memcpy(nt + a, text, tl);
    memcpy(nt + a + tl, f->text + b, (size_t)(f->len - b));
    nt[nl] = 0;
    char *before = f->text;
    f->text = nt;
    f->len = (int)nl;
    f->dirty = true;
    reparse(f);
    fd_edit_count++;
    if (fd_edit_hook) fd_edit_hook(f, before);
    free(before);
    return true;
}

/* ============================================================================ lookup */
FDEntry *fd_child(FDEntry *d, const char *key)
{
    if (!d) return NULL;
    for (int i = d->nChild - 1; i >= 0; i--)                 /* last definition wins, like OpenFOAM */
        if (d->child[i].kind != FD_DIRECTIVE && strcmp(d->child[i].key, key) == 0) return &d->child[i];
    return NULL;
}

/* OpenFOAM's rules: the last definition of a keyword wins, and dictionaries with the same keyword merge, so a path
   is looked up in the later dictionary first and then in the earlier ones (a/x found in an earlier "a { x 1; }") */
static FDEntry *find_in(FDEntry *d, const char *p)
{
    const char *sl = strchr(p, '/');
    size_t n = sl ? (size_t)(sl - p) : strlen(p);
    for (int i = d->nChild - 1; i >= 0; i--) {
        FDEntry *c = &d->child[i];
        if (c->kind == FD_DIRECTIVE || strlen(c->key) != n || strncmp(c->key, p, n)) continue;
        if (!sl) return c;
        FDEntry *r = find_in(c, sl + 1);
        if (r) return r;
    }
    return NULL;
}

FDEntry *fd_find(FDFile *f, const char *path)
{
    if (!f || !path || !*path) return NULL;
    return find_in(&f->root, path);
}

bool fd_entry_value(const FDFile *f, const FDEntry *e, char *buf, int cap)
{
    if (!e || e->kind == FD_DICT) { if (cap) buf[0] = 0; return false; }
    int n = e->valEnd - e->valStart;
    if (n < 0) n = 0;
    if (n >= cap) n = cap - 1;
    memcpy(buf, f->text + e->valStart, (size_t)n);
    buf[n] = 0;
    return true;
}

bool fd_get(FDFile *f, const char *path, char *buf, int cap)
{
    FDEntry *e = fd_find(f, path);
    if (!e || e->kind != FD_VALUE) { if (cap) buf[0] = 0; return false; }
    return fd_entry_value(f, e, buf, cap);
}

/* ============================================================================ editing */
static int line_start(const FDFile *f, int p) { while (p > 0 && f->text[p - 1] != '\n') p--; return p; }

static void indent_of(const FDFile *f, int p, char *out, int cap)
{
    int a = line_start(f, p), k = 0;
    while (a < f->len && (f->text[a] == ' ' || f->text[a] == '\t') && k < cap - 1) out[k++] = f->text[a++];
    out[k] = 0;
}

static void format_entry(char *out, size_t cap, const char *indent, const char *key, const char *value)
{
    int pad = 16 - (int)strlen(key);
    if (pad < 1) pad = 1;
    snprintf(out, cap, "%s%s%*s%s;\n", indent, key, pad, "", value);
}

/* insert a ready-made line(s) of text as the last item of dict d */
static bool insert_into(FDFile *f, FDEntry *d, const char *body)
{
    if (d == &f->root) {
        /* before the closing "// *****" banner if the file has one, else at the end */
        int at = f->len;
        const char *ban = NULL, *s = f->text;
        while ((s = strstr(s, "// ****"))) { ban = s; s++; }
        if (ban && ban > f->text + f->len / 2) at = line_start(f, (int)(ban - f->text));
        Str b; str_init(&b);
        if (at > 0 && f->text[at - 1] != '\n') str_cat(&b, "\n");
        str_cat(&b, body);
        if (at < f->len) str_cat(&b, "\n");
        bool ok = fd_replace_span(f, at, at, b.s);
        str_free(&b);
        return ok;
    }
    int close = d->valEnd;                                   /* position of '}' */
    int ls = line_start(f, close);
    bool onlyWs = true;
    for (int i = ls; i < close; i++) if (!isspace((unsigned char)f->text[i])) onlyWs = false;
    if (onlyWs) return fd_replace_span(f, ls, ls, body);
    char ind[64]; indent_of(f, d->start, ind, sizeof ind);
    Str b; str_init(&b);
    str_cat(&b, "\n"); str_cat(&b, body); str_cat(&b, ind);
    bool ok = fd_replace_span(f, close, close, b.s);
    str_free(&b);
    return ok;
}

static void child_indent(FDFile *f, FDEntry *d, char *ind, int cap)
{
    if (d->nChild > 0) { indent_of(f, d->child[d->nChild - 1].start, ind, cap); return; }
    if (d == &f->root) { ind[0] = 0; return; }
    indent_of(f, d->start, ind, cap);
    size_t n = strlen(ind);
    if (n + 4 < (size_t)cap) { memcpy(ind + n, "    ", 5); }
}

/* split "a/b/c" into parent path "a/b" and key "c" */
static void split_path(const char *path, char *parent, int pcap, char *key, int kcap)
{
    const char *sl = strrchr(path, '/');
    if (!sl) { parent[0] = 0; pl_strlcpy(key, path, (size_t)kcap); return; }
    int n = (int)(sl - path);
    if (n >= pcap) n = pcap - 1;
    memcpy(parent, path, (size_t)n); parent[n] = 0;
    pl_strlcpy(key, sl + 1, (size_t)kcap);
}

bool fd_ensure_dict(FDFile *f, const char *path)
{
    if (!*path) return true;
    FDEntry *e = fd_find(f, path);
    if (e) return e->kind == FD_DICT;
    char parent[512], key[256];
    split_path(path, parent, sizeof parent, key, sizeof key);
    if (!fd_ensure_dict(f, parent)) return false;
    FDEntry *pd = parent[0] ? fd_find(f, parent) : &f->root;
    if (!pd) return false;
    char ind[64]; child_indent(f, pd, ind, sizeof ind);
    char body[512];
    snprintf(body, sizeof body, "%s%s\n%s{\n%s}\n", ind, key, ind, ind);
    return insert_into(f, pd, body);
}

bool fd_put_entry(FDFile *f, FDEntry *e, const char *value)
{
    if (!e || e->kind != FD_VALUE) return false;
    if (e->valStart == e->valEnd) {                          /* "key;" -> "key value;" */
        if (!*value) return true;                            /* nothing to put: leave the bytes alone */
        char buf[1024]; snprintf(buf, sizeof buf, " %s", value);
        return fd_replace_span(f, e->keyEnd, e->keyEnd, buf);
    }
    return fd_replace_span(f, e->valStart, e->valEnd, value);
}

bool fd_put(FDFile *f, const char *path, const char *value)
{
    FDEntry *e = fd_find(f, path);
    if (e) return fd_put_entry(f, e, value);
    char parent[512], key[256];
    split_path(path, parent, sizeof parent, key, sizeof key);
    if (!fd_ensure_dict(f, parent)) return false;
    FDEntry *pd = parent[0] ? fd_find(f, parent) : &f->root;
    if (!pd) return false;
    char ind[64]; child_indent(f, pd, ind, sizeof ind);
    char body[2048];
    format_entry(body, sizeof body, ind, key, value);
    return insert_into(f, pd, body);
}

bool fd_remove(FDFile *f, const char *path)
{
    FDEntry *e = fd_find(f, path);
    if (!e) return false;
    int a = e->start, b = e->end;
    int ls = line_start(f, a);
    bool wsBefore = true;
    for (int i = ls; i < a; i++) if (!isspace((unsigned char)f->text[i])) wsBefore = false;
    int q = b;
    while (q < f->len && (f->text[q] == ' ' || f->text[q] == '\t')) q++;
    if (wsBefore && (q >= f->len || f->text[q] == '\n')) { a = ls; b = q < f->len ? q + 1 : q; }
    return fd_replace_span(f, a, b, "");
}

/* ============================================================================ value helpers */
bool fd_number_span(const char *v, int *a, int *b)
{
    int n = (int)strlen(v), found = 0, bestA = 0, bestB = 0;
    int depthSq = 0;
    for (int i = 0; i < n;) {
        char c = v[i];
        if (c == '[') { depthSq++; i++; continue; }
        if (c == ']') { depthSq--; i++; continue; }
        if (isspace((unsigned char)c) || c == '(' || c == ')') { i++; continue; }
        int j = i;
        while (j < n && !isspace((unsigned char)v[j]) && v[j] != '(' && v[j] != ')' && v[j] != '[' && v[j] != ']') j++;
        if (depthSq == 0) {
            char tmp[64];
            int len = j - i < 63 ? j - i : 63;
            memcpy(tmp, v + i, (size_t)len); tmp[len] = 0;
            if (pl_is_number(tmp)) { bestA = i; bestB = j; found = 1; }
        }
        i = j;
    }
    if (found) { *a = bestA; *b = bestB; }
    return found;
}

bool fd_word(const char *v, char *buf, int cap)
{
    while (*v && isspace((unsigned char)*v)) v++;
    int k = 0;
    while (v[k] && !isspace((unsigned char)v[k]) && v[k] != ';' && k < cap - 1) { buf[k] = v[k]; k++; }
    buf[k] = 0;
    return k > 0;
}

bool fd_is_multiline(const char *v) { return strchr(v, '\n') != NULL; }

bool fd_get_resolved(FDFile *f, const char *path, char *out, int cap)
{
    if (!fd_get(f, path, out, cap)) return false;
    for (int depth = 0; depth < 4 && out[0] == '$'; depth++) {
        char name[128]; const char *n = out + 1;
        if (*n == '{') n++;
        int k = 0; while (n[k] && n[k] != '}' && k < 127 && !isspace((unsigned char)n[k])) { name[k] = n[k]; k++; }
        name[k] = 0;
        if (!name[0] || !fd_get(f, name[0] == ':' ? name + 1 : name, out, cap)) return false;
    }
    return true;
}
