/* textdiff.c -- line diff for the review before saving (see textdiff.h). The common head and tail are cut first, so
 * the usual edit (a value or two in a dictionary) leaves a few lines in the middle for the LCS table; a middle too
 * big for the table falls back to "all old lines out, all new lines in", which is still right, only less tidy. */
#include "textdiff.h"

/* summaries are one line on screen: a long value is cut there on purpose */
#pragma GCC diagnostic ignored "-Wformat-truncation"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const char *s; int len; } Ln;

static int split(const char *t, Ln **out)
{
    int n = 0, cap = 64;
    Ln *l = malloc(sizeof(Ln) * (size_t)cap);
    if (!l) return -1;
    for (const char *p = t; p && *p; ) {
        const char *e = strchr(p, '\n');
        int len = e ? (int)(e - p) : (int)strlen(p);
        if (n == cap) { cap *= 2; Ln *nl = realloc(l, sizeof(Ln) * (size_t)cap); if (!nl) { free(l); return -1; } l = nl; }
        l[n++] = (Ln){ p, len };
        p = e ? e + 1 : NULL;
    }
    *out = l;
    return n;
}

static bool same(Ln a, Ln b) { return a.len == b.len && !memcmp(a.s, b.s, (size_t)a.len); }

static bool push(Diff *d, char kind, int al, int bl, Ln x)
{
    if (d->n == d->cap) {
        int nc = d->cap ? d->cap * 2 : 128;
        DiffLine *nl = realloc(d->l, sizeof(DiffLine) * (size_t)nc);
        if (!nl) return false;
        d->l = nl; d->cap = nc;
    }
    d->l[d->n++] = (DiffLine){ kind, al, bl, x.s, x.len };
    if (kind == '+') d->added++;
    if (kind == '-') d->removed++;
    return true;
}

void diff_free(Diff *d) { free(d->l); memset(d, 0, sizeof *d); }

bool diff_texts(const char *a, const char *b, int ctx, Diff *out)
{
    memset(out, 0, sizeof *out);
    Ln *A = NULL, *B = NULL;
    int na = split(a ? a : "", &A), nb = split(b ? b : "", &B);
    if (na < 0 || nb < 0) { free(A); free(B); return false; }
    int pre = 0; while (pre < na && pre < nb && same(A[pre], B[pre])) pre++;
    int suf = 0; while (suf < na - pre && suf < nb - pre && same(A[na - 1 - suf], B[nb - 1 - suf])) suf++;
    int ma = na - pre - suf, mb = nb - pre - suf;
    /* the full sequence of ops for the middle: LCS when it fits, else delete-all/insert-all */
    char *ops = malloc(2 * (size_t)(ma > mb ? ma : mb) + 1);
    int nops = 0;
    if (!ops) { free(A); free(B); return false; }
    if ((long long)(ma + 1) * (mb + 1) <= 4000000LL) {
        int W = mb + 1;
        unsigned short *T = calloc((size_t)(ma + 1) * (size_t)W, sizeof *T);
        if (!T) { free(ops); free(A); free(B); return false; }
        for (int i = ma - 1; i >= 0; i--)
            for (int j = mb - 1; j >= 0; j--)
                T[i * W + j] = same(A[pre + i], B[pre + j]) ? (unsigned short)(T[(i + 1) * W + j + 1] + 1)
                             : (T[(i + 1) * W + j] >= T[i * W + j + 1] ? T[(i + 1) * W + j] : T[i * W + j + 1]);
        int i = 0, j = 0;
        while (i < ma || j < mb) {
            if (i < ma && j < mb && same(A[pre + i], B[pre + j])) { ops[nops++] = '='; i++; j++; }
            else if (i < ma && (j >= mb || T[(i + 1) * W + j] >= T[i * W + j + 1])) { ops[nops++] = '-'; i++; }   /* old line first */
            else { ops[nops++] = '+'; j++; }
        }
        free(T);
    } else if (ma == mb) {                              /* too big for the table, same length: values changed in place */
        for (int i = 0; i < ma; i++) {
            if (same(A[pre + i], B[pre + i])) ops[nops++] = '=';
            else { ops[nops++] = '-'; ops[nops++] = '+'; }
        }
    } else {
        for (int i = 0; i < ma; i++) ops[nops++] = '-';
        for (int j = 0; j < mb; j++) ops[nops++] = '+';
    }
    /* the whole edit script, then only the changes with ctx lines around them */
    int total = pre + nops + suf;
    char *k = malloc((size_t)total + 1);
    int *ai = malloc(sizeof(int) * (size_t)(total + 1)), *bi = malloc(sizeof(int) * (size_t)(total + 1));
    if (!k || !ai || !bi) { free(k); free(ai); free(bi); free(ops); free(A); free(B); return false; }
    int t = 0, x = 0, y = 0;
    for (int q = 0; q < pre; q++) { k[t] = '='; ai[t] = x++; bi[t] = y++; t++; }
    for (int q = 0; q < nops; q++) {
        k[t] = ops[q];
        ai[t] = ops[q] == '+' ? -1 : x++;
        bi[t] = ops[q] == '-' ? -1 : y++;
        t++;
    }
    for (int q = 0; q < suf; q++) { k[t] = '='; ai[t] = x++; bi[t] = y++; t++; }
    bool ok = true;
    int lastShown = -1;
    for (int q = 0; q < total && ok; q++) {
        bool near = k[q] != '=';
        for (int r = q - ctx; r <= q + ctx && !near; r++) if (r >= 0 && r < total && k[r] != '=') near = true;
        if (!near) continue;
        if (lastShown >= 0 && q > lastShown + 1) ok = push(out, '@', 0, 0, (Ln){ "", 0 });
        if (k[q] == '=') ok = ok && push(out, ' ', ai[q] + 1, bi[q] + 1, A[ai[q]]);
        else if (k[q] == '-') ok = ok && push(out, '-', ai[q] + 1, 0, A[ai[q]]);
        else ok = ok && push(out, '+', 0, bi[q] + 1, B[bi[q]]);
        lastShown = q;
    }
    free(k); free(ai); free(bi); free(ops); free(A); free(B);
    return ok;
}

