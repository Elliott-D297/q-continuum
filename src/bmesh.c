/* bmesh.c -- blockMesh as a model: read, write, check, draw, templates (see bmesh.h) */
#define _GNU_SOURCE
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bmesh.h"

const char *const BE_NAME[BE__N] = { "arc", "spline", "polyLine", "BSpline" };

/* blockMesh's numbering (blockDescriptor / hexBlock): the 12 edges as vertex pairs, 0-3 along x1, 4-7 along x2, 8-11
   along x3; the 6 faces, each listed outward */
static const int EDGE[12][2] = { { 0, 1 }, { 3, 2 }, { 7, 6 }, { 4, 5 }, { 0, 3 }, { 1, 2 }, { 5, 6 }, { 4, 7 },
                                 { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } };
static const int FACE[6][4] = { { 0, 4, 7, 3 }, { 1, 2, 6, 5 }, { 0, 1, 5, 4 }, { 3, 7, 6, 2 }, { 0, 3, 2, 1 }, { 4, 5, 6, 7 } };

void bm_init(BmModel *m)
{
    memset(m, 0, sizeof *m);
    m->scale = 1;
}

double bm_cells(const BmModel *m)
{
    double n = 0;
    for (int i = 0; i < m->nb; i++) n += (double)m->b[i].n[0] * m->b[i].n[1] * m->b[i].n[2];
    return n;
}

/* ================================================================ tokens */
enum { T_END = 0, T_LP, T_RP, T_LB, T_RB, T_SEMI, T_WORD, T_STR };
typedef struct { const char *s; int i, n; int start; char w[256]; } Tk;

static int tk(Tk *t)
{
    const char *s = t->s;
    for (;;) {
        while (t->i < t->n && isspace((unsigned char)s[t->i])) t->i++;
        if (t->i + 1 < t->n && s[t->i] == '/' && s[t->i + 1] == '/') { while (t->i < t->n && s[t->i] != '\n') t->i++; continue; }
        if (t->i + 1 < t->n && s[t->i] == '/' && s[t->i + 1] == '*') {
            t->i += 2;
            while (t->i + 1 < t->n && !(s[t->i] == '*' && s[t->i + 1] == '/')) t->i++;
            t->i = t->i + 2 < t->n ? t->i + 2 : t->n;
            continue;
        }
        break;
    }
    t->start = t->i;
    if (t->i >= t->n) return T_END;
    char c = s[t->i];
    if (c == '(') { t->i++; return T_LP; }
    if (c == ')') { t->i++; return T_RP; }
    if (c == '{') { t->i++; return T_LB; }
    if (c == '}') { t->i++; return T_RB; }
    if (c == ';') { t->i++; return T_SEMI; }
    int k = 0;
    if (c == '"') {
        t->i++;
        while (t->i < t->n && s[t->i] != '"' && k < (int)sizeof t->w - 1) t->w[k++] = s[t->i++];
        if (t->i < t->n) t->i++;
        t->w[k] = 0;
        return T_STR;
    }
    while (t->i < t->n && !isspace((unsigned char)s[t->i]) && !strchr("(){};", s[t->i]) && k < (int)sizeof t->w - 1) t->w[k++] = s[t->i++];
    t->w[k] = 0;
    return T_WORD;
}

static bool is_num(const char *w, double *v)
{
    char *e; double d = strtod(w, &e);
    if (e == w || *e || !isfinite(d)) return false;              /* "nan" and "inf" are no coordinates */
    if (v) *v = d;
    return true;
}

static bool want(Tk *t, int type) { return tk(t) == type; }

static bool read_num(Tk *t, double *v) { return tk(t) == T_WORD && is_num(t->w, v); }
static bool read_int(Tk *t, int *v)
{
    double d;
    if (!read_num(t, &d) || d != floor(d) || fabs(d) > 1e9) return false;   /* whole, and fits an int */
    *v = (int)d;
    return true;
}
static bool read_point(Tk *t, double p[3]) { return want(t, T_LP) && read_num(t, &p[0]) && read_num(t, &p[1]) && read_num(t, &p[2]) && want(t, T_RP); }

/* a grading item: a number, or ((len cells ratio) ...) */
static bool read_grade(Tk *t, BmGrade *g)
{
    memset(g, 0, sizeof *g);
    int k = tk(t);
    double v;
    if (k == T_WORD && is_num(t->w, &v)) { g->n = 1; g->len[0] = g->cells[0] = 1; g->ratio[0] = v; return true; }
    if (k != T_LP) return false;
    for (;;) {
        k = tk(t);
        if (k == T_RP) break;
        if (k != T_LP || g->n >= BM_MAXSEG) return false;
        if (!read_num(t, &g->len[g->n]) || !read_num(t, &g->cells[g->n]) || !read_num(t, &g->ratio[g->n]) || !want(t, T_RP)) return false;
        g->n++;
    }
    return g->n > 0;
}

/* the text of an entry's value, NUL-terminated (malloc'd), or NULL when the entry isn't there */
static char *entry_text(FDFile *f, const char *key, FDEntry **out)
{
    FDEntry *e = fd_find(f, key);
    if (out) *out = e;
    if (!e) return NULL;
    int a = e->valStart, b = e->valEnd;                          /* a dictionary: the text inside its braces */
    char *s = malloc((size_t)(b - a) + 1);
    memcpy(s, f->text + a, (size_t)(b - a)); s[b - a] = 0;
    return s;
}

/* the things this model can't hold: anything evaluated or substituted */
static bool unsupported_text(const char *s, char *why, int cap, const char *where)
{
    Tk t = { s, 0, (int)strlen(s), 0, "" };
    int k;
    while ((k = tk(&t)) != T_END) {
        if (k != T_WORD) continue;
        if (t.w[0] == '$' || t.w[0] == '#') {
            if (why) snprintf(why, (size_t)cap, "%s uses %s: the block builder would replace it with plain numbers. Edit this blockMeshDict as text (Files) to keep it.", where, t.w);
            return true;
        }
    }
    return false;
}

static void set_why(char *why, int cap, const char *fmt, const char *a)
{
    if (why) snprintf(why, (size_t)cap, fmt, a);
}

