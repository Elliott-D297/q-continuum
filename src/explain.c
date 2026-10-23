/* explain.c -- OpenFOAM error messages in plain words (see explain.h) */
#define _GNU_SOURCE
#include <math.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include "explain.h"
#include "foamdict.h"
#include "fieldinfo.h"
#include "physics.h"

static double num(FDFile *f, const char *key, double def)
{
    char v[64];
    return f && fd_get(f, key, v, sizeof v) && pl_is_number(v) ? atof(v) : def;
}

static int line_of(FDFile *f, const char *key)
{
    FDEntry *e = f ? fd_find(f, key) : NULL;
    return e ? e->line : 0;
}

/* "No times selected": reconstructPar (or a -latestTime tool) found no time folders, because the solver wrote none */
static bool no_times(const char *casePath, const char *logName, Explanation *x)
{
    char p[PL_PATH]; pl_join(p, sizeof p, casePath, "system/controlDict");
    FDFile *cd = fd_load(p);
    double st = num(cd, "startTime", 0), et = num(cd, "endTime", -1), dt = num(cd, "deltaT", -1), wi = num(cd, "writeInterval", -1);
    char wc[64] = "timeStep"; if (cd) fd_get(cd, "writeControl", wc, sizeof wc);
    const char *tool = strstr(logName, "reconstructPar") ? "reconstructPar" : strstr(logName, "decomposePar") ? "decomposePar" : "This tool";
    snprintf(x->title, sizeof x->title, "No results were written");
    double first = -1;
    if (!strcmp(wc, "timeStep") && dt > 0 && wi > 0) first = st + wi * dt;
    else if ((!strcmp(wc, "runTime") || !strcmp(wc, "adjustable") || !strcmp(wc, "adjustableRunTime")) && wi > 0) first = st + wi;
    if (first > 0 && et >= 0 && first > et * (1 + 1e-9)) {
        if (!strcmp(wc, "timeStep"))
            snprintf(x->text, sizeof x->text, "%s found no time folders to work on: the solver wrote none. The run ends at t = %g s, but the "
                     "first write comes after writeInterval %g steps × deltaT %g s = t = %g s. Set writeInterval to at most %d steps, or "
                     "run to a later end time.", tool, et, wi, dt, first, (int)fmax(1, floor((et - st) / dt)));
        else
            snprintf(x->text, sizeof x->text, "%s found no time folders to work on: the solver wrote none. The run ends at t = %g s, but with "
                     "writeControl %s the first write is at t = %g + %g = %g s. Set writeInterval to at most %g s, or run to a later end "
                     "time.", tool, et, wc, st, wi, first, et - st);
        pl_strlcpy(x->file, "system/controlDict", sizeof x->file);
        pl_strlcpy(x->key, "writeInterval", sizeof x->key);
        x->line = line_of(cd, "writeInterval");
    } else {
        snprintf(x->text, sizeof x->text, "%s found no time folders to work on. The solver wrote no results, or they were removed: check "
                 "that the solver ran (its log ends with End) and that writeInterval in system/controlDict is reached before the end "
                 "time.", tool);
        pl_strlcpy(x->file, "system/controlDict", sizeof x->file);
        pl_strlcpy(x->key, "writeInterval", sizeof x->key);
        x->line = line_of(cd, "writeInterval");
    }
    fd_free(cd);
    return true;
}

/* 'error in IOstream "constant/polyMesh/faces" for operation Foam::Ostream& ...': a write failed part-way. OpenFOAM
   doesn't say why; on a desktop it is nearly always a full disk (or quota), so say how much room the case's disk has */
static bool write_failed(const char *casePath, const char *msg, Explanation *x)
{
    char file[160] = "";
    const char *q = strchr(msg, '"');
    if (q) { const char *e = strchr(q + 1, '"'); if (e && e - q - 1 < (long)sizeof file) { memcpy(file, q + 1, (size_t)(e - q - 1)); file[e - q - 1] = 0; } }
    const char *rel = file;
    size_t cl = strlen(casePath);
    if (!strncmp(rel, casePath, cl) && rel[cl] == '/') rel += cl + 1;
    snprintf(x->title, sizeof x->title, "Writing %.100s failed", rel[0] ? rel : "a file");
    struct statvfs v;
    char room[160] = "";
    if (statvfs(casePath, &v) == 0) {
        double free = (double)v.f_bavail * (double)v.f_frsize;
        if (free < 1e9) snprintf(room, sizeof room, " The disk holding the case has %.0f MB free.", free / 1e6);
        else snprintf(room, sizeof room, " The disk holding the case has %.1f GB free now.", free / 1e9);
    }
    snprintf(x->text, sizeof x->text, "OpenFOAM could not finish writing %s: the disk is full, a quota was reached, or the folder "
             "stopped being writable.%s Free some space (old time folders, processor* folders, the Trash on the History page) "
             "and run again; the half-written file is replaced on the next write.", rel[0] ? rel : "its output", room);
    return true;
}

/* ============================================================================ the error translator (3.2)
   Each entry was written against the message real OpenFOAM v2606 (and v2412) prints for that mistake: tests/logs/fatal_*.log */