/* the first word of a line (a keyword, a MOOSE key before "=", a CalculiX card) and the rest, trimmed */
static void word_rest(const DiffLine *l, char *w, int wc, char *rest, int rc)
{
    int i = 0;
    while (i < l->len && (l->s[i] == ' ' || l->s[i] == '\t')) i++;
    int k = 0;
    while (i < l->len && !isspace((unsigned char)l->s[i]) && l->s[i] != '=' && l->s[i] != ',' && k < wc - 1) w[k++] = l->s[i++];
    w[k] = 0;
    while (i < l->len && (isspace((unsigned char)l->s[i]) || l->s[i] == '=')) i++;
    int e = l->len;
    while (e > i && (isspace((unsigned char)l->s[e - 1]) || l->s[e - 1] == ';')) e--;
    int n = e - i < rc - 1 ? e - i : rc - 1;
    if (n < 0) n = 0;
    memcpy(rest, l->s + i, (size_t)n); rest[n] = 0;
}

int diff_summary(const Diff *d, char (*out)[160], int max)
{
    int n = 0;
    for (int i = 0; i < d->n && n < max; i++) {
        const DiffLine *l = &d->l[i];
        if (l->kind != '-' && l->kind != '+') continue;
        char w[64], r[96];
        word_rest(l, w, sizeof w, r, sizeof r);
        if (!w[0]) continue;
        if (l->kind == '-') {
            /* the matching added line: the same first word among the + lines of this run */
            int m = -1;
            for (int j = i + 1; j < d->n && (d->l[j].kind == '-' || d->l[j].kind == '+'); j++) {
                if (d->l[j].kind != '+') continue;
                char w2[64], r2[96]; word_rest(&d->l[j], w2, sizeof w2, r2, sizeof r2);
                if (!strcmp(w, w2)) { m = j; break; }
            }
            if (m >= 0) {
                char w2[64], r2[96]; word_rest(&d->l[m], w2, sizeof w2, r2, sizeof r2);
                if (r[0] || r2[0]) snprintf(out[n++], 160, "%s: %s \xe2\x86\x92 %s", w, r[0] ? r : "(empty)", r2[0] ? r2 : "(empty)");
                else snprintf(out[n++], 160, "%s: changed", w);
            } else snprintf(out[n++], 160, "%s: removed", w);
        } else {
            bool paired = false;                              /* already summarised with its removed line */
            for (int j = i - 1; j >= 0 && (d->l[j].kind == '-' || d->l[j].kind == '+'); j--) {
                if (d->l[j].kind != '-') continue;
                char w2[64], r2[96]; word_rest(&d->l[j], w2, sizeof w2, r2, sizeof r2);
                if (!strcmp(w, w2)) { paired = true; break; }
            }
            if (!paired) snprintf(out[n++], 160, "%s: added (%s)", w, r);
        }
    }
    return n;
}