bool bm_read(FDFile *f, BmModel *m, char *why, int cap)
{
    bm_init(m);
    if (why) why[0] = 0;
    if (!f || !f->ok) { set_why(why, cap, "%s", "The blockMeshDict could not be read"); return false; }
    char v[64];
    if (fd_get(f, "scale", v, sizeof v) || fd_get(f, "convertToMeters", v, sizeof v)) {
        if (!is_num(v, &m->scale)) { set_why(why, cap, "The scale (%s) isn't a plain number: edit this blockMeshDict as text (Files)", v); return false; }
    }
    /* what this model doesn't do */
    static const char *const NOPE[] = { "faces", "geometry", "namedVertices", "namedBlocks", "patches", NULL };
    for (int i = 0; NOPE[i]; i++) {
        FDEntry *e = fd_find(f, NOPE[i]);
        if (!e) continue;
        char *s = entry_text(f, NOPE[i], NULL);
        Tk t = { s, 0, (int)strlen(s), 0, "" };
        int k1 = tk(&t), k2 = tk(&t);
        free(s);
        if ((k1 == T_LP && k2 == T_RP) || k1 == T_END) continue;   /* an empty list or dictionary is no harm */
        if (why) snprintf(why, (size_t)cap, "This blockMeshDict has a %s section (%s), which the block builder doesn't edit: edit it as text (Files).",
                 NOPE[i], !strcmp(NOPE[i], "patches") ? "the old way of listing patches" : !strcmp(NOPE[i], "faces") ? "curved or projected faces" : "projected geometry");
        return false;
    }
    char *s;
    /* vertices */
    if (!(s = entry_text(f, "vertices", NULL))) { set_why(why, cap, "%s", "There is no vertices list"); return false; }
    if (unsupported_text(s, why, cap, "The vertices list")) { free(s); return false; }
    {
        Tk t = { s, 0, (int)strlen(s), 0, "" };
        bool ok = want(&t, T_LP);
        while (ok) {
            int k = tk(&t);
            if (k == T_RP) break;
            if (k != T_LP || m->nv >= BM_MAXV) { ok = false; break; }
            t.i = t.start;
            ok = read_point(&t, m->v[m->nv]);
            if (ok) m->nv++;
        }
        free(s);
        if (!ok) { set_why(why, cap, "%s", "The vertices list holds something other than plain (x y z) points (named or projected vertices): edit it as text (Files)."); return false; }
    }
    /* blocks */
    if (!(s = entry_text(f, "blocks", NULL))) { set_why(why, cap, "%s", "There is no blocks list"); return false; }
    if (unsupported_text(s, why, cap, "The blocks list")) { free(s); return false; }
    {
        Tk t = { s, 0, (int)strlen(s), 0, "" };
        bool ok = want(&t, T_LP);
        while (ok) {
            int k = tk(&t);
            if (k == T_RP) break;
            if (k != T_WORD || strcmp(t.w, "hex") || m->nb >= BM_MAXB) { ok = false; break; }
            BmBlock *b = &m->b[m->nb];
            memset(b, 0, sizeof *b);
            ok = want(&t, T_LP);
            for (int i = 0; ok && i < 8; i++) ok = read_int(&t, &b->v[i]);
            ok = ok && want(&t, T_RP);
            if (!ok) break;
            k = tk(&t);
            if (k == T_WORD) { snprintf(b->zone, sizeof b->zone, "%s", t.w); k = tk(&t); }
            ok = k == T_LP && read_int(&t, &b->n[0]) && read_int(&t, &b->n[1]) && read_int(&t, &b->n[2]) && want(&t, T_RP);
            if (!ok) break;
            int save = t.i;
            k = tk(&t);
            if (k == T_WORD && (!strcmp(t.w, "simpleGrading") || !strcmp(t.w, "edgeGrading"))) {
                int n = !strcmp(t.w, "simpleGrading") ? 3 : 12;
                b->kind = n == 3 ? BG_SIMPLE : BG_EDGE;
                ok = want(&t, T_LP);
                for (int i = 0; ok && i < n; i++) ok = read_grade(&t, &b->g[i]);
                ok = ok && want(&t, T_RP);
            } else {                                              /* no grading: uniform */
                t.i = save;
                b->kind = BG_SIMPLE;
                for (int i = 0; i < 3; i++) { b->g[i].n = 1; b->g[i].len[0] = b->g[i].cells[0] = b->g[i].ratio[0] = 1; }
            }
            if (ok) m->nb++;
        }
        free(s);
        if (!ok) { set_why(why, cap, "%s", "A block isn't a plain hex (8 vertices) (nx ny nz) simpleGrading / edgeGrading (...) the builder can read: edit it as text (Files)."); return false; }
    }
    /* edges (optional) */
    if ((s = entry_text(f, "edges", NULL))) {
        if (unsupported_text(s, why, cap, "The edges list")) { free(s); return false; }
        Tk t = { s, 0, (int)strlen(s), 0, "" };
        bool ok = want(&t, T_LP);
        while (ok) {
            int k = tk(&t);
            if (k == T_RP) break;
            if (k != T_WORD || m->ne >= BM_MAXE) { ok = false; break; }
            BmEdge *e = &m->e[m->ne];
            memset(e, 0, sizeof *e);
            e->kind = -1;
            for (int i = 0; i < BE__N; i++) if (!strcmp(t.w, BE_NAME[i])) e->kind = i;
            bool line = !strcmp(t.w, "line");
            if (e->kind < 0 && !line) { ok = false; break; }
            ok = read_int(&t, &e->a) && read_int(&t, &e->b);
            if (!ok) break;
            if (line) continue;                                    /* a straight edge: nothing to keep */
            if (e->kind == BE_ARC) {
                ok = read_point(&t, e->p[0]);                       /* the point form; origin / angle forms aren't read */
                e->np = 1;
            } else {
                ok = want(&t, T_LP);
                while (ok) {
                    k = tk(&t);
                    if (k == T_RP) break;
                    if (k != T_LP || e->np >= BM_MAXPTS) { ok = false; break; }
                    t.i = t.start;
                    ok = read_point(&t, e->p[e->np]);
                    if (ok) e->np++;
                }
            }
            if (ok) m->ne++;
        }
        free(s);
        if (!ok) { set_why(why, cap, "%s", "An edge isn't one the builder reads (arc a b (point), spline / polyLine / BSpline a b ((points)), line a b; not the origin or angle form of arc, projections, bezier): edit it as text (Files)."); return false; }
    }
    /* boundary (optional: an empty case) */
    FDEntry *be;
    if ((s = entry_text(f, "boundary", &be))) {
        if (unsupported_text(s, why, cap, "The boundary list")) { free(s); return false; }
        Tk t = { s, 0, (int)strlen(s), 0, "" };
        bool ok = want(&t, T_LP);
        while (ok) {
            int k = tk(&t);
            if (k == T_RP) break;
            if (k != T_WORD || m->np >= BM_MAXP) { ok = false; break; }
            BmPatch *p = &m->p[m->np];
            memset(p, 0, sizeof *p);
            snprintf(p->name, sizeof p->name, "%s", t.w);
            if (!want(&t, T_LB)) { ok = false; break; }
            for (;;) {
                k = tk(&t);
                if (k == T_RB) break;
                if (k != T_WORD) { ok = false; break; }
                int keyAt = t.start;
                char key[64]; snprintf(key, sizeof key, "%s", t.w);
                if (!strcmp(key, "type")) {
                    ok = tk(&t) == T_WORD;
                    if (ok) snprintf(p->type, sizeof p->type, "%s", t.w);
                    ok = ok && want(&t, T_SEMI);
                } else if (!strcmp(key, "faces")) {
                    ok = want(&t, T_LP);
                    while (ok) {
                        k = tk(&t);
                        if (k == T_RP) break;
                        if (k != T_LP || p->nf >= BM_MAXF) { ok = false; break; }
                        for (int i = 0; ok && i < 4; i++) ok = read_int(&t, &p->f[p->nf][i]);
                        ok = ok && want(&t, T_RP);
                        if (ok) p->nf++;
                    }
                    ok = ok && want(&t, T_SEMI);
                } else {                                         /* kept as written: inGroups, neighbourPatch, transform ... */
                    int depth = 0;
                    for (;;) {
                        k = tk(&t);
                        if (k == T_END) { ok = false; break; }
                        if (k == T_LP || k == T_LB) depth++;
                        else if (k == T_RP || k == T_RB) depth--;
                        else if (k == T_SEMI && depth == 0) break;
                    }
                    if (ok) {                                        /* kept whole, or the file isn't the builder's to edit */
                        size_t L = strlen(p->extra), add = (size_t)(t.i - keyAt) + (L ? 1 : 0);
                        if (L + add >= sizeof p->extra) {
                            if (why) snprintf(why, (size_t)cap, "Patch %s has more settings than the block builder keeps (%.40s ...): edit this "
                                              "blockMeshDict as text (Files).", p->name, s + keyAt);
                            free(s);
                            return false;
                        }
                        snprintf(p->extra + L, sizeof p->extra - L, "%s%.*s", L ? " " : "", t.i - keyAt, s + keyAt);
                    }
                }
                if (!ok) break;
            }
            if (ok) m->np++;
        }
        free(s);
        if (!ok) { set_why(why, cap, "%s", "A patch in the boundary list isn't one the builder reads (name { type ...; faces ((a b c d) ...); }): edit it as text (Files)."); return false; }
    }
    if (fd_find(f, "defaultPatch")) {
        fd_get(f, "defaultPatch/name", m->defaultName, sizeof m->defaultName);
        fd_get(f, "defaultPatch/type", m->defaultType, sizeof m->defaultType);
    }
    return true;
}

bool bm_read_text(const char *text, BmModel *m, char *why, int cap)
{
    FDFile *f = fd_from_text("blockMeshDict", text);
    bool ok = bm_read(f, m, why, cap);
    fd_free(f);
    return ok;
}

/* ================================================================ writing */
/* the shortest text that reads back as the same double (15 digits, else 17): a file put back through the builder meshes
   to the same points (2026-11-21: 10 digits moved T3A's points in the 10th digit) */
static void num(Str *o, double v)
{
    if (v == 0) v = 0;                                            /* no "-0" */
    char b[40];
    snprintf(b, sizeof b, "%.15g", v);
    if (strtod(b, NULL) != v) snprintf(b, sizeof b, "%.17g", v);
    str_cat(o, b);
}

static void grade_text(Str *o, const BmGrade *g)
{
    if (g->n <= 1) { num(o, g->n ? g->ratio[0] : 1); return; }
    str_cat(o, "(");
    for (int i = 0; i < g->n; i++) {
        str_cat(o, i ? " (" : "("); num(o, g->len[i]); str_cat(o, " "); num(o, g->cells[i]); str_cat(o, " "); num(o, g->ratio[i]); str_cat(o, ")");
    }
    str_cat(o, ")");
}

static void vertices_text(Str *o, const BmModel *m)
{
    str_cat(o, "(\n");
    for (int i = 0; i < m->nv; i++) {
        str_cat(o, "    ("); num(o, m->v[i][0]); str_cat(o, " "); num(o, m->v[i][1]); str_cat(o, " "); num(o, m->v[i][2]);
        str_catf(o, ")    // %d\n", i);
    }
    str_cat(o, ")");
}

static void blocks_text(Str *o, const BmModel *m)
{
    str_cat(o, "(\n");
    for (int i = 0; i < m->nb; i++) {
        const BmBlock *b = &m->b[i];
        str_catf(o, "    hex (%d %d %d %d %d %d %d %d)", b->v[0], b->v[1], b->v[2], b->v[3], b->v[4], b->v[5], b->v[6], b->v[7]);
        if (b->zone[0]) str_catf(o, " %s", b->zone);
        str_catf(o, " (%d %d %d) %s (", b->n[0], b->n[1], b->n[2], b->kind == BG_EDGE ? "edgeGrading" : "simpleGrading");
        int n = b->kind == BG_EDGE ? 12 : 3;
        for (int k = 0; k < n; k++) { if (k) str_cat(o, " "); grade_text(o, &b->g[k]); }
        str_catf(o, ")    // block %d\n", i);
    }
    str_cat(o, ")");
}

static void edges_text(Str *o, const BmModel *m)
{
    str_cat(o, "(\n");
    for (int i = 0; i < m->ne; i++) {
        const BmEdge *e = &m->e[i];
        str_catf(o, "    %s %d %d ", BE_NAME[e->kind], e->a, e->b);
        if (e->kind == BE_ARC) {
            str_cat(o, "("); num(o, e->p[0][0]); str_cat(o, " "); num(o, e->p[0][1]); str_cat(o, " "); num(o, e->p[0][2]); str_cat(o, ")");
        } else {
            str_cat(o, "(");
            for (int k = 0; k < e->np; k++) {
                str_cat(o, k ? " (" : "("); num(o, e->p[k][0]); str_cat(o, " "); num(o, e->p[k][1]); str_cat(o, " "); num(o, e->p[k][2]); str_cat(o, ")");
            }
            str_cat(o, ")");
        }
        str_cat(o, "\n");
    }
    str_cat(o, ")");
}

static void boundary_text(Str *o, const BmModel *m)
{
    str_cat(o, "(\n");
    for (int i = 0; i < m->np; i++) {
        const BmPatch *p = &m->p[i];
        str_catf(o, "    %s\n    {\n        type %s;\n", p->name, p->type[0] ? p->type : "patch");
        if (p->extra[0]) str_catf(o, "        %s\n", p->extra);
        str_cat(o, "        faces\n        (\n");
        for (int k = 0; k < p->nf; k++) str_catf(o, "            (%d %d %d %d)\n", p->f[k][0], p->f[k][1], p->f[k][2], p->f[k][3]);
        str_cat(o, "        );\n    }\n");
    }
    str_cat(o, ")");
}

char *bm_write(const BmModel *m, const char *note)
{
    Str o; str_init(&o);
    str_cat(&o, "/*--------------------------------*- C++ -*----------------------------------*\\\n"
                "  Written by Q-Continuum's block builder (Mesh > Block builder)\n"
                "\\*---------------------------------------------------------------------------*/\n"
                "FoamFile\n{\n    version     2.0;\n    format      ascii;\n    class       dictionary;\n    object      blockMeshDict;\n}\n");
    if (note && *note) {
        str_cat(&o, "// ");
        for (const char *p = note; *p; p++) { str_catf(&o, "%c", *p); if (*p == '\n' && p[1]) str_cat(&o, "// "); }
        str_cat(&o, "\n");
    }
    str_cat(&o, "\nscale   "); num(&o, m->scale); str_cat(&o, ";\n\nvertices\n");
    vertices_text(&o, m);
    str_cat(&o, ";\n\nblocks\n");
    blocks_text(&o, m);
    str_cat(&o, ";\n\nedges\n");
    edges_text(&o, m);
    str_cat(&o, ";\n\nboundary\n");
    boundary_text(&o, m);
    str_cat(&o, ";\n");
    if (m->defaultName[0] || m->defaultType[0])
        str_catf(&o, "\ndefaultPatch\n{\n    name %s;\n    type %s;\n}\n", m->defaultName[0] ? m->defaultName : "defaultFaces", m->defaultType[0] ? m->defaultType : "patch");
    str_cat(&o, "\nmergePatchPairs\n(\n);\n\n// ************************************************************************* //\n");
    return o.s;
}