static int edit_distance(const char *a, const char *b)
{
    int n = (int)strlen(a), m = (int)strlen(b);
    if (n > 60 || m > 60) return 99;
    int d[61][61];
    for (int i = 0; i <= n; i++) d[i][0] = i;
    for (int j = 0; j <= m; j++) d[0][j] = j;
    for (int i = 1; i <= n; i++)
        for (int j = 1; j <= m; j++) {
            int c = d[i - 1][j - 1] + (a[i - 1] != b[j - 1]);
            if (d[i - 1][j] + 1 < c) c = d[i - 1][j] + 1;
            if (d[i][j - 1] + 1 < c) c = d[i][j - 1] + 1;
            d[i][j] = c;
        }
    return d[n][m];
}

const char *closest_word(const char *w, const char *const *words, int n)
{
    const char *best = NULL; int bd = 99;
    for (int i = 0; i < n; i++) { int d = edit_distance(w, words[i]); if (d < bd) { bd = d; best = words[i]; } }
    return best && bd <= (int)(strlen(w) + 1) / 2 ? best : NULL;
}

static char *read_log(const char *casePath, const char *logName)
{
    if (!logName || !logName[0]) return NULL;
    char p[PL_PATH]; pl_join(p, sizeof p, casePath, logName);
    return pl_read_file(p, NULL);
}

/* "file: system/fvSchemes/divSchemes at line 30." after the FATAL line: the dictionary path and the line */
static bool log_file_line(const char *log, char *path, int cap, int *line)
{
    const char *f = log ? strstr(log, "FOAM FATAL") : NULL;
    if (f) f = strstr(f, "\nfile: ");
    if (!f) return false;
    f += 7;
    int k = 0;
    while (f[k] && f[k] != ' ' && f[k] != '\n' && k < cap - 1) { path[k] = f[k]; k++; }
    path[k] = 0;
    const char *l = strstr(f, " at line ");
    *line = l && l < strchr(f, '\n') ? atoi(l + 9) : 0;
    return k > 0;
}

/* "Valid patchField types :\n\n79\n(\nname\n...\n)": the names, pointers into a static buffer */
static int log_valid_list(const char *log, const char *marker, char ***out)
{
    static char *buf; static char *names[4096];
    free(buf); buf = NULL;
    const char *v = log ? strstr(log, marker) : NULL;
    if (!v) return 0;
    const char *o = strchr(v, '(');
    const char *c = o ? strstr(o, "\n)") : NULL;
    const char *c1 = o ? strchr(o, ')') : NULL, *nl = o ? strchr(o, '\n') : NULL;
    if (c1 && (!nl || c1 < nl)) c = c1;                       /* on one line: "7(FPCG GAMG PBiCGStab PCG ...)" (H-09) */
    if (!o || !c) return 0;
    buf = pl_strndup(o + 1, (size_t)(c - o - 1));
    int n = 0;
    for (char *t = strtok(buf, " \n\t"); t && n < 4096; t = strtok(NULL, " \n\t")) names[n++] = t;
    *out = names;
    return n;
}

/* a dictionary path from the log ("0/U/boundaryField", or an absolute ".../system/controlDict") split into the file
   (relative to the case) and the path inside it */
static void split_dict_path(const char *casePath, const char *dp, char *file, int fc, char *inside, int ic)
{
    char rel[PL_PATH]; pl_strlcpy(rel, dp, sizeof rel);
    size_t cl = strlen(casePath);
    if (!strncmp(rel, casePath, cl) && rel[cl] == '/') memmove(rel, rel + cl + 1, strlen(rel + cl + 1) + 1);
    const char *sl = strchr(rel, '/');
    file[0] = inside[0] = 0;
    for (const char *p = sl; p; p = strchr(p + 1, '/')) {      /* the longest leading part that is a file */
        char f[PL_PATH]; pl_fmt_ui(f, sizeof f, "%.*s", (int)(p - rel), rel);
        char full[PL_PATH]; pl_join(full, sizeof full, casePath, f);
        if (pl_is_file(full)) { pl_fmt_ui(file, (size_t)fc, "%s", f); pl_fmt_ui(inside, (size_t)ic, "%s", p + 1); }
    }
    char full[PL_PATH]; pl_join(full, sizeof full, casePath, rel);
    if (pl_is_file(full)) { pl_fmt_ui(file, (size_t)fc, "%s", rel); inside[0] = 0; }
}

static FDFile *load_rel(const char *casePath, const char *rel)
{
    char p[PL_PATH]; pl_join(p, sizeof p, casePath, rel);
    return fd_load(p);
}

static void put1(Explanation *x, const char *path, const char *val)
{
    if (x->nPut >= 3) return;
    pl_strlcpy(x->putPath[x->nPut], path, sizeof x->putPath[0]);
    pl_strlcpy(x->putVal[x->nPut], val, sizeof x->putVal[0]);
    x->nPut++;
}