bool bm_apply(FDFile *f, const BmModel *m)
{
    if (!f) return false;
    Str o; str_init(&o);
    bool ok = true;
    char sc[48]; snprintf(sc, sizeof sc, "%.15g", m->scale);
    if (strtod(sc, NULL) != m->scale) snprintf(sc, sizeof sc, "%.17g", m->scale);
    ok = ok && fd_put(f, fd_find(f, "convertToMeters") && !fd_find(f, "scale") ? "convertToMeters" : "scale", sc);
    o.len = 0; if (o.s) o.s[0] = 0; vertices_text(&o, m); ok = ok && fd_put(f, "vertices", o.s);
    o.len = 0; o.s[0] = 0; blocks_text(&o, m); ok = ok && fd_put(f, "blocks", o.s);
    o.len = 0; o.s[0] = 0; edges_text(&o, m); ok = ok && fd_put(f, "edges", o.s);
    o.len = 0; o.s[0] = 0; boundary_text(&o, m); ok = ok && fd_put(f, "boundary", o.s);
    str_free(&o);
    return ok;
}

/* ================================================================ the tables' text */
int bm_grading_text(const BmBlock *b, char *out, int cap)
{
    Str o; str_init(&o);
    int n = b->kind == BG_EDGE ? 12 : 3;
    for (int k = 0; k < n; k++) { if (k) str_cat(&o, " "); grade_text(&o, &b->g[k]); }
    int need = snprintf(out, (size_t)cap, "%s", o.s ? o.s : "");
    str_free(&o);
    return need;
}

bool bm_grading_parse(const char *text, BmBlock *b)
{
    Tk t = { text, 0, (int)strlen(text), 0, "" };
    BmGrade g[12]; int n = 0;
    for (;;) {
        int save = t.i;
        int k = tk(&t);
        if (k == T_END) break;
        t.i = save;
        if (n >= 12 || !read_grade(&t, &g[n])) return false;
        n++;
    }
    if (n != 3 && n != 12) return false;
    for (int i = 0; i < n; i++)
        for (int s = 0; s < g[i].n; s++) if (!(g[i].ratio[s] > 0) || !(g[i].len[s] > 0) || !(g[i].cells[s] > 0)) return false;
    b->kind = n == 3 ? BG_SIMPLE : BG_EDGE;
    memcpy(b->g, g, sizeof(BmGrade) * (size_t)n);
    return true;
}

int bm_ints_text(const int *v, int n, char *out, int cap)
{
    Str o; str_init(&o);
    for (int i = 0; i < n; i++) str_catf(&o, "%s%d", i ? " " : "", v[i]);
    int need = snprintf(out, (size_t)cap, "%s", o.s ? o.s : "");
    str_free(&o);
    return need;
}

bool bm_ints_parse(const char *text, int *v, int n)
{
    Tk t = { text, 0, (int)strlen(text), 0, "" };
    int got[64], k = 0, ty;
    while ((ty = tk(&t)) != T_END) {
        if (ty == T_LP || ty == T_RP) continue;
        double d;
        if (ty != T_WORD || !is_num(t.w, &d) || d != floor(d) || fabs(d) > 1e9 || k >= 64) return false;
        got[k++] = (int)d;
    }
    if (k != n) return false;
    memcpy(v, got, sizeof(int) * (size_t)n);
    return true;
}

int bm_faces_text(const BmPatch *p, char *out, int cap)
{
    Str o; str_init(&o);
    for (int i = 0; i < p->nf; i++) str_catf(&o, "%s(%d %d %d %d)", i ? " " : "", p->f[i][0], p->f[i][1], p->f[i][2], p->f[i][3]);
    int need = snprintf(out, (size_t)cap, "%s", o.s ? o.s : "");
    str_free(&o);
    return need;
}

bool bm_faces_parse(const char *text, BmPatch *p)
{
    Tk t = { text, 0, (int)strlen(text), 0, "" };
    int f[BM_MAXF][4], n = 0, ty;
    while ((ty = tk(&t)) != T_END) {
        if (ty != T_LP || n >= BM_MAXF) return false;
        for (int i = 0; i < 4; i++) if (!read_int(&t, &f[n][i])) return false;
        if (!want(&t, T_RP)) return false;
        n++;
    }
    memcpy(p->f, f, sizeof(int) * 4 * (size_t)n);
    p->nf = n;
    return true;
}

int bm_points_text(const BmEdge *e, char *out, int cap)
{
    Str o; str_init(&o);
    for (int i = 0; i < e->np; i++) {
        str_cat(&o, i ? " (" : "("); num(&o, e->p[i][0]); str_cat(&o, " "); num(&o, e->p[i][1]); str_cat(&o, " "); num(&o, e->p[i][2]); str_cat(&o, ")");
    }
    int need = snprintf(out, (size_t)cap, "%s", o.s ? o.s : "");
    str_free(&o);
    return need;
}

bool bm_points_parse(const char *text, BmEdge *e)
{
    Tk t = { text, 0, (int)strlen(text), 0, "" };
    double (*p)[3] = malloc(sizeof(double) * 3 * BM_MAXPTS);
    if (!p) return false;
    int n = 0, ty;
    bool ok = true;
    while (ok && (ty = tk(&t)) != T_END) {
        if (ty != T_LP || n >= BM_MAXPTS) { ok = false; break; }
        t.i = t.start;
        ok = read_point(&t, p[n]);
        if (ok) n++;
    }
    if (ok && (n < 1 || (e->kind == BE_ARC && n != 1))) ok = false;
    if (ok) { memcpy(e->p, p, sizeof(double) * 3 * (size_t)n); e->np = n; }
    free(p);
    return ok;
}