/* "Cannot find patchField entry for movingWall" */
static bool missing_patch(const char *casePath, const char *log, const char *msg, Explanation *x)
{
    char patch[128] = "";
    sscanf(strstr(msg, "entry for ") + 10, "%127s", patch);
    char dp[300] = "", file[160], inside[200]; int line = 0;
    log_file_line(log, dp, sizeof dp, &line);
    split_dict_path(casePath, dp, file, sizeof file, inside, sizeof inside);
    pl_fmt_ui(x->title, sizeof x->title, "%s has no condition for patch %s", file[0] ? file : "A field", patch);
    pl_strlcpy(x->file, file, sizeof x->file); x->line = line; pl_strlcpy(x->key, patch, sizeof x->key);
    FDFile *f = file[0] ? load_rel(casePath, file) : NULL;
    FDEntry *bf = f ? fd_find(f, "boundaryField") : NULL;
    const char *names[256] = { 0 }; int n = 0;
    for (int i = 0; bf && i < bf->nChild && n < 256; i++) if (bf->child[i].kind == FD_DICT) names[n++] = bf->child[i].key;
    const char *near = closest_word(patch, names, n);
    bool inMesh = false;
    if (near) {                                               /* a misspelt patch name in the field, unless it's a real patch too */
        char bp[PL_PATH]; pl_join(bp, sizeof bp, casePath, "constant/polyMesh/boundary");
        char *b = pl_read_file(bp, NULL);
        if (b) { char w[140]; pl_fmt_ui(w, sizeof w, "\n    %s\n", near); inMesh = strstr(b, w) != NULL; free(b); }
    }
    if (near && !inMesh) {
        pl_fmt_ui(x->text, sizeof x->text, "Every patch of the mesh needs a condition in every field. %s has none for %s, but it has one "
                 "for %s, which is not a patch of the mesh: most likely the same name misspelt.", file, patch, near);
        x->fix = FIX_RENAME;
        pl_fmt_ui(x->fixPath, sizeof x->fixPath, "boundaryField/%s", near);
        FDEntry *ne = fd_find(f, x->fixPath);
        if (ne) { x->line = ne->line; pl_strlcpy(x->key, near, sizeof x->key); }     /* point at the misspelt entry */
        pl_strlcpy(x->fixValue, patch, sizeof x->fixValue);
        pl_fmt_ui(x->fixLabel, sizeof x->fixLabel, "Rename %s to %s", near, patch);
    } else {
        pl_fmt_ui(x->text, sizeof x->text, "Every patch of the mesh needs a condition in every field, and %s has none for %s. Add one: "
                 "the Boundaries page shows every field on every patch.", file[0] ? file : "the field", patch);
        x->fix = FIX_ADD_BC;
        if (bf) { x->line = bf->line; pl_strlcpy(x->key, "boundaryField", sizeof x->key); }   /* where it goes, not a neighbour (H-20) */
        pl_strlcpy(x->fixValue, patch, sizeof x->fixValue);
        pl_fmt_ui(x->fixLabel, sizeof x->fixLabel, "Add a condition for %s", patch);
    }
    pl_strlcpy(x->fixFile, file, sizeof x->fixFile);
    if (!file[0]) x->fix = FIX_NONE;
    fd_free(f);
    return true;
}

/* "Unknown patchField type zeroGradeint for patch type wall" */
static bool bad_bc_type(const char *casePath, const char *log, const char *msg, Explanation *x)
{
    char type[96] = "", ptype[64] = "";
    sscanf(strstr(msg, "patchField type ") + 16, "%95s", type);
    const char *pt = strstr(msg, "for patch type ");
    if (pt) sscanf(pt + 15, "%63s", ptype);
    char dp[300] = "", file[160], inside[200]; int line = 0;
    log_file_line(log, dp, sizeof dp, &line);
    split_dict_path(casePath, dp, file, sizeof file, inside, sizeof inside);
    char **valid = NULL; int nv = log_valid_list(log, "Valid patchField types", &valid);
    const char *near = closest_word(type, (const char *const *)valid, nv);
    /* which patch: the boundaryField entry whose type is the unknown one */
    char patch[128] = "";
    FDFile *f = file[0] ? load_rel(casePath, file) : NULL;
    FDEntry *bf = f ? fd_find(f, "boundaryField") : NULL;
    for (int i = 0; bf && i < bf->nChild; i++) {
        FDEntry *t = bf->child[i].kind == FD_DICT ? fd_child(&bf->child[i], "type") : NULL;
        char v[96]; if (t && fd_entry_value(f, t, v, sizeof v) && !strcmp(v, type)) { pl_strlcpy(patch, bf->child[i].key, sizeof patch); if (!line) line = t->line; break; }
    }
    pl_fmt_ui(x->title, sizeof x->title, "%s is not a boundary condition OpenFOAM knows", type);
    pl_fmt_ui(x->text, sizeof x->text, "%s%s%s sets type %s, which isn't one of the %d condition types this OpenFOAM has%s%s%s.%s%s%s",
              file[0] ? file : "A field", patch[0] ? " on patch " : "", patch, type, nv, ptype[0] ? " for a " : "", ptype, ptype[0] ? " patch" : "",
              near ? " The closest is " : "", near ? near : "", near ? "." : "");
    pl_strlcpy(x->file, file, sizeof x->file); x->line = line; pl_strlcpy(x->key, "type", sizeof x->key);
    if (near && patch[0] && file[0]) {
        x->fix = FIX_PUT; pl_strlcpy(x->fixFile, file, sizeof x->fixFile);
        char p[300]; pl_fmt_ui(p, sizeof p, "boundaryField/%s/type", patch); put1(x, p, near);
        pl_fmt_ui(x->fixLabel, sizeof x->fixLabel, "Set the type to %s", near);
    }
    fd_free(f);
    return true;
}

/* the value a missing scheme usually takes, by its name and whether the solver is steady */
const char *scheme_default(const char *section, const char *key, bool steady)
{
    if (!strcmp(section, "ddtSchemes")) return steady ? "steadyState" : "Euler";
    if (!strcmp(section, "gradSchemes")) return "Gauss linear";
    if (!strcmp(section, "laplacianSchemes")) return "Gauss linear corrected";
    if (!strcmp(section, "interpolationSchemes")) return "linear";
    if (!strcmp(section, "snGradSchemes")) return "corrected";
    if (!strcmp(section, "divSchemes")) {
        if (strstr(key, "dev") || strstr(key, "nuEff") || strstr(key, "muEff")) return "Gauss linear";
        if (strstr(key, "phi,U")) return steady ? "bounded Gauss linearUpwind grad(U)" : "Gauss linear";
        return steady ? "bounded Gauss upwind" : "Gauss upwind";
    }
    return NULL;
}

/* "Entry 'div(phi,U)' not found in dictionary "system/fvSchemes/divSchemes"" */
static bool missing_entry(const char *casePath, const char *log, const char *msg, Explanation *x)
{
    char key[160] = "", dp[400] = "";
    const char *a = strchr(msg, '\''), *b = a ? strchr(a + 1, '\'') : NULL;
    if (!a || !b || b - a - 1 >= (long)sizeof key) return false;
    pl_fmt_ui(key, sizeof key, "%.*s", (int)(b - a - 1), a + 1);
    const char *q = strchr(b, '"'), *qe = q ? strchr(q + 1, '"') : NULL;
    if (q && qe) pl_fmt_ui(dp, sizeof dp, "%.*s", (int)(qe - q - 1), q + 1);
    char file[160], inside[200];
    split_dict_path(casePath, dp, file, sizeof file, inside, sizeof inside);
    if (!file[0]) return false;
    int line = 0; char lp[300]; if (log_file_line(log, lp, sizeof lp, &line)) {}
    FDFile *cd = load_rel(casePath, "system/controlDict");
    char app[64] = ""; if (cd) fd_get(cd, "application", app, sizeof app);
    bool steady = strstr(app, "simple") || strstr(app, "Simple") || strstr(app, "potential");
    pl_fmt_ui(x->title, sizeof x->title, "%s is missing from %s", key, file);
    char where[260]; pl_fmt_ui(where, sizeof where, "%s%s%s", file, inside[0] ? " under " : "", inside);
    const char *def = !strcmp(file, "system/fvSchemes") ? scheme_default(inside, key, steady) : NULL;
    char path[400]; pl_fmt_ui(path, sizeof path, "%s%s%s", inside, inside[0] ? "/" : "", key);
    char dtv[64] = "";
    if (!def && !strcmp(file, "system/controlDict") && !strcmp(key, "deltaT")) {
        FDFile *bm = load_rel(casePath, "system/blockMeshDict");
        BlockMeshInfo bi; if (bm) blockmesh_info(bm, &bi); else bi.ok = false;
        if (bm && bi.ok && bi.dxMin > 0) {
            double dx = bi.oneCellDir >= 0 ? bi.dxMinThin : bi.dxMin;
            human_num(0.5 * dx / 1.0, dtv, sizeof dtv);          /* Co 0.5 at 1 m/s; the Run page's check refines it */
            def = dtv;
        }
        fd_free(bm);
    }
    pl_fmt_ui(x->text, sizeof x->text, "%s needs %s in %s, and there is none there.%s%s%s", app[0] ? app : "The solver", key, where,
             def ? " A usual value is " : "", def ? def : "", def ? (dtv[0] ? " s (Courant 0.5 at 1 m/s on the smallest cell; adjust for your speeds)." : "." ) : "");
    pl_strlcpy(x->file, file, sizeof x->file); x->line = line; pl_strlcpy(x->key, key, sizeof x->key);
    if (def) { x->fix = FIX_PUT; pl_strlcpy(x->fixFile, file, sizeof x->fixFile); put1(x, path, def); pl_fmt_ui(x->fixLabel, sizeof x->fixLabel, "Add %s %s", key, def); }
    fd_free(cd);
    return true;
}