/* ================================================================ geometry */
static void sub3(const double *a, const double *b, double *o) { o[0] = a[0] - b[0]; o[1] = a[1] - b[1]; o[2] = a[2] - b[2]; }
static void cross3(const double *a, const double *b, double *o) { o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0]; }
static double dot3(const double *a, const double *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static double len3(const double *a) { return sqrt(dot3(a, a)); }

/* the curved edge between vertices a and b, if any; *flip when it is listed b to a */
static const BmEdge *find_edge(const BmModel *m, int a, int b, bool *flip)
{
    for (int i = 0; i < m->ne; i++) {
        if (m->e[i].a == a && m->e[i].b == b) { *flip = false; return &m->e[i]; }
        if (m->e[i].a == b && m->e[i].b == a) { *flip = true; return &m->e[i]; }
    }
    return NULL;
}

/* the circle through p0, pm, p1: centre and radius; false when the three points are in line */
static bool circle3(const double *p0, const double *pm, const double *p1, double *c, double *n)
{
    double a[3], b[3]; sub3(pm, p0, a); sub3(p1, p0, b);
    double ab[3]; cross3(a, b, ab);
    double d = 2 * dot3(ab, ab);
    if (d < 1e-30) return false;
    double aa = dot3(a, a), bb = dot3(b, b), t1[3], t2[3], k[3];
    cross3(ab, a, t1); cross3(b, ab, t2);
    for (int i = 0; i < 3; i++) k[i] = (bb * t1[i] + aa * t2[i]) / d;
    for (int i = 0; i < 3; i++) c[i] = p0[i] + k[i];
    double l = len3(ab); for (int i = 0; i < 3; i++) n[i] = ab[i] / l;
    return true;
}

/* a point on the curve through pts[0..n-1] at t in [0, 1] by arc length of the polyline, Catmull-Rom between points for
   the splines (blockMesh's spline is a Catmull-Rom spline; its BSpline is close enough for a preview) */
static void curve_point(const double (*pts)[3], int n, bool smooth, double t, double *out)
{
    if (n < 2) { memcpy(out, pts[0], sizeof(double) * 3); return; }
    double total = 0, seg[BM_MAXPTS + 2];
    for (int i = 0; i + 1 < n; i++) { double d[3]; sub3(pts[i + 1], pts[i], d); seg[i] = len3(d); total += seg[i]; }
    double want_ = t * total, acc = 0;
    int i = 0;
    while (i < n - 2 && acc + seg[i] < want_) { acc += seg[i]; i++; }
    double u = seg[i] > 0 ? (want_ - acc) / seg[i] : 0;
    if (u < 0) u = 0;
    if (u > 1) u = 1;
    if (!smooth) { for (int k = 0; k < 3; k++) out[k] = pts[i][k] + u * (pts[i + 1][k] - pts[i][k]); return; }
    const double *p0 = pts[i > 0 ? i - 1 : i], *p1 = pts[i], *p2 = pts[i + 1], *p3 = pts[i + 2 < n ? i + 2 : i + 1];
    double u2 = u * u, u3 = u2 * u;
    for (int k = 0; k < 3; k++)
        out[k] = 0.5 * (2 * p1[k] + (-p0[k] + p2[k]) * u + (2 * p0[k] - 5 * p1[k] + 4 * p2[k] - p3[k]) * u2 + (-p0[k] + 3 * p1[k] - 3 * p2[k] + p3[k]) * u3);
}

static void edge_between(const BmModel *m, int a, int b, double t, double *out)
{
    const double *pa = m->v[a], *pb = m->v[b];
    bool flip;
    const BmEdge *e = find_edge(m, a, b, &flip);
    if (!e) { for (int k = 0; k < 3; k++) out[k] = pa[k] + t * (pb[k] - pa[k]); return; }
    double tt = flip ? 1 - t : t;
    const double *s0 = m->v[e->a], *s1 = m->v[e->b];
    if (e->kind == BE_ARC) {
        double c[3], n[3];
        if (!circle3(s0, e->p[0], s1, c, n)) { for (int k = 0; k < 3; k++) out[k] = s0[k] + tt * (s1[k] - s0[k]); return; }
        double r0[3], r1[3], rm[3]; sub3(s0, c, r0); sub3(s1, c, r1); sub3(e->p[0], c, rm);
        double R = len3(r0), u[3], w[3];
        for (int k = 0; k < 3; k++) u[k] = r0[k] / R;
        cross3(n, u, w);
        double ang1 = atan2(dot3(r1, w), dot3(r1, u)), angm = atan2(dot3(rm, w), dot3(rm, u));
        if (ang1 < 0) ang1 += 2 * M_PI;
        if (angm < 0) angm += 2 * M_PI;
        if (angm > ang1) ang1 -= 2 * M_PI;                     /* the way through the middle point */
        double a2 = tt * ang1;
        for (int k = 0; k < 3; k++) out[k] = c[k] + R * (cos(a2) * u[k] + sin(a2) * w[k]);
        return;
    }
    double (*pts)[3] = malloc(sizeof(double) * 3 * (size_t)(e->np + 2));
    memcpy(pts[0], s0, sizeof(double) * 3);
    for (int i = 0; i < e->np; i++) memcpy(pts[i + 1], e->p[i], sizeof(double) * 3);
    memcpy(pts[e->np + 1], s1, sizeof(double) * 3);
    curve_point((const double (*)[3])pts, e->np + 2, e->kind != BE_POLYLINE, tt, out);
    free(pts);
}

bool bm_block_edge(const BmModel *m, int b, int k, int *a, int *c)
{
    if (b < 0 || b >= m->nb || k < 0 || k > 11) return false;
    *a = m->b[b].v[EDGE[k][0]]; *c = m->b[b].v[EDGE[k][1]];
    return true;
}

void bm_edge_point(const BmModel *m, int b, int k, double t, double out[3])
{
    out[0] = out[1] = out[2] = 0;
    if (b < 0 || b >= m->nb || k < 0 || k > 11) return;
    const BmBlock *bl = &m->b[b];
    int a = bl->v[EDGE[k][0]], c = bl->v[EDGE[k][1]];
    if (a < 0 || a >= m->nv || c < 0 || c >= m->nv) return;     /* a vertex number typed out of range */
    edge_between(m, a, c, t, out);
}

void bm_grade_points(const BmGrade *g, int n, double *out)
{
    out[0] = 0;
    if (n <= 0) return;
    BmGrade one = { 1, { 1 }, { 1 }, { 1 } };
    if (!g || g->n <= 0) g = &one;
    double sl = 0, sc = 0;
    for (int i = 0; i < g->n; i++) { sl += g->len[i]; sc += g->cells[i]; }
    if (sl <= 0 || sc <= 0) { for (int i = 0; i <= n; i++) out[i] = (double)i / n; return; }
    /* cells per segment as blockMesh shares them: rounded, the last takes what is left */
    int at = 0; double x0 = 0;
    for (int s = 0; s < g->n; s++) {
        int ns = s == g->n - 1 ? n - at : (int)lround(n * g->cells[s] / sc);
        if (ns < 0) ns = 0;
        if (at + ns > n) ns = n - at;
        double L = g->len[s] / sl, R = g->ratio[s];
        double r = ns > 1 && R > 0 ? pow(R, 1.0 / (ns - 1)) : 1;
        double d = fabs(r - 1) < 1e-12 ? L / (ns > 0 ? ns : 1) : L * (r - 1) / (pow(r, ns) - 1), x = x0;
        for (int i = 0; i < ns; i++) { x += d; d *= r; out[at + i + 1] = x; }
        at += ns; x0 += L;
    }
    for (int i = at + 1; i <= n; i++) out[i] = 1;
    out[n] = 1;
}

static const BmGrade *grade_of_edge(const BmBlock *b, int k)
{
    return b->kind == BG_EDGE ? &b->g[k] : &b->g[k / 4];
}

void bm_draw_build(const BmModel *m, BmDraw *d)
{
    memset(d, 0, sizeof *d);
    int cap = 1024;
    d->seg = malloc(sizeof(float) * 6 * (size_t)cap);
    d->segBlockEdge = malloc(sizeof(int) * (size_t)cap);
    for (int k = 0; k < 3; k++) { d->lo[k] = 1e30f; d->hi[k] = -1e30f; }
    double sc = m->scale > 0 ? m->scale : 1;
    /* every distinct block edge once, sampled; and a short tick at each cell division along it */
    int *seen = calloc((size_t)m->nb * 12, sizeof(int));
    for (int b = 0; b < m->nb; b++) {
        const BmBlock *bl = &m->b[b];
        bool bad = false;
        for (int i = 0; i < 8; i++) if (bl->v[i] < 0 || bl->v[i] >= m->nv) bad = true;
        if (bad) continue;
        double c[3] = { 0, 0, 0 };
        for (int i = 0; i < 8; i++) for (int k = 0; k < 3; k++) c[k] += m->v[bl->v[i]][k] / 8;
        for (int e = 0; e < 12; e++) {
            int a = bl->v[EDGE[e][0]], bb = bl->v[EDGE[e][1]];
            if (a == bb) continue;                                 /* collapsed (a wedge's axis) */
            bool dup = false;
            for (int q = 0; q < b && !dup; q++)
                for (int f = 0; f < 12 && !dup; f++) {
                    int qa = m->b[q].v[EDGE[f][0]], qb = m->b[q].v[EDGE[f][1]];
                    if ((qa == a && qb == bb) || (qa == bb && qb == a)) dup = seen[q * 12 + f];
                }
            seen[b * 12 + e] = 1;
            int n = bl->n[e / 4];
            if (n < 1) n = 1;
            int ns = 24;
            double prev[3]; edge_between(m, a, bb, 0, prev);
            if (!dup)
                for (int s = 1; s <= ns; s++) {
                    double p[3]; edge_between(m, a, bb, (double)s / ns, p);
                    if (d->nSeg + 2 > cap) { cap *= 2; d->seg = realloc(d->seg, sizeof(float) * 6 * (size_t)cap); d->segBlockEdge = realloc(d->segBlockEdge, sizeof(int) * (size_t)cap); }
                    float *g = &d->seg[d->nSeg * 6];
                    for (int k = 0; k < 3; k++) { g[k] = (float)(prev[k] * sc); g[3 + k] = (float)(p[k] * sc); }
                    d->segBlockEdge[d->nSeg++] = 1;
                    memcpy(prev, p, sizeof prev);
                }
            /* the divisions, as ticks toward the block's middle */
            if (n > 400) continue;
            double *t = malloc(sizeof(double) * (size_t)(n + 1));
            bm_grade_points(grade_of_edge(bl, e), n, t);
            for (int i = 1; i < n; i++) {
                double p[3]; edge_between(m, a, bb, t[i], p);
                double q[3]; for (int k = 0; k < 3; k++) q[k] = p[k] + 0.06 * (c[k] - p[k]);
                if (d->nSeg + 2 > cap) { cap *= 2; d->seg = realloc(d->seg, sizeof(float) * 6 * (size_t)cap); d->segBlockEdge = realloc(d->segBlockEdge, sizeof(int) * (size_t)cap); }
                float *g = &d->seg[d->nSeg * 6];
                for (int k = 0; k < 3; k++) { g[k] = (float)(p[k] * sc); g[3 + k] = (float)(q[k] * sc); }
                d->segBlockEdge[d->nSeg++] = 0;
            }
            free(t);
        }
    }
    free(seen);
    for (int i = 0; i < d->nSeg; i++)
        for (int k = 0; k < 3; k++) {
            float a = d->seg[i * 6 + k], b = d->seg[i * 6 + 3 + k];
            d->lo[k] = fminf(d->lo[k], fminf(a, b)); d->hi[k] = fmaxf(d->hi[k], fmaxf(a, b));
        }
    if (!d->nSeg) for (int k = 0; k < 3; k++) { d->lo[k] = 0; d->hi[k] = 1; }
}

void bm_draw_free(BmDraw *d) { free(d->seg); free(d->segBlockEdge); memset(d, 0, sizeof *d); }

/* ================================================================ checks */
static int add_issue(BmIssue *out, int max, int n, int level, const char *fmt, ...) __attribute__((format(printf, 5, 6)));
static int add_issue(BmIssue *out, int max, int n, int level, const char *fmt, ...)
{
    if (n >= max) return n;
    va_list ap; va_start(ap, fmt);
    vsnprintf(out[n].text, sizeof out[n].text, fmt, ap);
    va_end(ap);
    out[n].level = level;
    return n + 1;
}

/* a face as its sorted distinct vertices (a degenerate face has fewer than 3) */
static int face_key(const int *f, int key[4])
{
    int t[4] = { f[0], f[1], f[2], f[3] }, n = 0;
    for (int i = 0; i < 4; i++) {
        bool dup = false;
        for (int j = 0; j < n; j++) if (key[j] == t[i]) dup = true;
        if (!dup) key[n++] = t[i];
    }
    for (int i = 0; i < n; i++) for (int j = i + 1; j < n; j++) if (key[j] < key[i]) { int x = key[i]; key[i] = key[j]; key[j] = x; }
    for (int i = n; i < 4; i++) key[i] = -1;
    return n;
}

/* the cells along the edge from global vertex a to b in a block, or -1 when it isn't one of the block's edges */
static int edge_cells(const BmBlock *b, int a, int c)
{
    for (int k = 0; k < 12; k++) {
        int p = b->v[EDGE[k][0]], q = b->v[EDGE[k][1]];
        if ((p == a && q == c) || (p == c && q == a)) return b->n[k / 4];
    }
    return -1;
}

static double block_det(const BmModel *m, const BmBlock *b)
{
    double d[3][3] = { { 0 } };
    for (int dir = 0; dir < 3; dir++)
        for (int e = 0; e < 4; e++) {
            double v[3]; sub3(m->v[b->v[EDGE[dir * 4 + e][1]]], m->v[b->v[EDGE[dir * 4 + e][0]]], v);
            for (int k = 0; k < 3; k++) d[dir][k] += v[k] / 4;
        }
    double c[3]; cross3(d[0], d[1], c);
    return dot3(c, d[2]);
}

int bm_check(const BmModel *m, BmIssue *out, int max)
{
    int n = 0;
    if (!m->nb) return add_issue(out, max, n, 2, "There are no blocks: add one (or start from a template).");
    if (!(m->scale > 0)) n = add_issue(out, max, n, 2, "The scale is %g: it must be above 0.", m->scale);
    bool vok = true;
    for (int i = 0; i < m->nb; i++) {
        const BmBlock *b = &m->b[i];
        for (int k = 0; k < 8; k++) if (b->v[k] < 0 || b->v[k] >= m->nv) {
            n = add_issue(out, max, n, 2, "Block %d uses vertex %d; the vertices run 0 to %d.", i, b->v[k], m->nv - 1);
            vok = false; break;
        }
        for (int k = 0; k < 3; k++) if (b->n[k] < 1) n = add_issue(out, max, n, 2, "Block %d has %d cells along x%d: at least 1.", i, b->n[k], k + 1);
        int ng = b->kind == BG_EDGE ? 12 : 3;
        for (int k = 0; k < ng; k++)
            for (int s = 0; s < b->g[k].n; s++) {
                if (!(b->g[k].ratio[s] > 0)) n = add_issue(out, max, n, 2, "Block %d: an expansion ratio of %g; ratios are above 0 (below 1 the cells shrink).", i, b->g[k].ratio[s]);
                if (!(b->g[k].len[s] > 0) || !(b->g[k].cells[s] > 0))
                    n = add_issue(out, max, n, 2, "Block %d: a multi-grading segment with %g of the length and %g of the cells; both shares are above 0.", i, b->g[k].len[s], b->g[k].cells[s]);
            }
    }
    if (!vok) return n;
    for (int i = 0; i < m->nb; i++) {
        double det = block_det(m, &m->b[i]);
        if (det < 0) n = add_issue(out, max, n, 2, "Block %d is inside out: its vertices go clockwise. List the bottom four counter-clockwise seen from above "
                                                 "(x1, x2, x3 a right-handed set), or swap its top and bottom four.", i);
        else if (det == 0) n = add_issue(out, max, n, 2, "Block %d has no volume: its vertices are flat or repeated.", i);
    }
    /* faces: each block face once (boundary) or twice (between blocks); boundary faces in a patch */
    int nf = 0, cap = m->nb * 6;
    int (*keys)[4] = malloc(sizeof(int) * 4 * (size_t)cap);
    int *count = calloc((size_t)cap, sizeof(int)), *owner = malloc(sizeof(int) * (size_t)cap);
    int *inPatch = calloc((size_t)cap, sizeof(int));
    for (int i = 0; i < m->nb; i++)
        for (int fc = 0; fc < 6; fc++) {
            int f[4], key[4];
            for (int k = 0; k < 4; k++) f[k] = m->b[i].v[FACE[fc][k]];
            if (face_key(f, key) < 3) continue;
            int j = 0;
            while (j < nf && memcmp(keys[j], key, sizeof key)) j++;
            if (j == nf) { memcpy(keys[nf], key, sizeof key); owner[nf] = i; nf++; }
            else if (count[j] == 1) {
                /* the cells must match across it, edge by edge: each side of the face is an edge of both blocks */
                int ao = owner[j];
                for (int s = 0; s < 4; s++) {
                    int va = f[s], vb = f[(s + 1) % 4];
                    if (va == vb) continue;                   /* a collapsed side */
                    int ca = edge_cells(&m->b[ao], va, vb), cb = edge_cells(&m->b[i], va, vb);
                    if (ca > 0 && cb > 0 && ca != cb) {
                        n = add_issue(out, max, n, 2, "Blocks %d and %d share a face but not its cells: %d against %d along the edge %d to %d. blockMesh "
                                                     "would leave a seam: give them the same count along that edge.", ao, i, ca, cb, va, vb);
                        break;
                    }
                }
            }
            count[j]++;
        }
    for (int j = 0; j < nf; j++)
        if (count[j] > 2) n = add_issue(out, max, n, 2, "The face (%d %d %d %d) belongs to %d blocks: two at most.", keys[j][0], keys[j][1], keys[j][2], keys[j][3], count[j]);
    for (int p = 0; p < m->np; p++) {
        const BmPatch *pa = &m->p[p];
        if (!pa->name[0]) n = add_issue(out, max, n, 2, "Patch %d has no name.", p);
        for (int q = 0; q < p; q++) if (!strcmp(m->p[q].name, pa->name)) n = add_issue(out, max, n, 2, "Two patches are called %s.", pa->name);
        static const char *const TYPES[] = { "patch", "wall", "symmetry", "symmetryPlane", "empty", "wedge", "cyclic", "cyclicAMI", "processor", NULL };
        bool known = false;
        for (int t = 0; TYPES[t]; t++) if (!strcmp(pa->type, TYPES[t])) known = true;
        if (!known) n = add_issue(out, max, n, 1, "Patch %s has type %s, not one of patch, wall, symmetry, symmetryPlane, empty, wedge, cyclic.", pa->name, pa->type);
        if (!strcmp(pa->type, "cyclic") && !strstr(pa->extra, "neighbourPatch"))
            n = add_issue(out, max, n, 2, "Patch %s is cyclic without a neighbourPatch.", pa->name);
        for (int k = 0; k < pa->nf; k++) {
            int key[4];
            bool range = true;
            for (int i = 0; i < 4; i++) if (pa->f[k][i] < 0 || pa->f[k][i] >= m->nv) range = false;
            if (!range) { n = add_issue(out, max, n, 2, "Patch %s lists a face with a vertex outside 0 to %d.", pa->name, m->nv - 1); continue; }
            if (face_key(pa->f[k], key) < 3) continue;          /* a collapsed face (a wedge's axis): blockMesh drops it */
            int j = 0;
            while (j < nf && memcmp(keys[j], key, sizeof key)) j++;
            if (j == nf) n = add_issue(out, max, n, 2, "Patch %s lists the face (%d %d %d %d), which is no block's face.", pa->name, pa->f[k][0], pa->f[k][1], pa->f[k][2], pa->f[k][3]);
            else if (count[j] == 2) n = add_issue(out, max, n, 2, "Patch %s lists the face (%d %d %d %d), which lies between two blocks.", pa->name, pa->f[k][0], pa->f[k][1], pa->f[k][2], pa->f[k][3]);
            else if (inPatch[j]) n = add_issue(out, max, n, 2, "The face (%d %d %d %d) is in two patches.", pa->f[k][0], pa->f[k][1], pa->f[k][2], pa->f[k][3]);
            else inPatch[j] = p + 1;
        }
    }
    int loose = 0;
    for (int j = 0; j < nf; j++) if (count[j] == 1 && !inPatch[j]) loose++;
    if (loose) n = add_issue(out, max, n, 1, "%d outside face%s %s in no patch: blockMesh puts %s in %s (%s).", loose, loose == 1 ? "" : "s", loose == 1 ? "is" : "are",
                             loose == 1 ? "it" : "them", m->defaultName[0] ? m->defaultName : "defaultFaces", m->defaultType[0] ? m->defaultType : "empty");
    free(keys); free(count); free(owner); free(inPatch);
    /* edges */
    for (int i = 0; i < m->ne; i++) {
        const BmEdge *e = &m->e[i];
        if (e->a < 0 || e->a >= m->nv || e->b < 0 || e->b >= m->nv) { n = add_issue(out, max, n, 2, "Edge %d joins vertices %d and %d; they run 0 to %d.", i, e->a, e->b, m->nv - 1); continue; }
        bool used = false;
        for (int b = 0; b < m->nb && !used; b++)
            for (int k = 0; k < 12 && !used; k++) {
                int a = m->b[b].v[EDGE[k][0]], c = m->b[b].v[EDGE[k][1]];
                used = (a == e->a && c == e->b) || (a == e->b && c == e->a);
            }
        if (!used) n = add_issue(out, max, n, 1, "The %s edge %d to %d isn't an edge of any block: blockMesh ignores it.", BE_NAME[e->kind], e->a, e->b);
        for (int j = 0; j < i; j++)
            if ((m->e[j].a == e->a && m->e[j].b == e->b) || (m->e[j].a == e->b && m->e[j].b == e->a))
                n = add_issue(out, max, n, 2, "Two curved edges join vertices %d and %d.", e->a, e->b);
        if (e->kind == BE_ARC) {
            double c[3], nn[3];
            if (!circle3(m->v[e->a], e->p[0], m->v[e->b], c, nn)) n = add_issue(out, max, n, 2, "The arc %d to %d: its point is in line with the ends, so it makes no circle.", e->a, e->b);
        } else if (e->np < 1) n = add_issue(out, max, n, 1, "The %s edge %d to %d has no points between its ends: it is straight.", BE_NAME[e->kind], e->a, e->b);
    }
    double cells = bm_cells(m);
    if (cells > 5e7) n = add_issue(out, max, n, 1, "%.0f million cells: more than one machine meshes and solves comfortably.", cells / 1e6);
    return n;
}

/* ================================================================ templates */
#define P(k, l, u, h, d, lo, hi) { k, l, u, h, d, lo, hi }
const BmTemplate BM_TEMPLATES[BT__N] = {
    [BT_CAVITY] = { "cavity", "Lid-driven cavity", "A square box whose top wall slides: the classic first case. 2-D, one cell deep.", 4, {
        P("L", "Side", "m", "The square's side", 0.1, 1e-6, 1e4),
        P("N", "Cells along a side", "", "", 20, 2, 2000),
        P("first", "First cell at the walls", "m", "0: even cells; else they grow from each wall", 0, 0, 1e4),
        P("depth", "Depth (one cell)", "m", "", 0.01, 1e-9, 1e4) }, "movingWall (wall, the lid), fixedWalls (wall), frontAndBack (empty)" },
    [BT_CHANNEL] = { "channel", "Channel or duct", "Flow between two walls (2-D) or in a rectangular duct (3-D, with a width).", 8, {
        P("L", "Length", "m", "Along the flow", 1, 1e-6, 1e4),
        P("H", "Height", "m", "Wall to wall", 0.1, 1e-6, 1e4),
        P("W", "Width", "m", "0: a 2-D channel", 0, 0, 1e4),
        P("Nx", "Cells along", "", "", 100, 1, 100000),
        P("Ny", "Cells across the height", "", "", 20, 2, 100000),
        P("Nz", "Cells across the width", "", "3-D only", 20, 2, 100000),
        P("first", "First cell at the walls", "m", "0: even cells", 0, 0, 1e4),
        P("depth", "Depth (2-D, one cell)", "m", "", 0.01, 1e-9, 1e4) }, "inlet, outlet (patch), walls (wall), frontAndBack (empty, 2-D)" },
    [BT_STEP] = { "step", "Backward-facing step", "A channel that suddenly deepens: the flow separates at the step and reattaches downstream.", 7, {
        P("h", "Step height", "m", "", 0.025, 1e-6, 1e4),
        P("Hi", "Inlet channel height", "m", "Above the step", 0.025, 1e-6, 1e4),
        P("Lu", "Length before the step", "m", "", 0.2, 1e-6, 1e4),
        P("Ld", "Length after the step", "m", "", 0.5, 1e-6, 1e4),
        P("Nh", "Cells across the step", "", "The other counts follow, for square cells", 10, 2, 10000),
        P("first", "First cell at the walls", "m", "0: even cells", 0, 0, 1e4),
        P("depth", "Depth (one cell)", "m", "", 0.001, 1e-9, 1e4) }, "inlet, outlet (patch), upperWall, lowerWall (wall), frontAndBack (empty)" },
    [BT_PLATE] = { "plate", "Flat plate", "A boundary layer growing along a thin plate in a free stream, a stretch of symmetry ahead of it.", 7, {
        P("Lp", "Plate length", "m", "", 1, 1e-6, 1e4),
        P("Lu", "Free stretch before the plate", "m", "", 0.2, 0, 1e4),
        P("H", "Domain height", "m", "Well clear of the boundary layer: a few times its thickness", 0.3, 1e-6, 1e4),
        P("Nx", "Cells along the plate", "", "", 100, 2, 100000),
        P("Ny", "Cells up", "", "", 60, 2, 100000),
        P("first", "First cell on the plate", "m", "From the y+ calculator", 1e-4, 0, 1e4),
        P("depth", "Depth (one cell)", "m", "", 0.01, 1e-9, 1e4) }, "inlet, outlet, top (patch), plate (wall), symmetry (symmetryPlane), frontAndBack (empty)" },
    [BT_PIPE] = { "pipe", "Pipe (O-grid)", "A round pipe in 3-D: a square core and four curved blocks round it, so no cell is squashed at the centre.", 7, {
        P("D", "Diameter", "m", "", 0.05, 1e-6, 1e4),
        P("L", "Length", "m", "", 0.5, 1e-6, 1e4),
        P("Nq", "Cells round a quarter", "", "Also across the core", 10, 2, 2000),
        P("Nr", "Cells from core to wall", "", "", 10, 1, 2000),
        P("Nl", "Cells along", "", "", 100, 1, 100000),
        P("core", "Core size", "", "The core's corners as a share of the radius (0.4 to 0.7)", 0.55, 0.2, 0.85),
        P("first", "First cell at the wall", "m", "0: even cells", 0, 0, 1e4) }, "inlet, outlet (patch), wall (wall)" },
    [BT_CYLINDER] = { "cylinder", "Cylinder in crossflow", "Flow past a circular cylinder (2-D): a ring of curved blocks round it, eight more out to the edges.", 9, {
        P("D", "Diameter", "m", "", 0.1, 1e-6, 1e4),
        P("ring", "Ring size", "", "The ring's square, as a multiple of the diameter (1.5 to 4)", 2, 1.2, 20),
        P("Lu", "Upstream extent", "m", "From the centre", 0.5, 1e-6, 1e4),
        P("Ld", "Downstream extent", "m", "From the centre: the wake needs room", 1.5, 1e-6, 1e4),
        P("Hh", "Half height", "m", "Centre to top and bottom", 0.5, 1e-6, 1e4),
        P("Nq", "Cells round a quarter", "", "", 20, 2, 2000),
        P("Nr", "Cells across the ring", "", "", 20, 1, 2000),
        P("first", "First cell on the cylinder", "m", "0: even cells", 0, 0, 1e4),
        P("depth", "Depth (one cell)", "m", "", 0.01, 1e-9, 1e4) }, "inlet, outlet (patch), cylinder (wall), sides (symmetry), frontAndBack (empty)" },
    [BT_AIRFOIL] = { "airfoil", "Airfoil (C-mesh)", "A NACA four-digit airfoil (2-D) in a C-shaped mesh: wrapped round the nose, a wake behind.", 12, {
        P("t", "Thickness", "%", "NACA ..XX: 12 for 0012", 12, 1, 40),
        P("camber", "Camber", "%", "NACA X...: 0 for symmetric", 0, 0, 9),
        P("pos", "Camber position", "tenths", "NACA .X..", 4, 1, 9),
        P("c", "Chord", "m", "", 1, 1e-6, 1e4),
        P("aoa", "Angle of attack", "°", "Nose up", 0, -20, 20),
        P("R", "Far-field radius", "chords", "", 8, 2, 100),
        P("W", "Wake length", "chords", "", 8, 1, 100),
        P("Nc", "Cells round each surface", "", "", 80, 8, 4000),
        P("Nn", "Cells out from the surface", "", "", 60, 4, 4000),
        P("Nw", "Cells along the wake", "", "", 60, 4, 4000),
        P("first", "First cell on the airfoil", "m", "From the y+ calculator", 2e-5, 0, 1e4),
        P("depth", "Depth (one cell)", "m", "", 0.1, 1e-9, 1e4) }, "airfoil (wall), farfield, outlet (patch), frontAndBack (empty)" },
    [BT_WEDGE] = { "wedge", "Axisymmetric pipe (wedge)", "A round pipe solved as one thin slice: a 2-D axisymmetric case, far cheaper than 3-D.", 6, {
        P("D", "Diameter", "m", "", 0.05, 1e-6, 1e4),
        P("L", "Length", "m", "", 0.5, 1e-6, 1e4),
        P("angle", "Wedge angle", "°", "5 or less", 5, 0.1, 10),
        P("Nr", "Cells from axis to wall", "", "", 20, 1, 10000),
        P("Nl", "Cells along", "", "", 100, 1, 100000),
        P("first", "First cell at the wall", "m", "0: even cells", 0, 0, 1e4) }, "inlet, outlet (patch), wall (wall), front, back (wedge), axis (empty, no faces)" },
};
#undef P

int bm_template_par(int t, const char *key)
{
    if (t < 0 || t >= BT__N) return -1;
    for (int i = 0; i < BM_TEMPLATES[t].nPar; i++) if (!strcmp(BM_TEMPLATES[t].par[i].key, key)) return i;
    return -1;
}

static double first_cell(double L, int n, double r) { return fabs(r - 1) < 1e-12 ? L / n : L * (r - 1) / (pow(r, n) - 1); }

double bm_ratio_for_first(double L, int n, double d1)
{
    if (!(d1 > 0) || n <= 1 || !(L > 0)) return 1;
    double even = L / n;
    if (d1 >= even * (1 - 1e-12)) return 1;                      /* a "first cell" as big as even ones: even cells, not coarser at the wall */
    double lo = 1, hi = 2;
    while (first_cell(L, n, hi) > d1 && hi < 1e6) hi *= 2;
    for (int i = 0; i < 200; i++) {
        double mid = 0.5 * (lo + hi);
        if (first_cell(L, n, mid) > d1) lo = mid; else hi = mid;
    }
    return pow(0.5 * (lo + hi), n - 1);
}

/* ---- building blocks for the templates */
static int vtx(BmModel *m, double x, double y, double z)
{
    if (m->nv >= BM_MAXV) return 0;
    m->v[m->nv][0] = x; m->v[m->nv][1] = y; m->v[m->nv][2] = z;
    return m->nv++;
}

static void grade1(BmGrade *g, double R) { memset(g, 0, sizeof *g); g->n = 1; g->len[0] = g->cells[0] = 1; g->ratio[0] = R; }
/* fine at both ends, coarse in the middle: two halves, each growing from its wall */
static void grade2(BmGrade *g, double R)
{
    memset(g, 0, sizeof *g);
    if (fabs(R - 1) < 1e-12) { grade1(g, 1); return; }
    g->n = 2; g->len[0] = g->len[1] = g->cells[0] = g->cells[1] = 0.5; g->ratio[0] = R; g->ratio[1] = 1 / R;
}

static int hex(BmModel *m, const int v[8], int n1, int n2, int n3, double r1, double r2, double r3)
{
    if (m->nb >= BM_MAXB) return -1;
    BmBlock *b = &m->b[m->nb];
    memset(b, 0, sizeof *b);
    memcpy(b->v, v, sizeof b->v);
    b->n[0] = n1; b->n[1] = n2; b->n[2] = n3;
    b->kind = BG_SIMPLE;
    grade1(&b->g[0], r1); grade1(&b->g[1], r2); grade1(&b->g[2], r3);
    return m->nb++;
}

/* a block to edgeGrading: each of the 12 edges its own ratio */
static void edge_grades(BmModel *m, int b, const double r[12])
{
    m->b[b].kind = BG_EDGE;
    for (int k = 0; k < 12; k++) grade1(&m->b[b].g[k], r[k]);
}

static int patch(BmModel *m, const char *name, const char *type)
{
    for (int i = 0; i < m->np; i++) if (!strcmp(m->p[i].name, name)) return i;
    if (m->np >= BM_MAXP) return 0;
    BmPatch *p = &m->p[m->np];
    memset(p, 0, sizeof *p);
    snprintf(p->name, sizeof p->name, "%s", name);
    snprintf(p->type, sizeof p->type, "%s", type);
    return m->np++;
}

static void face(BmModel *m, int p, int a, int b, int c, int d)
{
    BmPatch *pa = &m->p[p];
    if (pa->nf >= BM_MAXF) return;
    pa->f[pa->nf][0] = a; pa->f[pa->nf][1] = b; pa->f[pa->nf][2] = c; pa->f[pa->nf][3] = d;
    pa->nf++;
}

static void edge_arc(BmModel *m, int a, int b, double x, double y, double z)
{
    if (m->ne >= BM_MAXE) return;
    BmEdge *e = &m->e[m->ne++];
    memset(e, 0, sizeof *e);
    e->kind = BE_ARC; e->a = a; e->b = b; e->np = 1;
    e->p[0][0] = x; e->p[0][1] = y; e->p[0][2] = z;
}

/* ---- the 2-D templates: points in the x-y plane, each a vertex at z = 0 (2i) and at z = depth (2i + 1) */
typedef struct { BmModel *m; int np; double depth; } Flat;
static int pt(Flat *F, double x, double y) { vtx(F->m, x, y, 0); vtx(F->m, x, y, F->depth); return F->np++; }
/* a block on the counter-clockwise quad a b c d: x1 along a-b, x2 along a-d (b-c), one cell deep */
static int quad(Flat *F, int a, int b, int c, int d, int n1, int n2, double r1, double r2)
{
    int v[8] = { 2 * a, 2 * b, 2 * c, 2 * d, 2 * a + 1, 2 * b + 1, 2 * c + 1, 2 * d + 1 };
    return hex(F->m, v, n1, n2, 1, r1, r2, 1);
}
static void side(Flat *F, int p, int a, int b) { face(F->m, p, 2 * a, 2 * b, 2 * b + 1, 2 * a + 1); }   /* outward for a counter-clockwise a -> b */
static void front_back(Flat *F, int p, int a, int b, int c, int d)
{
    face(F->m, p, 2 * a, 2 * d, 2 * c, 2 * b);
    face(F->m, p, 2 * a + 1, 2 * b + 1, 2 * c + 1, 2 * d + 1);
}
static void arc2(Flat *F, int a, int b, double x, double y)
{
    edge_arc(F->m, 2 * a, 2 * b, x, y, 0);
    edge_arc(F->m, 2 * a + 1, 2 * b + 1, x, y, F->depth);
}
static void spline2(Flat *F, int a, int b, const double (*xy)[2], int n)
{
    for (int z = 0; z < 2 && F->m->ne < BM_MAXE; z++) {
        BmEdge *e = &F->m->e[F->m->ne++];
        memset(e, 0, sizeof *e);
        e->kind = BE_SPLINE; e->a = 2 * a + z; e->b = 2 * b + z;
        for (int i = 0; i < n && e->np < BM_MAXPTS; i++) { e->p[e->np][0] = xy[i][0]; e->p[e->np][1] = xy[i][1]; e->p[e->np][2] = z ? F->depth : 0; e->np++; }
    }
}

static int cnt(double v) { return v < 1 ? 1 : (int)lround(v); }

/* NACA four-digit, closed trailing edge (a4 = -0.1036): the upper (s = 1) or lower (s = -1) surface at x / c */
static void naca(double t, double m, double p, double xc, int s, double *x, double *y)
{
    double yt = 5 * t * (0.2969 * sqrt(xc) - 0.1260 * xc - 0.3516 * xc * xc + 0.2843 * xc * xc * xc - 0.1036 * xc * xc * xc * xc);
    double yc = 0, dy = 0;
    if (m > 0 && p > 0) {
        if (xc < p) { yc = m / (p * p) * (2 * p * xc - xc * xc); dy = 2 * m / (p * p) * (p - xc); }
        else { yc = m / ((1 - p) * (1 - p)) * (1 - 2 * p + 2 * p * xc - xc * xc); dy = 2 * m / ((1 - p) * (1 - p)) * (p - xc); }
    }
    double th = atan(dy);
    *x = xc - s * yt * sin(th);
    *y = yc + s * yt * cos(th);
}

bool bm_template(int t, const double *val, BmModel *m, char *err, int cap)
{
    bm_init(m);
    char spare[8];
    if (!err) { err = spare; cap = sizeof spare; }
    err[0] = 0;
    if (t < 0 || t >= BT__N) { snprintf(err, (size_t)cap, "No such template"); return false; }
    const BmTemplate *T = &BM_TEMPLATES[t];
    double v[BT_MAXPAR];
    for (int i = 0; i < T->nPar; i++) {
        v[i] = val ? val[i] : T->par[i].def;
        if (!(v[i] >= T->par[i].lo && v[i] <= T->par[i].hi)) {
            snprintf(err, (size_t)cap, "%s is %g: give %g to %g", T->par[i].label, v[i], T->par[i].lo, T->par[i].hi);
            return false;
        }
    }
#define V(k) v[bm_template_par(t, k)]
    Flat F = { m, 0, 0 };
    switch (t) {
    case BT_CAVITY: {
        double L = V("L"); int N = cnt(V("N")); F.depth = V("depth");
        double R = bm_ratio_for_first(L / 2, N / 2, V("first"));
        int a = pt(&F, 0, 0), b = pt(&F, L, 0), c = pt(&F, L, L), d = pt(&F, 0, L);
        int k = quad(&F, a, b, c, d, N, N, 1, 1);
        grade2(&m->b[k].g[0], R); grade2(&m->b[k].g[1], R);
        int lid = patch(m, "movingWall", "wall"), fw = patch(m, "fixedWalls", "wall"), fb = patch(m, "frontAndBack", "empty");
        side(&F, lid, c, d); side(&F, fw, a, b); side(&F, fw, b, c); side(&F, fw, d, a);
        front_back(&F, fb, a, b, c, d);
        break;
    }
    case BT_CHANNEL: {
        double L = V("L"), H = V("H"), W = V("W"), d1 = V("first");
        int Nx = cnt(V("Nx")), Ny = cnt(V("Ny")), Nz = cnt(V("Nz"));
        double Ry = bm_ratio_for_first(H / 2, Ny / 2, d1);
        int in = patch(m, "inlet", "patch"), out = patch(m, "outlet", "patch"), wl = patch(m, "walls", "wall");
        if (W <= 0) {
            F.depth = V("depth");
            int a = pt(&F, 0, 0), b = pt(&F, L, 0), c = pt(&F, L, H), d = pt(&F, 0, H);
            int k = quad(&F, a, b, c, d, Nx, Ny, 1, 1);
            grade2(&m->b[k].g[1], Ry);
            side(&F, wl, a, b); side(&F, out, b, c); side(&F, wl, c, d); side(&F, in, d, a);
            front_back(&F, patch(m, "frontAndBack", "empty"), a, b, c, d);
        } else {
            double Rz = bm_ratio_for_first(W / 2, Nz / 2, d1);
            int p[8] = { vtx(m, 0, 0, 0), vtx(m, L, 0, 0), vtx(m, L, H, 0), vtx(m, 0, H, 0), vtx(m, 0, 0, W), vtx(m, L, 0, W), vtx(m, L, H, W), vtx(m, 0, H, W) };
            int k = hex(m, p, Nx, Ny, Nz, 1, 1, 1);
            grade2(&m->b[k].g[1], Ry); grade2(&m->b[k].g[2], Rz);
            face(m, in, p[0], p[4], p[7], p[3]); face(m, out, p[1], p[2], p[6], p[5]);
            face(m, wl, p[0], p[1], p[5], p[4]); face(m, wl, p[3], p[7], p[6], p[2]);
            face(m, wl, p[0], p[3], p[2], p[1]); face(m, wl, p[4], p[5], p[6], p[7]);
        }
        break;
    }
    case BT_STEP: {
        double h = V("h"), Hi = V("Hi"), Lu = V("Lu"), Ld = V("Ld"), d1 = V("first"); F.depth = V("depth");
        int Nh = cnt(V("Nh")), Ni = cnt(Nh * Hi / h), Nu = cnt(Nh * Lu / h), Nd = cnt(Nh * Ld / h);
        double Ri = bm_ratio_for_first(Hi / 2, Ni / 2, d1), Rh = bm_ratio_for_first(h, Nh, d1);
        int p0 = pt(&F, -Lu, h), p1 = pt(&F, 0, h), p2 = pt(&F, 0, h + Hi), p3 = pt(&F, -Lu, h + Hi);
        int p4 = pt(&F, 0, 0), p5 = pt(&F, Ld, 0), p6 = pt(&F, Ld, h), p7 = pt(&F, Ld, h + Hi);
        int A = quad(&F, p0, p1, p2, p3, Nu, Ni, 1, 1);           /* the inlet channel */
        int B = quad(&F, p4, p5, p6, p1, Nd, Nh, 1, Rh);          /* below the step's top, after it: fine at the floor */
        int C = quad(&F, p1, p6, p7, p2, Nd, Ni, 1, 1);           /* above, after it */
        grade2(&m->b[A].g[1], Ri); grade2(&m->b[C].g[1], Ri);     /* the same across the face A and C share */
        (void)B;
        int in = patch(m, "inlet", "patch"), out = patch(m, "outlet", "patch"), up = patch(m, "upperWall", "wall"), lo = patch(m, "lowerWall", "wall");
        int fb = patch(m, "frontAndBack", "empty");
        side(&F, in, p3, p0); side(&F, lo, p0, p1); side(&F, up, p2, p3);
        side(&F, lo, p4, p5); side(&F, out, p5, p6); side(&F, lo, p1, p4);
        side(&F, out, p6, p7); side(&F, up, p7, p2);
        front_back(&F, fb, p0, p1, p2, p3); front_back(&F, fb, p4, p5, p6, p1); front_back(&F, fb, p1, p6, p7, p2);
        break;
    }
    case BT_PLATE: {
        double Lp = V("Lp"), Lu = V("Lu"), H = V("H"), d1 = V("first"); F.depth = V("depth");
        int Nx = cnt(V("Nx")), Ny = cnt(V("Ny")), Nu = Lu > 0 ? cnt(fmax(4, Nx * Lu / Lp * 0.5)) : 0;
        double Ry = bm_ratio_for_first(H, Ny, d1);
        int in = patch(m, "inlet", "patch"), out = patch(m, "outlet", "patch"), top = patch(m, "top", "patch"), pl = patch(m, "plate", "wall");
        int sym = Lu > 0 ? patch(m, "symmetry", "symmetryPlane") : -1, fb = patch(m, "frontAndBack", "empty");
        int p1 = pt(&F, 0, 0), p2 = pt(&F, Lp, 0), p3 = pt(&F, Lp, H), p4 = pt(&F, 0, H);
        quad(&F, p1, p2, p3, p4, Nx, Ny, 1, Ry);
        side(&F, pl, p1, p2); side(&F, out, p2, p3); side(&F, top, p3, p4);
        front_back(&F, fb, p1, p2, p3, p4);
        if (Lu > 0) {
            /* ahead of the plate the cells grow away from its leading edge, matching it there */
            int p0 = pt(&F, -Lu, 0), p5 = pt(&F, -Lu, H);
            double first = Lp / Nx, Rx = bm_ratio_for_first(Lu, Nu, first);
            quad(&F, p0, p1, p4, p5, Nu, Ny, 1 / Rx, Ry);
            side(&F, sym, p0, p1); side(&F, top, p4, p5); side(&F, in, p5, p0);
            front_back(&F, fb, p0, p1, p4, p5);
        } else side(&F, in, p4, p1);
        break;
    }
    case BT_PIPE: {
        double R = V("D") / 2, L = V("L"), core = V("core"), d1 = V("first");
        int Nq = cnt(V("Nq")), Nr = cnt(V("Nr")), Nl = cnt(V("Nl"));
        double rc = core * R;                                       /* the core's corners on this radius, at 45 degrees */
        double Rr = bm_ratio_for_first(R - rc * 0.85, Nr, d1);     /* wall at the far end of x1: graded toward it */
        F.depth = L;
        double ang[4] = { 225, 315, 45, 135 };
        int I[4], O[4];
        for (int k = 0; k < 4; k++) {
            double a = ang[k] * M_PI / 180;
            I[k] = pt(&F, rc * cos(a), rc * sin(a));
            O[k] = pt(&F, R * cos(a), R * sin(a));
        }
        /* along z: the 2-D trick with depth L and Nl cells */
        int kc = quad(&F, I[0], I[1], I[2], I[3], Nq, Nq, 1, 1);
        m->b[kc].n[2] = Nl;
        int in = patch(m, "inlet", "patch"), out = patch(m, "outlet", "patch"), wl = patch(m, "wall", "wall");
        face(m, in, 2 * I[0], 2 * I[3], 2 * I[2], 2 * I[1]); face(m, out, 2 * I[0] + 1, 2 * I[1] + 1, 2 * I[2] + 1, 2 * I[3] + 1);
        for (int k = 0; k < 4; k++) {
            int k1 = (k + 1) % 4;
            int kb = quad(&F, I[k], O[k], O[k1], I[k1], Nr, Nq, 1 / Rr, 1);
            m->b[kb].n[2] = Nl;
            side(&F, wl, O[k], O[k1]);
            face(m, in, 2 * I[k], 2 * I[k1], 2 * O[k1], 2 * O[k]);
            face(m, out, 2 * I[k] + 1, 2 * O[k] + 1, 2 * O[k1] + 1, 2 * I[k1] + 1);
            double am = (ang[k] + 45) * M_PI / 180;
            arc2(&F, O[k], O[k1], R * cos(am), R * sin(am));
        }
        break;
    }
    case BT_CYLINDER: {
        double R = V("D") / 2, S = V("ring") * V("D") / 2, Lu = V("Lu"), Ld = V("Ld"), Hh = V("Hh"), d1 = V("first"); F.depth = V("depth");
        if (Lu <= S * 1.05 || Ld <= S * 1.05 || Hh <= S * 1.05) {
            snprintf(err, (size_t)cap, "The ring's square (%.3g m across) must sit inside the domain: make the extents larger or the ring smaller", 2 * S);
            return false;
        }
        int Nq = cnt(V("Nq")), Nr = cnt(V("Nr"));
        double cell = 2 * S / Nq;                                   /* the cells at the square's edge */
        int Nu = cnt((Lu - S) / cell * 0.6), Nd = cnt((Ld - S) / cell * 0.6), Nh = cnt((Hh - S) / cell * 0.6);
        double Rr = bm_ratio_for_first(S - R * 0.85, Nr, d1);
        /* the outer grid: x at -Lu, -S, S, Ld; y at -Hh, -S, S, Hh */
        double X[4] = { -Lu, -S, S, Ld }, Y[4] = { -Hh, -S, S, Hh };
        int G[4][4];
        for (int j = 0; j < 4; j++) for (int i = 0; i < 4; i++) G[i][j] = pt(&F, X[i], Y[j]);
        int Cq[4], Q[4] = { G[2][2], G[1][2], G[1][1], G[2][1] };
        double ang[4] = { 45, 135, 225, 315 };
        for (int k = 0; k < 4; k++) Cq[k] = pt(&F, R * cos(ang[k] * M_PI / 180), R * sin(ang[k] * M_PI / 180));
        int cy = patch(m, "cylinder", "wall"), in = patch(m, "inlet", "patch"), out = patch(m, "outlet", "patch"), sd = patch(m, "sides", "symmetry");   /* top and bottom: two planes, so not symmetryPlane (one plane only) */
        int fb = patch(m, "frontAndBack", "empty");
        for (int k = 0; k < 4; k++) {
            int k1 = (k + 1) % 4;
            quad(&F, Cq[k], Q[k], Q[k1], Cq[k1], Nr, Nq, Rr, 1);
            side(&F, cy, Cq[k1], Cq[k]);
            front_back(&F, fb, Cq[k], Q[k], Q[k1], Cq[k1]);
            double am = (ang[k] + 45) * M_PI / 180;
            arc2(&F, Cq[k], Cq[k1], R * cos(am), R * sin(am));
        }
        int nx[3] = { Nu, Nq, Nd }, ny[3] = { Nh, Nq, Nh };
        /* the outer blocks grow away from the square, their first cell the square's */
        double rx[3] = { 1 / bm_ratio_for_first(Lu - S, Nu, cell), 1, bm_ratio_for_first(Ld - S, Nd, cell) };
        double ry[3] = { 1 / bm_ratio_for_first(Hh - S, Nh, cell), 1, bm_ratio_for_first(Hh - S, Nh, cell) };
        for (int j = 0; j < 3; j++)
            for (int i = 0; i < 3; i++) {
                if (i == 1 && j == 1) continue;
                quad(&F, G[i][j], G[i + 1][j], G[i + 1][j + 1], G[i][j + 1], nx[i], ny[j], rx[i], ry[j]);
                front_back(&F, fb, G[i][j], G[i + 1][j], G[i + 1][j + 1], G[i][j + 1]);
                if (i == 0) side(&F, in, G[0][j + 1], G[0][j]);
                if (i == 2) side(&F, out, G[3][j], G[3][j + 1]);
                if (j == 0) side(&F, sd, G[i][0], G[i + 1][0]);
                if (j == 2) side(&F, sd, G[i + 1][3], G[i][3]);
            }
        break;
    }
    case BT_AIRFOIL: {
        double tk_ = V("t") / 100, cm = V("camber") / 100, cp = V("pos") / 10, c = V("c"), aoa = V("aoa") * M_PI / 180;
        double R = V("R") * c, W = V("W") * c, d1 = V("first"); F.depth = V("depth");
        int Nc = cnt(V("Nc")), Nn = cnt(V("Nn")), Nw = cnt(V("Nw"));
        const double split = 0.3;                                   /* each surface split here (x / c): no two edges on one vertex pair */
        int Ncf = cnt(Nc * 0.45), Ncr = Nc - Ncf < 2 ? 2 : Nc - Ncf;
        double Rn = bm_ratio_for_first(R, Nn, d1);
        /* the airfoil turned by the angle of attack about the quarter chord, nose up */
        double ca = cos(-aoa), sa = sin(-aoa);
#define TURN(px, py, ox, oy) do { double _x = (px) - 0.25 * c, _y = (py); ox = 0.25 * c + _x * ca - _y * sa; oy = _x * sa + _y * ca; } while (0)
        double x, y, X, Y;
        TURN(0, 0, X, Y); int LE = pt(&F, X, Y);
        TURN(c, 0, X, Y); int TE = pt(&F, X, Y);
        naca(tk_, cm, cp, split, 1, &x, &y); TURN(x * c, y * c, X, Y); int U1 = pt(&F, X, Y);
        naca(tk_, cm, cp, split, -1, &x, &y); TURN(x * c, y * c, X, Y); int L1 = pt(&F, X, Y);
        int D = pt(&F, c - R, 0), Cu = pt(&F, c, R), Cl = pt(&F, c, -R);
        int Gu = pt(&F, c - R / sqrt(2), R / sqrt(2)), Gl = pt(&F, c - R / sqrt(2), -R / sqrt(2));
        int E = pt(&F, c + W, 0), Fu = pt(&F, c + W, R), Fl = pt(&F, c + W, -R);
        /* the surfaces as splines through points spaced closer at the nose */
        for (int s = 1; s >= -1; s -= 2) {
            double seg[2][2] = { { 0, split }, { split, 1 } };
            for (int part = 0; part < 2; part++) {
                double xy[40][2]; int n = 0;
                for (int i = 1; i < 40 && n < 39; i++) {
                    double u = (double)i / 40, xc = seg[part][0] + (seg[part][1] - seg[part][0]) * (part == 0 ? 1 - cos(u * M_PI / 2) : u);
                    naca(tk_, cm, cp, xc, s, &x, &y);
                    TURN(x * c, y * c, X, Y);
                    xy[n][0] = X; xy[n][1] = Y; n++;
                }
                int a = part == 0 ? LE : s > 0 ? U1 : L1, b = part == 0 ? (s > 0 ? U1 : L1) : TE;
                spline2(&F, a, b, (const double (*)[2])xy, n);
            }
        }
#undef TURN
        double first_w = (1 - split) * c / Ncr, Rw = bm_ratio_for_first(W, Nw, first_w);
        int af = patch(m, "airfoil", "wall"), ff = patch(m, "farfield", "patch"), out = patch(m, "outlet", "patch"), fb = patch(m, "frontAndBack", "empty");
        /* upper: front (LE U1 Gu D), rear (U1 TE Cu Gu), wake (TE E Fu Cu) */
        quad(&F, LE, U1, Gu, D, Ncf, Nn, 1, Rn);
        quad(&F, U1, TE, Cu, Gu, Ncr, Nn, 1, Rn);
        int wu = quad(&F, TE, E, Fu, Cu, Nw, Nn, Rw, Rn);
        /* lower: front (LE D Gl L1), rear (L1 Gl Cl TE), wake (TE Cl Fl E) */
        quad(&F, LE, D, Gl, L1, Nn, Ncf, Rn, 1);
        quad(&F, L1, Gl, Cl, TE, Nn, Ncr, Rn, 1);
        int wl = quad(&F, TE, Cl, Fl, E, Nn, Nw, Rn, Rw);
        /* the wake's thin cells at the centreline would end beside its long last cells (aspect ratio 20 000 at the outlet):
           at the outlet the cells across start at a 300th of the wake's last cell, at the trailing edge as on the airfoil */
        double dxEnd = first_w * Rw, Rout = bm_ratio_for_first(R, Nn, fmax(d1, dxEnd / 300));
        double gu[12] = { Rw, Rw, Rw, Rw, Rn, Rout, Rout, Rn, 1, 1, 1, 1 };
        double gl[12] = { Rn, Rout, Rout, Rn, Rw, Rw, Rw, Rw, 1, 1, 1, 1 };
        edge_grades(m, wu, gu); edge_grades(m, wl, gl);
        side(&F, af, LE, U1); side(&F, af, U1, TE); side(&F, af, L1, LE); side(&F, af, TE, L1);
        side(&F, ff, Gu, D); side(&F, ff, Cu, Gu); side(&F, ff, Fu, Cu);
        side(&F, ff, D, Gl); side(&F, ff, Gl, Cl); side(&F, ff, Cl, Fl);
        side(&F, out, E, Fu); side(&F, out, Fl, E);
        front_back(&F, fb, LE, U1, Gu, D); front_back(&F, fb, U1, TE, Cu, Gu); front_back(&F, fb, TE, E, Fu, Cu);
        front_back(&F, fb, LE, D, Gl, L1); front_back(&F, fb, L1, Gl, Cl, TE); front_back(&F, fb, TE, Cl, Fl, E);
        /* the far field: arcs about (c, 0) */
        arc2(&F, Gu, D, c - R * cos(M_PI / 8), R * sin(M_PI / 8));
        arc2(&F, Cu, Gu, c - R * sin(M_PI / 8), R * cos(M_PI / 8));
        arc2(&F, D, Gl, c - R * cos(M_PI / 8), -R * sin(M_PI / 8));
        arc2(&F, Gl, Cl, c - R * sin(M_PI / 8), -R * cos(M_PI / 8));
        break;
    }
    case BT_WEDGE: {
        double R = V("D") / 2, L = V("L"), h = V("angle") * M_PI / 360, d1 = V("first");
        int Nr = cnt(V("Nr")), Nl = cnt(V("Nl"));
        double Rr = bm_ratio_for_first(R, Nr, d1);
        double cc = R * cos(h), ss = R * sin(h);
        /* the axis along x; the wedge either side of the x-y plane; the axis edge collapsed (blockMesh drops its faces) */
        int a0 = vtx(m, 0, 0, 0), a1 = vtx(m, L, 0, 0), w1 = vtx(m, L, cc, -ss), w0 = vtx(m, 0, cc, -ss), e1 = vtx(m, L, cc, ss), e0 = vtx(m, 0, cc, ss);
        int vv[8] = { a0, a1, w1, w0, a0, a1, e1, e0 };
        hex(m, vv, Nl, Nr, 1, 1, 1 / Rr, 1);
        int in = patch(m, "inlet", "patch"), out = patch(m, "outlet", "patch"), wl = patch(m, "wall", "wall");
        int fr = patch(m, "front", "wedge"), bk = patch(m, "back", "wedge"), ax = patch(m, "axis", "empty");
        face(m, in, a0, w0, e0, a0); face(m, out, a1, e1, w1, a1);
        face(m, wl, w0, w1, e1, e0);
        face(m, bk, a0, a1, w1, w0); face(m, fr, a0, e0, e1, a1);
        face(m, ax, a0, a1, a1, a0);
        edge_arc(m, w0, e0, 0, R, 0);                             /* the arcs through (x, R, 0), on the plane between the sides */
        edge_arc(m, w1, e1, L, R, 0);
        break;
    }
    }
#undef V
    return true;
}