/* "The dimensions [0 2 -2 0 0 0 0] provided do not match the expected dimensions [0 2 -1 0 0 0 0]" */
static bool bad_dims(const char *casePath, const char *log, const char *msg, Explanation *x)
{
    const char *a = strchr(msg, '['), *ae = a ? strchr(a, ']') : NULL;
    const char *b = ae ? strchr(ae, '[') : NULL, *be = b ? strchr(b, ']') : NULL;
    if (!a || !ae || !b || !be) return false;
    char got[64], want[64];
    pl_fmt_ui(got, sizeof got, "%.*s", (int)(ae - a + 1), a);
    pl_fmt_ui(want, sizeof want, "%.*s", (int)(be - b + 1), b);
    char ug[48] = "", uw[48] = "";
    dims_unit(got, ug, sizeof ug); dims_unit(want, uw, sizeof uw);
    char dp[300] = "", file[160], inside[200]; int line = 0;
    log_file_line(log, dp, sizeof dp, &line);
    split_dict_path(casePath, dp, file, sizeof file, inside, sizeof inside);
    pl_fmt_ui(x->title, sizeof x->title, "%s%s has the wrong units", inside[0] ? inside : "A value", file[0] ? "" : "");
    pl_fmt_ui(x->text, sizeof x->text, "%s%s%s is given in %s %s, but OpenFOAM expects %s %s here.", inside[0] ? inside : "The value",
             file[0] ? " in " : "", file, ug, got, uw, want);
    pl_strlcpy(x->file, file, sizeof x->file); x->line = line; pl_strlcpy(x->key, inside, sizeof x->key);
    FDFile *f = file[0] && inside[0] ? load_rel(casePath, file) : NULL;
    char v[200];
    if (f && fd_get(f, inside, v, sizeof v)) {
        char *p = strstr(v, got);
        if (!p) {                                             /* written with other spacing: replace the first [...] */
            char *o = strchr(v, '['), *c = o ? strchr(o, ']') : NULL;
            if (o && c) { char nv[200]; pl_fmt_ui(nv, sizeof nv, "%.*s%s%s", (int)(o - v), v, want, c + 1); pl_strlcpy(v, nv, sizeof v); p = NULL; }
            else { fd_free(f); return true; }
        } else { char nv[200]; pl_fmt_ui(nv, sizeof nv, "%.*s%s%s", (int)(p - v), v, want, p + strlen(got)); pl_strlcpy(v, nv, sizeof v); }
        x->fix = FIX_PUT; pl_strlcpy(x->fixFile, file, sizeof x->fixFile); put1(x, inside, v);
        pl_fmt_ui(x->fixLabel, sizeof x->fixLabel, "Use %s %s (keeps the number)", uw, want);
    }
    fd_free(f);
    return true;
}

/* "Incompatible dimensions for operation [U[0 2 -2 0 0 0 0] ] + [U[0 3 -2 0 0 0 0] ]": the terms of an equation disagree,
   most often because a field file has the wrong dimensions (H-10). Checked against what the field must be. */
static bool incompatible_dims(const char *casePath, const char *msg, Explanation *x)
{
    const char *a = strchr(msg, '[');
    char name[64] = "";
    if (!a || sscanf(a + 1, "%63[^[ ]", name) != 1) return false;
    const char *want = field_expected_dims(name);
    char rel[160] = "", dims[96] = ""; int line = 0;
    static const char *const dirs[] = { "0", "0.orig", NULL };
    for (int i = 0; dirs[i] && !rel[0]; i++) {
        char r[160]; snprintf(r, sizeof r, "%s/%s", dirs[i], name);
        FDFile *f = load_rel(casePath, r);
        FDEntry *e = f ? fd_find(f, "dimensions") : NULL;
        if (e && fd_entry_value(f, e, dims, sizeof dims)) { pl_strlcpy(rel, r, sizeof rel); line = e->line; }
        if (f) fd_free(f);
    }
    pl_fmt_ui(x->title, sizeof x->title, "The terms of the %s equation have different units", name);
    if (want && rel[0] && !dims_equal(dims, want)) {
        char ug[48] = "", uw[48] = "";
        dims_unit(dims, ug, sizeof ug); dims_unit(want, uw, sizeof uw);
        pl_fmt_ui(x->title, sizeof x->title, "%s has the wrong dimensions", rel);
        pl_fmt_ui(x->text, sizeof x->text, "%s says %s is in %s %s, but %s is always %s %s. Every term built from it then comes out in "
                  "the wrong units, and adding two of them stops the solver.", rel, name, ug, dims, name, uw, want);
        pl_strlcpy(x->file, rel, sizeof x->file); x->line = line; pl_strlcpy(x->key, "dimensions", sizeof x->key);
        x->fix = FIX_PUT; pl_strlcpy(x->fixFile, rel, sizeof x->fixFile); put1(x, "dimensions", want);
        pl_fmt_ui(x->fixLabel, sizeof x->fixLabel, "Set the dimensions to %s %s", uw, want);
        return true;
    }
    pl_fmt_ui(x->text, sizeof x->text, "Two terms of the %s equation came out in different units (%.*s). Check the dimensions in the %s field "
              "file and of the constants it meets (nu in constant/transportProperties, the thermophysical properties).", name,
              (int)(strlen(msg) > 200 ? 200 : strlen(msg)), msg, name);
    if (rel[0]) { pl_strlcpy(x->file, rel, sizeof x->file); x->line = line; }
    return true;
}

/* names that came in v2606, for "Unknown ... type X" under an older OpenFOAM */
static const char *const NEW_IN_2606[] = { "GEKO", NULL };

/* "Unknown RAS model type kEpsilonn", "Unknown radiationModel type ..." and the like */
static bool unknown_type(const char *casePath, const char *log, const char *msg, Explanation *x)
{
    const char *t = strstr(msg, " type ");
    if (!t) return false;
    char what[96]; pl_fmt_ui(what, sizeof what, "%.*s", (int)(t - msg - 8 > 0 && t - msg - 8 < 95 ? t - msg - 8 : 0), msg + 8);
    char name[96] = ""; sscanf(t + 6, "%95s", name);
    char marker[128]; pl_fmt_ui(marker, sizeof marker, "Valid %s types", what);
    char **valid = NULL; int nv = log_valid_list(log, marker, &valid);
    const char *near = closest_word(name, (const char *const *)valid, nv);
    char ver[32] = "";
    const char *vo = log ? strstr(log, "(openfoam-") : NULL;
    if (vo) sscanf(vo + 10, "%31[0-9]", ver);
    bool newer = false;
    for (int i = 0; NEW_IN_2606[i]; i++) if (!strcmp(name, NEW_IN_2606[i])) newer = true;
    /* where it is set: a constant/ or system/ dictionary with that word as a value */
    static const char *const files[] = { "constant/momentumTransport", "constant/turbulenceProperties", "constant/radiationProperties",
        "constant/thermophysicalProperties", "constant/transportProperties", "system/fvSolution", "system/fvSchemes", NULL };
    char file[160] = "", path[200] = ""; int line = 0;
    for (int i = 0; files[i] && !file[0]; i++) {
        FDFile *f = load_rel(casePath, files[i]);
        if (!f) continue;
        const char *cands[] = { "RAS/model", "RAS/RASModel", "LES/model", "LES/LESModel", "radiationModel", "transportModel", "simulationType", NULL };
        for (int k = 0; cands[k]; k++) {
            char v[96]; FDEntry *e = fd_find(f, cands[k]);
            if (e && fd_entry_value(f, e, v, sizeof v) && !strcmp(v, name)) { pl_strlcpy(file, files[i], sizeof file); pl_strlcpy(path, cands[k], sizeof path); line = e->line; break; }
        }
        fd_free(f);
    }
    /* else the dictionary the log names: "file: system/fvSolution/solvers/p at line 21 to 24." -> the entry in it set to name */
    const char *fl = !file[0] && log ? strstr(log, "\nfile: ") : NULL;
    if (fl) {
        char dp[PL_PATH] = ""; sscanf(fl + 7, "%4095s", dp);
        char f2[160], inside[200];
        split_dict_path(casePath, dp, f2, sizeof f2, inside, sizeof inside);
        FDFile *f = f2[0] ? load_rel(casePath, f2) : NULL;
        FDEntry *d = f ? (inside[0] ? fd_find(f, inside) : &f->root) : NULL;
        for (int i = 0; d && d->kind == FD_DICT && i < d->nChild && !file[0]; i++) {
            char v[96];
            if (d->child[i].kind == FD_VALUE && fd_entry_value(f, &d->child[i], v, sizeof v) && !strcmp(v, name)) {
                pl_strlcpy(file, f2, sizeof file);
                pl_fmt_ui(path, sizeof path, "%s%s%s", inside, inside[0] ? "/" : "", d->child[i].key);
                line = d->child[i].line;
            }
        }
        if (f) fd_free(f);
    }
    pl_fmt_ui(x->title, sizeof x->title, "This OpenFOAM has no %s called %s", what, name);
    if (newer && ver[0] && atoi(ver) < 2606)
        pl_fmt_ui(x->text, sizeof x->text, "%s came in OpenFOAM v2606, and this run used v%s (aerofly has v2412 too). Use a model v%s has%s%s%s, "
                 "or run with v2606.", name, ver, ver, near ? ", such as " : "", near ? near : "", "");
    else
        pl_fmt_ui(x->text, sizeof x->text, "%s %s isn't one of the %d this OpenFOAM%s%s has.%s%s%s", what, name, nv, ver[0] ? " v" : "", ver,
                 near ? " Did you mean " : "", near ? near : "", near ? "?" : "");
    x->text[0] = (char)toupper((unsigned char)x->text[0]);
    pl_strlcpy(x->file, file, sizeof x->file); x->line = line; pl_strlcpy(x->key, path, sizeof x->key);
    if (near && file[0] && !(newer && ver[0] && atoi(ver) < 2606 && edit_distance(name, near) > 2)) {
        x->fix = FIX_PUT; pl_strlcpy(x->fixFile, file, sizeof x->fixFile); put1(x, path, near);
        pl_fmt_ui(x->fixLabel, sizeof x->fixLabel, "Use %s", near);
    }
    return true;
}

/* "Wrong number of domain divisions in geomDecomp: Number of domains : 3 Wanted decomposition : (2 2 1)" */
static bool bad_decomp(const char *casePath, const char *log, const char *msg, Explanation *x)
{
    long nd = 0, a = 0, b = 0, c = 0;
    const char *p = strstr(msg, "Number of domains");
    if (p) { p = strchr(p, ':'); if (p) nd = atol(p + 1); }
    const char *w = strstr(msg, "decomposition");
    w = w ? strchr(w, '(') : NULL;
    if (w) sscanf(w, "(%ld %ld %ld)", &a, &b, &c);
    char dp[300] = "", file[160], inside[200]; int line = 0;
    log_file_line(log, dp, sizeof dp, &line);
    split_dict_path(casePath, dp, file, sizeof file, inside, sizeof inside);
    if (!file[0]) pl_strlcpy(file, "system/decomposeParDict", sizeof file);
    pl_fmt_ui(x->title, sizeof x->title, "The split doesn't make %ld pieces", nd);
    pl_fmt_ui(x->text, sizeof x->text, "numberOfSubdomains is %ld, but n (%ld %ld %ld) splits the mesh into %ld × %ld × %ld = %ld pieces. "
             "The two have to agree.", nd, a, b, c, a, b, c, a * b * c);
    pl_strlcpy(x->file, file, sizeof x->file); x->line = line; pl_strlcpy(x->key, "n", sizeof x->key);
    if (nd > 0) {
        long s[3]; decomp_split_n(nd, c == 1 ? 2 : 3, s);
        char v[96]; pl_fmt_ui(v, sizeof v, "(%ld %ld %ld)", s[0], s[1], s[2]);
        char path[260]; pl_fmt_ui(path, sizeof path, "%s%sn", inside, inside[0] ? "/" : "");
        x->fix = FIX_PUT; pl_strlcpy(x->fixFile, file, sizeof x->fixFile); put1(x, path, v);
        pl_fmt_ui(x->fixLabel, sizeof x->fixLabel, "Set n to %s", v);
    }
    return true;
}

/* "Unable to set reference cell for field p  Please supply either pRefCell or pRefPoint" */
static bool no_ref(const char *casePath, const char *log, const char *msg, Explanation *x)
{
    char field[32] = "p";
    const char *f = strstr(msg, "for field ");
    if (f) sscanf(f + 10, "%31s", field);
    char dp[300] = "", file[160], inside[200]; int line = 0;
    log_file_line(log, dp, sizeof dp, &line);
    split_dict_path(casePath, dp, file, sizeof file, inside, sizeof inside);
    if (!file[0]) pl_strlcpy(file, "system/fvSolution", sizeof file);
    pl_fmt_ui(x->title, sizeof x->title, "Nothing fixes the level of %s", field);
    pl_fmt_ui(x->text, sizeof x->text, "No patch gives %s a fixed value (no fixedValue outlet), so the solver only knows it up to a "
             "constant and asks for one cell to hold it: pRefCell and pRefValue in %s%s%s. If the case should have an outlet at a fixed "
             "pressure, set that patch to fixedValue instead.", field, file, inside[0] ? " under " : "", inside);
    pl_strlcpy(x->file, file, sizeof x->file); x->line = line; pl_strlcpy(x->key, "pRefCell", sizeof x->key);
    if (inside[0]) {
        char p1[260], p2[260];
        pl_fmt_ui(p1, sizeof p1, "%s/%sRefCell", inside, field); pl_fmt_ui(p2, sizeof p2, "%s/%sRefValue", inside, field);
        x->fix = FIX_PUT; pl_strlcpy(x->fixFile, file, sizeof x->fixFile); put1(x, p1, "0"); put1(x, p2, "0");
        pl_fmt_ui(x->fixLabel, sizeof x->fixLabel, "Hold %s = 0 in cell 0", field);
    }
    return true;
}

/* 'Cannot find file "points" in directory "bottomWater/polyMesh"': a multi-region case run before its regions were split */
static bool region_mesh(const char *casePath, const char *msg, Explanation *x)
{
    const char *d = strstr(msg, "in directory \"");
    if (!d) return false;
    char dir[160]; sscanf(d + 14, "%159[^\"]", dir);
    char *pm = strstr(dir, "/polyMesh");
    if (!pm) return false;
    *pm = 0;
    if (!dir[0] || strchr(dir, '/')) return false;            /* constant/polyMesh itself: no mesh at all */
    char rp[PL_PATH]; pl_join(rp, sizeof rp, casePath, "constant/regionProperties");
    if (!pl_is_file(rp)) return false;
    pl_fmt_ui(x->title, sizeof x->title, "The mesh of region %s hasn't been made", dir);
    pl_fmt_ui(x->text, sizeof x->text, "This is a multi-region case: the solver reads one mesh per region from constant/<region>/polyMesh, "
             "and constant/%s/polyMesh doesn't exist. The regions are cut out of the whole mesh by topoSet and splitMeshRegions "
             "-cellZones, which the mesh steps run.", dir);
    pl_strlcpy(x->file, "constant/regionProperties", sizeof x->file);
    x->fix = FIX_MESH;
    pl_fmt_ui(x->fixLabel, sizeof x->fixLabel, "Make the mesh and the region meshes");
    return true;
}

/* "Continuity error cannot be removed by adjusting the outflow. ... Specified mass inflow : 0.000254 ... outflow : 0" */
static bool no_outflow(const char *casePath, const char *log, Explanation *x)
{
    double in = -1, outv = -1;
    const char *p = log ? strstr(log, "Specified mass inflow") : NULL;
    if (p && (p = strchr(p, ':'))) in = atof(p + 1);
    p = log ? strstr(log, "Specified mass outflow") : NULL;
    if (p && (p = strchr(p, ':'))) outv = atof(p + 1);
    pl_fmt_ui(x->title, sizeof x->title, "The flow has nowhere to leave");
    char nums[160] = "";
    if (in >= 0) pl_fmt_ui(nums, sizeof nums, " %g m³/s comes in through fixed inlets and %g goes out.", in, outv < 0 ? 0 : outv);
    /* a patch called outlet* whose p isn't fixed: the usual cause */
    FDFile *f = load_rel(casePath, "0/p");
    if (!f) f = load_rel(casePath, "0.orig/p");
    FDEntry *bf = f ? fd_find(f, "boundaryField") : NULL;
    char outlet[128] = ""; char ptype[64] = "";
    for (int i = 0; bf && i < bf->nChild; i++) {
        if (bf->child[i].kind != FD_DICT || !strcasestr(bf->child[i].key, "outlet")) continue;
        FDEntry *t = fd_child(&bf->child[i], "type");
        if (t && fd_entry_value(f, t, ptype, sizeof ptype) && strcmp(ptype, "fixedValue") && strcmp(ptype, "totalPressure")) {
            pl_strlcpy(outlet, bf->child[i].key, sizeof outlet); x->line = t->line; break;
        }
    }
    pl_fmt_ui(x->text, sizeof x->text, "No boundary lets the solver adjust the outflow to balance the inflow, so mass can't be conserved.%s%s%s%s%s%s "
              "An outlet normally has p fixedValue (and U zeroGradient or inletOutlet).", nums, outlet[0] ? " Patch " : "", outlet,
              outlet[0] ? " has p " : "", outlet[0] ? ptype : "", outlet[0] ? "." : "");
    if (outlet[0]) {
        pl_strlcpy(x->file, pl_ends_with(f->path, "0.orig/p") ? "0.orig/p" : "0/p", sizeof x->file);
        pl_strlcpy(x->key, outlet, sizeof x->key);
        char a[260], b[260]; pl_fmt_ui(a, sizeof a, "boundaryField/%s/type", outlet); pl_fmt_ui(b, sizeof b, "boundaryField/%s/value", outlet);
        x->fix = FIX_PUT; pl_strlcpy(x->fixFile, x->file, sizeof x->fixFile); put1(x, a, "fixedValue"); put1(x, b, "uniform 0");
        pl_fmt_ui(x->fixLabel, sizeof x->fixLabel, "Fix p = 0 on %s", outlet);
    }
    fd_free(f);
    return true;
}

bool foam_explain(const char *casePath, const char *logName, const char *fatalMsg, Explanation *out)
{
    memset(out, 0, sizeof *out);
    if (!fatalMsg || !*fatalMsg) return false;
    if (strcasestr(fatalMsg, "No times selected")) return no_times(casePath, logName ? logName : "", out);
    if (strstr(fatalMsg, "error in IOstream") && strstr(fatalMsg, "Ostream&")) return write_failed(casePath, fatalMsg, out);
    char *log = read_log(casePath, logName);
    bool ok = false;
    if (strstr(fatalMsg, "Cannot find patchField entry for ")) ok = missing_patch(casePath, log, fatalMsg, out);
    else if (strstr(fatalMsg, "Unknown patchField type ")) ok = bad_bc_type(casePath, log, fatalMsg, out);
    else if (pl_starts_with(fatalMsg, "Entry '") && strstr(fatalMsg, "' not found in dictionary")) ok = missing_entry(casePath, log, fatalMsg, out);
    else if (strstr(fatalMsg, "provided do not match the expected dimensions")) ok = bad_dims(casePath, log, fatalMsg, out);
    else if (strstr(fatalMsg, "Incompatible dimensions for operation")) ok = incompatible_dims(casePath, fatalMsg, out);
    else if (strstr(fatalMsg, "Wrong number of domain divisions")) ok = bad_decomp(casePath, log, fatalMsg, out);
    else if (strstr(fatalMsg, "Unable to set reference cell")) ok = no_ref(casePath, log, fatalMsg, out);
    else if (strstr(fatalMsg, "Cannot find file \"points\" in directory")) ok = region_mesh(casePath, fatalMsg, out);
    else if (strstr(fatalMsg, "Continuity error cannot be removed")) ok = no_outflow(casePath, log, out);
    else if (pl_starts_with(fatalMsg, "Unknown ") && strstr(fatalMsg, " type ")) ok = unknown_type(casePath, log, fatalMsg, out);
    free(log);
    return ok;
}

bool explain_apply(void *fdfile, const Explanation *x)
{
    FDFile *f = fdfile;
    if (!f || f->binary) return false;
    if (x->fix == FIX_PUT) {
        for (int i = 0; i < x->nPut; i++) if (!fd_put(f, x->putPath[i], x->putVal[i])) return false;
        return x->nPut > 0;
    }
    if (x->fix == FIX_RENAME) {
        FDEntry *e = fd_find(f, x->fixPath);
        return e && fd_replace_span(f, e->keyStart, e->keyEnd, x->fixValue);
    }
    return false;
}
