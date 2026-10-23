/* sweep.c -- SENSOR SWEEP (ROADMAP 3.1): the case read before a run, for the mistakes that would stop or spoil it.
 *
 * Reads the open case through its dictionary cache (so unsaved edits count): patch coverage and constraint types,
 * schemes and linear solvers for the solved fields, the pressure's units for the solver family, the decomposition,
 * multi-region files, the output volume against the free disk, the end time against the first write, processor*
 * folders left from another decomposition, and the Fourier number of solid regions. Each finding says where, in
 * numbers, and carries a safe fix when there is one (Explanation's FIX_* fields, applied by the app).
 */
#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include "sweep.h"
#include "fieldinfo.h"
#include "physics.h"
#include "precheck.h"

static Finding *add(Sweep *w, int level)
{
    if (w->n >= SWEEP_MAX) return NULL;
    Finding *f = &w->f[w->n++];
    memset(f, 0, sizeof *f);
    f->level = level;
    if (level > w->worst) w->worst = level;
    return f;
}

static bool constraint(const char *t)
{
    return !strcmp(t, "empty") || !strcmp(t, "symmetryPlane") || !strcmp(t, "symmetry") || !strcmp(t, "wedge") ||
           !strcmp(t, "cyclic") || !strcmp(t, "cyclicAMI") || !strcmp(t, "processor");
}

/* the boundaryField entry that applies to a patch: exact name, then "regex" keys (last wins, as OpenFOAM does) */
static bool in_words(const char *list, const char *w)
{
    size_t n = strlen(w);
    for (const char *p = list; (p = strstr(p, w)); p += n)
        if ((p == list || p[-1] == ' ') && (p[n] == 0 || p[n] == ' ')) return true;
    return false;
}

/* the boundaryField entry that applies to a patch, in OpenFOAM's order: exact name, "regex" keys (last wins), then
   the patch's groups (inGroups; a constraint type is a group of its own) */
static FDEntry *bc_for(FDEntry *bf, const PatchInfo *pa)
{
    FDEntry *hit = NULL;
    for (int i = 0; i < bf->nChild; i++) if (bf->child[i].key && !strcmp(bf->child[i].key, pa->name)) return &bf->child[i];
    for (int i = 0; i < bf->nChild; i++) if (bf->child[i].key && bf->child[i].key[0] == '"' && pl_foam_key_matches(bf->child[i].key, pa->name)) hit = &bf->child[i];
    if (hit) return hit;
    for (int i = 0; i < bf->nChild; i++) {
        const char *k = bf->child[i].key;
        if (k && (in_words(pa->groups, k) || (constraint(pa->type) && !strcmp(k, pa->type)))) return &bf->child[i];
    }
    return NULL;
}

static bool has_directive(FDEntry *d)
{
    for (int i = 0; i < d->nChild; i++) if (d->child[i].kind == FD_DIRECTIVE) return true;
    return false;
}

/* the case has an Allrun of its own: it may change the patches after blockMesh (createPatch, splitMeshRegions, extrudeMesh, ...) */
static bool scripts_reshape_patches(const char *casePath)
{
    static const char *const names[] = { "Allrun", "Allrun.pre", "Allmesh", NULL };
    static const char *const tools[] = { "createPatch", "splitMeshRegions", "extrudeMesh", "snappyHexMesh", "createBaffles", "mergeMeshes", "stitchMesh", NULL };
    (void)tools;
    for (int i = 0; names[i]; i++) {                       /* any script of its own: tutorials set up fields and patches in
                                                              ways a sweep can't follow (createPatch, foamDictionary, m4) */
        char p[PL_PATH]; if (!pl_fmt(p, sizeof p, "%s/%s", casePath, names[i])) continue;
        if (pl_exists(p)) return true;
    }
    return false;
}

static void coverage(Case *c, Sweep *w)
{
    if (c->s.nRegions > 0 && !c->region[0]) return;        /* per region, on the Boundaries page */
    if (c->patchSource == 0 || c->patchSource == 3) return; /* no mesh or blockMeshDict to say what the patches are */
    int missing = 0;
    bool renames = scripts_reshape_patches(c->s.path);
    for (int fi = 0; fi < c->fields.n; fi++) {
        char rel[300]; snprintf(rel, sizeof rel, "%s/%s", c->fieldDir, c->fields.names[fi]);
        FDFile *f = case_dict(c, rel);
        if (!f || !f->ok || f->binary) continue;
        char cls[64] = ""; fd_get(f, "FoamFile/class", cls, sizeof cls);
        if (!pl_starts_with(cls, "vol")) continue;
        FDEntry *bf = fd_find(f, "boundaryField");
        if (!bf || bf->kind != FD_DICT) continue;
        bool incl = has_directive(bf);                      /* #includeEtc "caseDicts/setConstraintTypes" and the like */
        for (int p = 0; p < c->nPatches; p++) {
            const PatchInfo *pa = &c->patches[p];
            if (pa->pattern || !strcmp(pa->type, "processor") || (c->patchSource != 1 && pa->collapsed)) continue;
            FDEntry *e = bc_for(bf, pa);
            if (!e) {
                if (incl) continue;                         /* may come from the include: not ours to say */
                if (++missing > 6) continue;
                /* before meshing the patches come from blockMeshDict, and blockMesh drops some (collapsed axis faces) */
                Finding *g = add(w, c->patchSource == 1 ? PC_BAD : PC_WARN); if (!g) return;
                g->ask = c->patchSource == 1 || !renames;      /* an Allrun of its own may well make it (createPatch, ...) */
                Explanation *x = &g->x;
                pl_fmt_ui(x->title, sizeof x->title, "%s has no condition for %s", rel, pa->name);
                if (c->patchSource == 1)
                    pl_fmt_ui(x->text, sizeof x->text, "Patch %s (%s) has no entry in %s, so the solver stops when it reads the field.",
                              pa->name, pa->type[0] ? pa->type : "patch", rel);
                else pl_fmt_ui(x->text, sizeof x->text, "system/blockMeshDict makes a patch %s (%s), and %s has no entry for it. If the mesh keeps the "
                               "patch (blockMesh drops collapsed ones; createPatch or a script may rename it), the solver stops on it.",
                               pa->name, pa->type[0] ? pa->type : "patch", rel);
                pl_strlcpy(x->file, rel, sizeof x->file); x->line = bf->line;
                x->fix = FIX_ADD_BC; pl_strlcpy(x->fixFile, rel, sizeof x->fixFile); pl_strlcpy(x->fixValue, pa->name, sizeof x->fixValue);
                pl_fmt_ui(x->fixLabel, sizeof x->fixLabel, "Add a condition for %s", pa->name);
                continue;
            }
            if (constraint(pa->type) && e->kind == FD_DICT && c->patchSource == 1) {
                FDEntry *t = fd_child(e, "type");
                char tv[64] = ""; if (t) fd_entry_value(f, t, tv, sizeof tv);
                if (tv[0] && strcmp(tv, pa->type)) {
                    Finding *g = add(w, PC_BAD); if (!g) return;
                    Explanation *x = &g->x;
                    pl_fmt_ui(x->title, sizeof x->title, "%s on %s must be %s", rel, pa->name, pa->type);
                    pl_fmt_ui(x->text, sizeof x->text, "Patch %s is a %s patch in the mesh, so every field has to use type %s there; %s has %s.",
                              pa->name, pa->type, pa->type, rel, tv);
                    pl_strlcpy(x->file, rel, sizeof x->file); x->line = t->line;
                    x->fix = FIX_PUT; pl_strlcpy(x->fixFile, rel, sizeof x->fixFile);
                    pl_fmt_ui(x->putPath[0], sizeof x->putPath[0], "boundaryField/%s/type", e->key); pl_strlcpy(x->putVal[0], pa->type, sizeof x->putVal[0]);
                    x->nPut = 1;
                    pl_fmt_ui(x->fixLabel, sizeof x->fixLabel, "Set it to %s", pa->type);
                }
            }
        }
    }
    if (missing > 6) {
        Finding *g = add(w, c->patchSource == 1 ? PC_BAD : PC_WARN);
        if (g) pl_fmt_ui(g->x.title, sizeof g->x.title, "... and %d more patches without a condition (Boundaries page)", missing - 6);
    }
    w->checks++;
}

static bool steady_app(const char *app) { return strstr(app, "simple") || strstr(app, "Simple") || strstr(app, "potential") || strstr(app, "steady"); }

static bool has_field(const Case *c, const char *n) { for (int i = 0; i < c->fields.n; i++) if (!strcmp(c->fields.names[i], n)) return true; return false; }

/* the standard incompressible solvers, whose U and p solves are known: the others (rhoCentralFoam, interFoam with
   momentumPredictor off, solid and DNS solvers) solve differently */
static bool std_solver(Case *c)
{
    static const char *const S[] = { "icoFoam", "simpleFoam", "pimpleFoam", "pisoFoam", "porousSimpleFoam", "SRFSimpleFoam", "SRFPimpleFoam", NULL };
    for (int i = 0; S[i]; i++) if (!strcmp(c->s.app, S[i])) {
        FDFile *sol = case_dict(c, "system/fvSolution");
        char mp[16] = "";
        if (sol && (fd_get(sol, "PIMPLE/momentumPredictor", mp, sizeof mp) || fd_get(sol, "PISO/momentumPredictor", mp, sizeof mp)) &&
            (!strcmp(mp, "no") || !strcmp(mp, "off") || !strcmp(mp, "false"))) return false;
        return true;
    }
    return false;
}

/* the transported fields this case solves and whose schemes and solvers must be there */
static int solved_fields(Case *c, const char **out)
{
    int n = 0;
    char st[32] = "", model[64] = "";
    FDFile *t = case_dict(c, case_rpath(c, "constant/turbulenceProperties"));
    if (!t) t = case_dict(c, case_rpath(c, "constant/momentumTransport"));
    if (t) {
        fd_get(t, "simulationType", st, sizeof st);
        if (!fd_get(t, !strcmp(st, "LES") ? "LES/model" : "RAS/model", model, sizeof model)) fd_get(t, !strcmp(st, "LES") ? "LES/LESModel" : "RAS/RASModel", model, sizeof model);
    }
    static const char *const CAND[] = { "U", "k", "epsilon", "omega", "nuTilda", NULL };
    for (int i = 0; CAND[i]; i++) {
        if (!has_field(c, CAND[i])) continue;
        if (i == 0 && !std_solver(c)) continue;               /* other solvers name, split or skip the momentum solve */
        if (i > 0 && turb_field_used(CAND[i], st, model) != 1) continue;
        out[n++] = CAND[i];
    }
    return n;
}

static bool solver_for(FDFile *sol, const char *field)
{
    FDEntry *s = fd_find(sol, "solvers");
    if (!s) return false;
    for (int i = 0; i < s->nChild; i++) if (s->child[i].key && pl_foam_key_matches(s->child[i].key, field)) return true;
    return false;
}

static void schemes_solvers(Case *c, Sweep *w)
{
    if (c->s.nRegions > 0) return;                          /* per-region dictionaries: checked by region consistency */
    FDFile *sch = case_dict(c, "system/fvSchemes"), *sol = case_dict(c, "system/fvSolution");
    if (!sch || !sol || !sch->ok || !sol->ok) return;
    const char *f[8]; int n = solved_fields(c, f);
    bool steady = steady_app(c->s.app);
    FDEntry *div = fd_find(sch, "divSchemes");
    bool divDefault = false;
    if (div) { char v[80] = ""; divDefault = fd_get(sch, "divSchemes/default", v, sizeof v) && strcmp(v, "none"); }
    for (int i = 0; i < n; i++) {
        if (div && !divDefault) {
            bool found = false; char want[64], k1[64], k2[64];
            snprintf(want, sizeof want, ",%s)", f[i]); snprintf(k1, sizeof k1, "div(phi,%s)", f[i]); snprintf(k2, sizeof k2, "div(rhoPhi,%s)", f[i]);
            for (int k = 0; k < div->nChild; k++) {
                const char *dk = div->child[k].key;
                if (dk && (strstr(dk, want) || (dk[0] == '"' && (pl_foam_key_matches(dk, k1) || pl_foam_key_matches(dk, k2))))) found = true;
            }
            if (!found) {
                Finding *g = add(w, PC_BAD); if (!g) return;
                Explanation *x = &g->x;
                char key[64]; snprintf(key, sizeof key, "div(phi,%s)", f[i]);
                const char *d = scheme_default("divSchemes", key, steady);
                pl_fmt_ui(x->title, sizeof x->title, "No %s in system/fvSchemes", key);
                pl_fmt_ui(x->text, sizeof x->text, "%s solves %s, and divSchemes has neither %s nor a default: it stops at its first step.", c->s.app, f[i], key);
                pl_strlcpy(x->file, "system/fvSchemes", sizeof x->file); x->line = div->line;
                if (d) { x->fix = FIX_PUT; pl_strlcpy(x->fixFile, "system/fvSchemes", sizeof x->fixFile);
                         pl_fmt_ui(x->putPath[0], sizeof x->putPath[0], "divSchemes/%s", key); pl_strlcpy(x->putVal[0], d, sizeof x->putVal[0]); x->nPut = 1;
                         pl_fmt_ui(x->fixLabel, sizeof x->fixLabel, "Add %s %s", key, d); }
            }
        }
        char fin[48]; snprintf(fin, sizeof fin, "%sFinal", f[i]);
        if (!solver_for(sol, f[i]) && !solver_for(sol, fin)) {      /* PIMPLE with one outer corrector solves only the Final */
            Finding *g = add(w, PC_BAD); if (!g) return;
            Explanation *x = &g->x;
            pl_fmt_ui(x->title, sizeof x->title, "No linear solver for %s", f[i]);
            pl_fmt_ui(x->text, sizeof x->text, "system/fvSolution has no entry under solvers that matches %s (a name, or a \"(U|k)\" pattern).", f[i]);
            pl_strlcpy(x->file, "system/fvSolution", sizeof x->file);
            FDEntry *s = fd_find(sol, "solvers"); x->line = s ? s->line : 0;
        }
    }
    /* the pressure, and its Final for PISO/PIMPLE */
    const char *pn = !std_solver(c) ? NULL : has_field(c, "p_rgh") ? "p_rgh" : has_field(c, "p") ? "p" : NULL;
    if (pn && !solver_for(sol, pn)) {
        Finding *g = add(w, PC_BAD);
        if (g) { pl_fmt_ui(g->x.title, sizeof g->x.title, "No linear solver for %s", pn);
                 pl_fmt_ui(g->x.text, sizeof g->x.text, "system/fvSolution has no entry under solvers that matches %s.", pn);
                 pl_strlcpy(g->x.file, "system/fvSolution", sizeof g->x.file); }
    }
    bool transient = !steady && (fd_find(sol, "PISO") || fd_find(sol, "PIMPLE"));
    if (pn && transient) {
        char fn[32]; snprintf(fn, sizeof fn, "%sFinal", pn);
        if (solver_for(sol, pn) && !solver_for(sol, fn)) {
            Finding *g = add(w, PC_BAD);
            if (g) { pl_fmt_ui(g->x.title, sizeof g->x.title, "No linear solver for %s", fn);
                     pl_fmt_ui(g->x.text, sizeof g->x.text, "PISO and PIMPLE solve %s on their last corrector with the solver called %s. Add one under "
                               "solvers in system/fvSolution: %s { $%s; relTol 0; }", pn, fn, fn, pn);
                     pl_strlcpy(g->x.file, "system/fvSolution", sizeof g->x.file); }
        }
    }
    w->checks++;
}

/* incompressible solvers work in kinematic pressure (m²/s²), the rest in Pa */
static void pressure_units(Case *c, Sweep *w)
{
    const char *app = c->s.app;
    if (strstr(app, "Boussinesq")) return;                 /* kinematic pressure, buoyant: either way it works */
    static const char *const KIN[] = { "icoFoam", "simpleFoam", "pimpleFoam", "pisoFoam", "SRFSimpleFoam", "SRFPimpleFoam", "porousSimpleFoam",
                                       "nonNewtonianIcoFoam", "adjointShapeOptimizationFoam", "boundaryFoam", "shallowWaterFoam", NULL };
    int kin = -1;
    for (int i = 0; KIN[i]; i++) if (!strcmp(app, KIN[i])) kin = 1;
    if (kin < 0 && (strstr(app, "rho") || strstr(app, "buoyant") || strstr(app, "cht") || strstr(app, "sonic") || strstr(app, "Sonic") ||
                    strstr(app, "interFoam") || strstr(app, "compressible") || strstr(app, "reacting"))) kin = 0;
    if (kin < 0 || !has_field(c, "p")) return;
    char rel[300]; snprintf(rel, sizeof rel, "%s/p", c->fieldDir);
    FDFile *f = case_dict(c, rel);
    char d[64] = "";
    if (!f || !fd_get(f, "dimensions", d, sizeof d)) return;
    const char *want = kin ? "[0 2 -2 0 0 0 0]" : "[1 -1 -2 0 0 0 0]";
    char ud[48] = "", uw[48] = "";
    if (!dims_unit(d, ud, sizeof ud)) return;
    dims_unit(want, uw, sizeof uw);
    if (!strcmp(ud, uw)) { w->checks++; return; }
    Finding *g = add(w, PC_BAD); if (!g) return;
    Explanation *x = &g->x;
    pl_fmt_ui(x->title, sizeof x->title, "p is in %s; %s works in %s", ud, app, uw);
    pl_fmt_ui(x->text, sizeof x->text, kin ? "%s is incompressible: it solves kinematic pressure p/ρ in m²/s², and %s gives p %s. Change the "
                                             "dimensions (and divide the values by the density)." :
                                             "%s solves the pressure itself, in Pa, and %s gives p %s. Change the dimensions (and the values: "
                                             "about 1e5 Pa at sea level).", app, rel, d);
    pl_strlcpy(x->file, rel, sizeof x->file);
    FDEntry *e = fd_find(f, "dimensions"); x->line = e ? e->line : 0;
    x->fix = FIX_PUT; pl_strlcpy(x->fixFile, rel, sizeof x->fixFile); pl_strlcpy(x->putPath[0], "dimensions", sizeof x->putPath[0]);
    pl_strlcpy(x->putVal[0], want, sizeof x->putVal[0]); x->nPut = 1;
    pl_fmt_ui(x->fixLabel, sizeof x->fixLabel, "Set the dimensions to %s (check the values)", uw);
    w->checks++;
}

static void decomposition(Case *c, Sweep *w)
{
    FDFile *d = case_dict(c, "system/decomposeParDict");
    if (!d || !d->ok) return;
    char v[64] = "", m[64] = "";
    fd_get(d, "numberOfSubdomains", v, sizeof v); fd_get(d, "method", m, sizeof m);
    long nd = atol(v);
    if (nd < 1 || (strcmp(m, "simple") && strcmp(m, "hierarchical"))) { w->checks++; return; }

    char path[64] = ""; char n[96] = "";
    char pref[64]; pl_fmt_ui(pref, sizeof pref, "%sCoeffs/n", m);
    if (fd_get(d, "coeffs/n", n, sizeof n)) pl_strlcpy(path, "coeffs/n", sizeof path);
    else if (fd_get(d, pref, n, sizeof n)) pl_strlcpy(path, pref, sizeof path);

    long a = 0, b = 0, cc = 0;
    if (!path[0] || sscanf(n, "(%ld %ld %ld)", &a, &b, &cc) != 3) { w->checks++; return; }
    if (a * b * cc == nd) { w->checks++; return; }
    Finding *g = add(w, PC_WARN); if (!g) return;
    Explanation *x = &g->x;
    pl_fmt_ui(x->title, sizeof x->title, "decomposeParDict: n %s makes %ld pieces, not %ld", n, a * b * cc, nd);
    pl_fmt_ui(x->text, sizeof x->text, "numberOfSubdomains is %ld but n %s splits the mesh into %ld × %ld × %ld = %ld. decomposePar stops on "
              "this (the app's own parallel runs set both from the Processes count; an Allrun uses the file as it is).", nd, n, a, b, cc, a * b * cc);
    pl_strlcpy(x->file, "system/decomposeParDict", sizeof x->file);
    FDEntry *e = fd_find(d, path); x->line = e ? e->line : 0;
    long s[3]; decomp_split_n(nd, cc == 1 ? 2 : 3, s);
    x->fix = FIX_PUT; pl_strlcpy(x->fixFile, "system/decomposeParDict", sizeof x->fixFile); pl_strlcpy(x->putPath[0], path, sizeof x->putPath[0]);
    pl_fmt_ui(x->putVal[0], sizeof x->putVal[0], "(%ld %ld %ld)", s[0], s[1], s[2]); x->nPut = 1;
    pl_fmt_ui(x->fixLabel, sizeof x->fixLabel, "Set n to %s", x->putVal[0]);
    w->checks++;
}

static void regions(Case *c, Sweep *w)
{
    if (c->s.nRegions == 0) return;
    for (int r = 0; r < c->s.nRegions; r++) {
        const char *rg = c->s.regions[r];
        const char *need[] = { "system/%s/fvSchemes", "system/%s/fvSolution", NULL };
        for (int k = 0; need[k]; k++) {
            char rel[300]; snprintf(rel, sizeof rel, need[k], rg);
            char p[PL_PATH]; pl_join(p, sizeof p, c->s.path, rel);
            if (pl_is_file(p)) continue;
            Finding *g = add(w, PC_BAD); if (!g) return;
            pl_fmt_ui(g->x.title, sizeof g->x.title, "Region %s has no %s", rg, rel);
            pl_fmt_ui(g->x.text, sizeof g->x.text, "constant/regionProperties lists %s, and every region needs its own %s.", rg, rel);
            pl_strlcpy(g->x.file, "constant/regionProperties", sizeof g->x.file);
        }
        char a[PL_PATH], b[PL_PATH], t[PL_PATH];
        snprintf(t, sizeof t, "0.orig/%s", rg); pl_join(a, sizeof a, c->s.path, t);
        snprintf(t, sizeof t, "0/%s", rg); pl_join(b, sizeof b, c->s.path, t);
        if (!pl_is_dir(a) && !pl_is_dir(b) && !c->s.hasZeroOrig) {
            Finding *g = add(w, PC_WARN); if (!g) return;
            pl_fmt_ui(g->x.title, sizeof g->x.title, "Region %s has no initial fields", rg);
            pl_fmt_ui(g->x.text, sizeof g->x.text, "Neither 0/%s nor 0.orig/%s exists: the solver needs the region's fields there.", rg, rg);
        }
        snprintf(t, sizeof t, "constant/%s/thermophysicalProperties", rg); pl_join(a, sizeof a, c->s.path, t);
        char ph[300]; snprintf(ph, sizeof ph, "constant/%s/phaseProperties", rg); pl_join(b, sizeof b, c->s.path, ph);
        if (!pl_is_file(a) && !pl_is_file(b)) {
            Finding *g = add(w, PC_BAD); if (!g) return;
            pl_fmt_ui(g->x.title, sizeof g->x.title, "Region %s has no thermophysicalProperties", rg);
            pl_fmt_ui(g->x.text, sizeof g->x.text, "%s is missing: the region's material comes from there.", t);
        }
    }
    w->checks++;
}

/* writes: how many, and how much disk they take */
static void output(Case *c, Sweep *w)
{
    FDFile *cd = case_dict(c, "system/controlDict");
    if (!cd) return;
    char v[64];
    double st = fd_get(cd, "startTime", v, sizeof v) ? atof(v) : 0, et = fd_get(cd, "endTime", v, sizeof v) ? atof(v) : -1;
    double dt = fd_get(cd, "deltaT", v, sizeof v) ? atof(v) : -1, wi = fd_get(cd, "writeInterval", v, sizeof v) ? atof(v) : -1;
    char wc[48] = "timeStep"; fd_get(cd, "writeControl", wc, sizeof wc);
    char fmt[16] = "ascii"; fd_get(cd, "writeFormat", fmt, sizeof fmt);
    int purge = fd_get(cd, "purgeWrite", v, sizeof v) ? atoi(v) : 0;
    if (et <= st || wi <= 0) return;
    double first = -1, span = et - st;
    long writes = -1;
    if (!strcmp(wc, "timeStep") && dt > 0) { first = st + wi * dt; writes = (long)floor(span / (wi * dt) + 1e-9); }
    else if (!strcmp(wc, "runTime") || !strcmp(wc, "adjustable") || !strcmp(wc, "adjustableRunTime")) {
        /* written at whole multiples of writeInterval in time, not counted from startTime (H-14: start 0.001, end 0.5,
           every 0.25 writes 0.25 and 0.5) */
        double k0 = floor(st / wi + 1e-9);
        first = (k0 + 1) * wi;
        writes = (long)(floor(et / wi + 1e-9) - k0);
    }
    if (first > 0 && first > et * (1 + 1e-9)) {
        Finding *g = add(w, PC_WARN); if (!g) return;
        g->ask = true;
        Explanation *x = &g->x;
        pl_fmt_ui(x->title, sizeof x->title, "Nothing would be written: the first write is after the end");
        if (!strcmp(wc, "timeStep"))
            pl_fmt_ui(x->text, sizeof x->text, "The run ends at t = %g s, and writeInterval %g steps × deltaT %g s puts the first write at t = %g s. "
                      "The run would leave no results.", et, wi, dt, first);
        else pl_fmt_ui(x->text, sizeof x->text, "The run ends at t = %g s, and with writeInterval %g s from t = %g the first write is at t = %g s.", et, wi, st, first);
        pl_strlcpy(x->file, "system/controlDict", sizeof x->file);
        FDEntry *e = fd_find(cd, "writeInterval"); x->line = e ? e->line : 0;
        x->fix = FIX_PUT; pl_strlcpy(x->fixFile, "system/controlDict", sizeof x->fixFile); pl_strlcpy(x->putPath[0], "writeInterval", sizeof x->putPath[0]);
        if (!strcmp(wc, "timeStep")) pl_fmt_ui(x->putVal[0], sizeof x->putVal[0], "%ld", (long)fmax(1, floor(span / dt + 1e-9)));
        else pl_fmt_ui(x->putVal[0], sizeof x->putVal[0], "%g", span);
        x->nPut = 1;
        pl_fmt_ui(x->fixLabel, sizeof x->fixLabel, "Write at the end (writeInterval %s)", x->putVal[0]);
        w->checks++;
        return;
    }
    long cells = c->s.nCells;
    if (cells <= 0) {
        FDFile *bm = case_dict(c, "system/blockMeshDict");
        BlockMeshInfo bi; if (bm) { blockmesh_info(bm, &bi); if (bi.ok) cells = bi.cells; }
    }
    if (cells <= 0 || writes <= 0) { w->checks++; return; }
    if (purge > 0 && writes > purge) writes = purge;
    double values = 0;                                       /* numbers per cell over the fields written */
    for (int i = 0; i < c->fields.n; i++) {
        char rel[300]; snprintf(rel, sizeof rel, "%s/%s", c->fieldDir, c->fields.names[i]);
        FDFile *f = case_dict(c, rel);
        char cls[64] = ""; if (f) fd_get(f, "FoamFile/class", cls, sizeof cls);
        values += strstr(cls, "Vector") ? 3 : strstr(cls, "SymmTensor") ? 6 : strstr(cls, "Tensor") ? 9 : 1;
    }
    values += 3 + 1;                                         /* phi on faces (about 3 per cell) and the odd extra field */
    double bytes = (double)writes * (double)cells * values * (!strcmp(fmt, "binary") ? 8.5 : 14);
    struct statvfs sv; double freeB = statvfs(c->s.path, &sv) == 0 ? (double)sv.f_bavail * (double)sv.f_frsize : -1;
    char hb[32], hf[32]; pl_human_size(hb, sizeof hb, (long long)bytes); pl_human_size(hf, sizeof hf, (long long)(freeB > 0 ? freeB : 0));
    int level = freeB > 0 && bytes > freeB ? PC_BAD : (freeB > 0 && bytes > 0.5 * freeB) || bytes > 50e9 ? PC_WARN : PC_OK;
    Finding *g = add(w, level); if (!g) return;
    Explanation *x = &g->x;
    pl_fmt_ui(x->title, sizeof x->title, "Output: about %s in %ld write%s%s", hb, writes, writes == 1 ? "" : "s", level == PC_BAD ? ", more than the disk has" : "");
    pl_fmt_ui(x->text, sizeof x->text, "%ld writes × %ld cells × %.0f values per cell (%d fields and the face fluxes), %s: about %s. The disk has %s free.%s",
              writes, cells, values, c->fields.n, fmt, hb, hf, level >= PC_WARN ? " Write less often (writeInterval), keep fewer (purgeWrite) or write binary." : "");
    pl_strlcpy(x->file, "system/controlDict", sizeof x->file);
    FDEntry *e = fd_find(cd, "writeInterval"); x->line = e ? e->line : 0;
    if (level >= PC_WARN && strcmp(fmt, "binary")) {
        x->fix = FIX_PUT; pl_strlcpy(x->fixFile, "system/controlDict", sizeof x->fixFile);
        pl_strlcpy(x->putPath[0], "writeFormat", sizeof x->putPath[0]); pl_strlcpy(x->putVal[0], "binary", sizeof x->putVal[0]); x->nPut = 1;
        pl_fmt_ui(x->fixLabel, sizeof x->fixLabel, "Write binary (about 40 %% smaller)");
    }
    w->checks++;
}

static void leftovers(Case *c, Sweep *w)
{
    if (c->s.nProcDirs <= 0) return;
    FDFile *d = case_dict(c, "system/decomposeParDict");
    char v[32] = ""; long nd = d && fd_get(d, "numberOfSubdomains", v, sizeof v) ? atol(v) : 0;
    char pm[PL_PATH], pp[PL_PATH];
    pl_join(pm, sizeof pm, c->s.path, "constant/polyMesh/owner");
    pl_join(pp, sizeof pp, c->s.path, "processor0/constant/polyMesh/owner");
    bool older = pl_is_file(pm) && pl_is_file(pp) && pl_mtime(pp) < pl_mtime(pm);
    if ((nd > 0 && nd != c->s.nProcDirs) || older) {
        Finding *g = add(w, PC_WARN); if (!g) return;
        pl_fmt_ui(g->x.title, sizeof g->x.title, "processor0..%d are left from an earlier decomposition", c->s.nProcDirs - 1);
        pl_fmt_ui(g->x.text, sizeof g->x.text, "%d processor folders%s%s. A parallel run decomposes again (decomposePar -force); an Allrun "
                  "that only reconstructs would read the old ones. Clean results moves them to the trash.", c->s.nProcDirs,
                  nd > 0 && nd != c->s.nProcDirs ? ", but decomposeParDict asks for a different count" : "",
                  older ? ", older than constant/polyMesh" : "");
    }
    w->checks++;
}

/* solid regions: the Fourier number per step, for the record (implicit solvers stay stable; it says how fast heat
   crosses a cell compared to a step) */
static void fourier_solids(Case *c, Sweep *w)
{
    if (c->s.nRegions == 0 || c->s.deltaT <= 0) return;
    FDFile *bm = case_dict(c, "system/blockMeshDict");
    BlockMeshInfo bi; memset(&bi, 0, sizeof bi); if (bm) blockmesh_info(bm, &bi);
    if (!bi.ok || bi.dxMin <= 0) return;
    for (int r = 0; r < c->s.nRegions; r++) {
        char rel[300]; snprintf(rel, sizeof rel, "constant/%s/thermophysicalProperties", c->s.regions[r]);
        FDFile *f = case_dict(c, rel);
        char type[64] = ""; if (!f || !fd_get(f, "thermoType/type", type, sizeof type) || !strstr(type, "solid")) continue;
        char k[48] = "", rho[48] = "", cp[48] = "";
        fd_get(f, "mixture/transport/kappa", k, sizeof k); fd_get(f, "mixture/equationOfState/rho", rho, sizeof rho);
        fd_get(f, "mixture/thermodynamics/Cp", cp, sizeof cp);
        if (!(atof(k) > 0 && atof(rho) > 0 && atof(cp) > 0)) continue;
        Finding *g = add(w, PC_OK); if (!g) return;
        double dx = bi.oneCellDir >= 0 ? bi.dxMinThin : bi.dxMin;
        char t[300]; fourier_text(atof(k), atof(rho), atof(cp), c->s.deltaT, dx, t, sizeof t);
        pl_fmt_ui(g->x.title, sizeof g->x.title, "Solid %s: Fourier number per step", c->s.regions[r]);
        pl_fmt_ui(g->x.text, sizeof g->x.text, "%s (κ %s W/(m·K), ρ %s kg/m³, c_p %s J/(kg·K), Δx from the blockMeshDict). Heat-transfer solvers "
                  "are implicit in solids, so a large value is stable; it means heat crosses many cells in one step.", t, k, rho, cp);
        pl_strlcpy(g->x.file, rel, sizeof g->x.file);
    }
    w->checks++;
}

/* ROADMAP 4.3 continuum check: the Knudsen number where the gas is thinnest. λ = k_B T/(√2 π d² p) against the mean cell
   size. Checked at the initial state, at each patch with a set pressure (an ambient or vacuum boundary) and, when the
   case has results, in the cell with the lowest pressure of the latest time. Compressible cases only (p in Pa). */
static double uniform_value(const char *raw, bool *ok)
{
    const char *t = raw;
    while (*t == ' ' || *t == '\t' || *t == '\n') t++;
    if (!strncmp(t, "uniform", 7)) t += 7;
    char *e; double v = strtod(t, &e);
    *ok = e != t;
    return v;
}

/* the lowest p of a written ascii field and the T in the same cell; false when not readable (binary, processor*) */
static bool lowest_p_cell(const char *casePath, const char *dir, double *pMin, double *tAt)
{
    char pp[PL_PATH], tp[PL_PATH];
    if (!pl_fmt(pp, sizeof pp, "%s/%s/p", casePath, dir) || !pl_fmt(tp, sizeof tp, "%s/%s/T", casePath, dir)) return false;
    char *a = pl_read_file(pp, NULL), *b = pl_read_file(tp, NULL);
    bool ok = false;
    const char *ia = a ? strstr(a, "internalField") : NULL, *ib = b ? strstr(b, "internalField") : NULL;
    if (ia && ib && strstr(a, "format      ascii") && strstr(ia, "nonuniform") && strstr(ia, "nonuniform") - ia < 40) {
        const char *la = strchr(ia, '('), *lb = strchr(ib, '(');
        long n = 0; const char *q = la ? la - 1 : NULL;
        while (q && q > ia && (*q == '\n' || *q == ' ')) q--;
        while (q && q > ia && *q >= '0' && *q <= '9') q--;
        if (q) n = atol(q + 1);
        bool tUniform = !strstr(ib, "nonuniform") || strstr(ib, "nonuniform") - ib > 40;
        double tU = tUniform ? uniform_value(ib + 13, &ok) : 0;
        if (la && n > 0 && (tUniform || lb)) {
            char *ea = (char *)la + 1, *eb = lb ? (char *)lb + 1 : NULL;
            *pMin = INFINITY;
            for (long i = 0; i < n; i++) {
                char *na; double pv = strtod(ea, &na);
                if (na == ea) break;
                ea = na;
                double tv = tU;
                if (!tUniform) { char *nb; tv = strtod(eb, &nb); if (nb == eb) break; eb = nb; }
                if (pv > 0 && pv < *pMin) { *pMin = pv; *tAt = tv; }
            }
            ok = isfinite(*pMin);
        }
    }
    free(a); free(b);
    return ok;
}

static void knudsen(Case *c, Sweep *w)
{
    char fdir[160]; pl_strlcpy(fdir, c->fieldDir, sizeof fdir);
    char rel[300]; snprintf(rel, sizeof rel, "%s/p", fdir);
    FDFile *pf = case_dict(c, rel);
    if (!pf && pl_starts_with(fdir, "0.orig")) {          /* 0.orig holding .gz fields, 0 the setFields result */
        snprintf(fdir, sizeof fdir, "0%s", c->fieldDir + 6);
        snprintf(rel, sizeof rel, "%s/p", fdir);
        pf = case_dict(c, rel);
    }
    char dims[64] = "";
    if (!pf || !pf->ok || pf->binary || !fd_get(pf, "dimensions", dims, sizeof dims)) return;
    double dv[7] = { 0 };
    if (sscanf(dims, "[%lf %lf %lf", &dv[0], &dv[1], &dv[2]) != 3 || dv[0] != 1 || dv[1] != -1 || dv[2] != -2) return;   /* Pa */
    snprintf(rel, sizeof rel, "%s/T", fdir);
    FDFile *tf = case_dict(c, rel);
    if (!tf || !tf->ok || tf->binary) return;
    /* the gas, from the molar mass of the mixture */
    char thermo[300];
    if (c->region[0]) snprintf(thermo, sizeof thermo, "constant/%s/thermophysicalProperties", c->region);
    else snprintf(thermo, sizeof thermo, "constant/thermophysicalProperties");
    FDFile *th = case_dict(c, thermo);
    char mw[48] = "";
    if (th) fd_get(th, "mixture/specie/molWeight", mw, sizeof mw);
    if (mw[0] && atof(mw) > 400) return;                  /* a non-dimensional setup (obliqueShock: p 1, T 1, molWeight 11640) */
    const GasDiameter *g = mw[0] ? gas_for_molweight(atof(mw)) : NULL;
    double d = g ? g->d : 4.19e-10;
    /* the mean cell size: the domain's extent over the cell count (area over cells for a 2-D or wedge case) */
    double lo[3], hi[3]; long cells = c->s.nCells;
    bool have = c->s.meshed && mesh_bounds(c->s.path, lo, hi);
    if (!have) {
        FDFile *bm = case_dict(c, "system/blockMeshDict");
        BlockMeshInfo bi; memset(&bi, 0, sizeof bi);
        if (bm) blockmesh_info(bm, &bi);
        if (bi.ok) { memcpy(lo, bi.lo, sizeof lo); memcpy(hi, bi.hi, sizeof hi); cells = bi.cells; have = true; }
    }
    if (!have || cells <= 0) return;
    double e[3], emax = 0, vol = 1; int nd = 0;
    for (int i = 0; i < 3; i++) { e[i] = hi[i] - lo[i]; if (e[i] > emax) emax = e[i]; }
    for (int i = 0; i < 3; i++) if (e[i] > 0.02 * emax) { vol *= e[i]; nd++; }
    double dx = nd == 3 ? cbrt(vol / (double)cells) : nd == 2 ? sqrt(vol / (double)cells) : vol / (double)cells;
    if (!(dx > 0)) return;

    /* the samples: where the gas is */
    typedef struct { char where[96]; double p, T; } Sample;
    Sample sm[24]; int ns = 0;
    char raw[200] = ""; bool ok;
    double tInt = 0, pInt = 0;
    FDEntry *ie = fd_find(tf, "internalField");
    if (ie) { fd_entry_value(tf, ie, raw, sizeof raw); tInt = uniform_value(raw, &ok); if (!ok) tInt = 0; }
    ie = fd_find(pf, "internalField");
    if (ie) { fd_entry_value(pf, ie, raw, sizeof raw); pInt = uniform_value(raw, &ok); if (ok && tInt > 0 && pInt > 0) { snprintf(sm[ns].where, sizeof sm[0].where, "initial field"); sm[ns].p = pInt; sm[ns++].T = tInt; } }
    if (ns == 0) {                                          /* a non-uniform start (setFields, mapFields): its thinnest cell */
        double pm = 0, ta = 0;
        if (lowest_p_cell(c->s.path, fdir, &pm, &ta) && ta > 0) { snprintf(sm[ns].where, sizeof sm[0].where, "lowest pressure of the initial field"); sm[ns].p = pm; sm[ns++].T = ta; }
        if (tInt <= 0) tInt = ta;
    }
    FDEntry *bf = fd_find(pf, "boundaryField"), *tbf = fd_find(tf, "boundaryField");
    for (int i = 0; bf && bf->kind == FD_DICT && i < bf->nChild && ns < 20; i++) {
        FDEntry *b = &bf->child[i];
        if (b->kind != FD_DICT) continue;
        char type[64] = "", path[300];
        snprintf(path, sizeof path, "boundaryField/%s/type", b->key); fd_get(pf, path, type, sizeof type);
        const char *vk = !strcmp(type, "totalPressure") ? "p0" : (!strcmp(type, "fixedValue") || strstr(type, "waveTransmissive") || strstr(type, "Pressure")) ? "value" : NULL;
        if (!vk || !strcmp(type, "fixedFluxPressure") || !strcmp(type, "calculated")) continue;
        snprintf(path, sizeof path, "boundaryField/%s/%s", b->key, vk);
        if (!fd_get(pf, path, raw, sizeof raw)) continue;
        double pv = uniform_value(raw, &ok);
        if (!ok || pv <= 0) continue;
        double tv = tInt;
        FDEntry *tb = tbf ? fd_child(tbf, b->key) : NULL;
        if (tb) { snprintf(path, sizeof path, "boundaryField/%s/value", b->key); if (fd_get(tf, path, raw, sizeof raw)) { double t2 = uniform_value(raw, &ok); if (ok && t2 > 0) tv = t2; } }
        if (tv <= 0) continue;
        snprintf(sm[ns].where, sizeof sm[0].where, "patch %s (%s)", b->key, type);
        sm[ns].p = pv; sm[ns++].T = tv;
    }
    const char *latest = case_latest_result(&c->s);
    char ldir[300];
    if (latest[0] && ns < 24) {
        if (c->region[0]) snprintf(ldir, sizeof ldir, "%s/%s", latest, c->region); else snprintf(ldir, sizeof ldir, "%s", latest);
        double pm = 0, ta = 0;
        if (lowest_p_cell(c->s.path, ldir, &pm, &ta) && ta > 0) { snprintf(sm[ns].where, sizeof sm[0].where, "lowest pressure at t = %s", latest); sm[ns].p = pm; sm[ns++].T = ta; }
    }
    if (ns == 0) return;
    int worst = 0; double knw = 0;
    for (int i = 0; i < ns; i++) { double kn = mean_free_path(sm[i].T, sm[i].p, d) / dx; if (kn > knw) { knw = kn; worst = i; } }
    Finding *f = add(w, knw >= 0.01 ? PC_WARN : PC_OK);
    if (!f) return;
    char t[400]; knudsen_text(sm[worst].T, sm[worst].p, d, dx, t, sizeof t);
    char kns[32]; human_num(knw, kns, sizeof kns);
    if (knw < 0.01) pl_fmt_ui(f->x.title, sizeof f->x.title, "Continuum flow everywhere checked (Kn ≤ %s)", kns);
    else if (knw < 0.1) pl_fmt_ui(f->x.title, sizeof f->x.title, "Slip flow at the %s: Kn = %s", sm[worst].where, kns);
    else pl_fmt_ui(f->x.title, sizeof f->x.title, "Continuum CFD is not valid at the %s: Kn = %s", sm[worst].where, kns);
    Str b; str_init(&b);
    str_catf(&b, "At the %s, p = %g Pa, T = %g K: %s. Gas: %s (d = %g m, from molWeight %s); Δx is the mean cell size, the domain's %s over %ld cells; refined "
             "cells have a larger Kn.", sm[worst].where, sm[worst].p, sm[worst].T, t, g ? g->name : "taken as air", d, mw[0] ? mw : "not set",
             nd == 3 ? "volume" : "area", cells);
    if (knw >= 0.1) str_cat(&b, " Above 0.1 the Navier-Stokes equations stop describing the gas: use DSMC there (dsmcFoam, or SPARTA), "
                                "or cut the domain where Kn passes 0.1.");
    else if (knw >= 0.01) str_cat(&b, " Between 0.01 and 0.1 the gas slips at walls: no-slip overpredicts shear and heat flux; "
                                     "maxwellSlipU on U and smoluchowskiJumpT on T model it.");
    if (ns > 1) {
        str_cat(&b, " Elsewhere:");
        for (int i = 0; i < ns; i++) {
            if (i == worst) continue;
            double kn = mean_free_path(sm[i].T, sm[i].p, d) / dx;
            char k2[32]; human_num(kn, k2, sizeof k2);
            str_catf(&b, " %s Kn %s (%s);", sm[i].where, k2, knudsen_regime(kn));
        }
        if (b.len && b.s[b.len - 1] == ';') b.s[--b.len] = 0;
        str_cat(&b, ".");
    }
    pl_fmt_ui(f->x.text, sizeof f->x.text, "%s", b.s ? b.s : "");
    str_free(&b);
    pl_fmt(f->x.file, sizeof f->x.file, "%s/p", fdir);
    w->checks++;
}

/* field files whose dimensions can't be right for the field, whatever the solver (H-10): U in m²/s, T not in K */
static void field_dims(Case *c, Sweep *w)
{
    for (int fi = 0; fi < c->fields.n; fi++) {
        const char *want = field_expected_dims(c->fields.names[fi]);
        if (!want) continue;
        char rel[300]; snprintf(rel, sizeof rel, "%s/%s", c->fieldDir, c->fields.names[fi]);
        FDFile *f = case_dict(c, rel);
        char dims[96] = "";
        if (!f || !f->ok || f->binary || !fd_get(f, "dimensions", dims, sizeof dims) || dims[0] != '[' || dims_equal(dims, want)) continue;
        double d0; if (sscanf(dims + 1, "%lf", &d0) != 1) continue;   /* named units ("[m^2 s^-3]"): fine as they are */
        Finding *g = add(w, PC_BAD); if (!g) return;
        Explanation *x = &g->x;
        char ug[48] = "", uw[48] = ""; dims_unit(dims, ug, sizeof ug); dims_unit(want, uw, sizeof uw);
        pl_fmt_ui(x->title, sizeof x->title, "%s has dimensions %s, not %s", rel, dims, want);
        pl_fmt_ui(x->text, sizeof x->text, "%s is in %s %s here, but %s is always %s %s: the solver stops at the first equation that adds "
                  "two of its terms (\"Incompatible dimensions\").", c->fields.names[fi], ug, dims, c->fields.names[fi], uw, want);
        pl_strlcpy(x->file, rel, sizeof x->file);
        FDEntry *e = fd_find(f, "dimensions"); x->line = e ? e->line : 0;
        x->fix = FIX_PUT; pl_strlcpy(x->fixFile, rel, sizeof x->fixFile); pl_strlcpy(x->putPath[0], "dimensions", sizeof x->putPath[0]);
        pl_strlcpy(x->putVal[0], want, sizeof x->putVal[0]); x->nPut = 1;
        pl_fmt_ui(x->fixLabel, sizeof x->fixLabel, "Set them to %s %s", uw, want);
    }
    w->checks++;
}

/* a near-miss of a solution-control keyword (nCorrector for nCorrectors): OpenFOAM ignores the unknown word and uses
   its default without a word (H-11; two shipped tutorials have "nCOrrectors") */
static void control_keywords(Case *c, Sweep *w)
{
    static const char *const known[] = { "nNonOrthogonalCorrectors", "nCorrectors", "nOuterCorrectors", "momentumPredictor",
        "pRefValue", "pRefCell", "correctPhi", "residualControl", "consistent", "transonic", "pMinFactor", "pMaxFactor",
        "pRefPoint", "moveMeshOuterCorrectors", "faceMomentum", "rDeltaTSmoothingCoeff", "maxDeltaT", "maxCo", "rhoMax",
        "rDeltaTDampingCoeff", "turbOnFinalIterOnly", "rhoMin", "nEnergyCorrectors", "frozenFlow", "alphaTemp",
        "solvePrimaryRegion", "oversetAdjustPhi", "nHydrostaticCorrectors", "hydrostaticInitialization", "nUCorrectors",
        "nAlphaSubCycles", "nAlphaCorr", "finalOnLastPimpleIterOnly", "checkMeshCourantNo", "cAlpha", "pMin", "pMax", "alphaY",
        "Yref", "nAlphaSweepIter", "nAlphaSpreadIter", "meshFluxCorrection", "maxAlphaCo", "energyCoupling", "SIMPLErho",
        "outerCorrectorResidualControl", "ddtCorr", "nCorrPIMPLE", "nCorrPISO", "nonOrthogonalCorrectors", "ddtPhiCoeff", NULL };
    int nk = 0; while (known[nk]) nk++;
    FDFile *f = case_dict(c, "system/fvSolution");
    if (!f || !f->ok) return;
    static const char *const secs[] = { "PISO", "PIMPLE", "SIMPLE", NULL };
    for (int si = 0; secs[si]; si++) {
        FDEntry *d = fd_find(f, secs[si]);
        if (!d || d->kind != FD_DICT) continue;
        for (int i = 0; i < d->nChild; i++) {
            const char *k = d->child[i].key;
            if (!k || d->child[i].kind == FD_DIRECTIVE || k[0] == '"' || k[0] == '$') continue;
            bool isKnown = false;
            for (int j = 0; j < nk && !isKnown; j++) if (!strcmp(k, known[j])) isKnown = true;
            if (isKnown) continue;
            const char *near = closest_word(k, known, nk);
            if (!near) continue;
            bool there = false;                                       /* the right one is set too: just a leftover */
            for (int j = 0; j < d->nChild; j++) if (d->child[j].key && !strcmp(d->child[j].key, near)) there = true;
            Finding *g = add(w, PC_WARN); if (!g) return;
            Explanation *x = &g->x;
            char v[64] = ""; fd_entry_value(f, &d->child[i], v, sizeof v);
            pl_fmt_ui(x->title, sizeof x->title, "%s/%s isn't a keyword OpenFOAM reads: %s?", secs[si], k, near);
            char dflt[120] = "";
            if (!there) pl_fmt_ui(dflt, sizeof dflt, ", and uses its default for %s", near);
            pl_fmt_ui(x->text, sizeof x->text, "OpenFOAM ignores %s %s in %s without saying so%s. A misspelt corrector count changes how the "
                      "pressure and velocity are coupled, and a run can then blow up for no visible reason.%s%s", k, v, secs[si], dflt,
                      there ? " The right keyword is set as well, so this line does nothing: " : "", there ? near : "");
            pl_strlcpy(x->file, "system/fvSolution", sizeof x->file); x->line = d->child[i].line;
            if (!there) {
                x->fix = FIX_RENAME; pl_strlcpy(x->fixFile, "system/fvSolution", sizeof x->fixFile);
                pl_fmt_ui(x->fixPath, sizeof x->fixPath, "%s/%s", secs[si], k); pl_strlcpy(x->fixValue, near, sizeof x->fixValue);
                pl_fmt_ui(x->fixLabel, sizeof x->fixLabel, "Rename it %s", near);
            }
        }
    }
    w->checks++;
}

static int cmpf(const void *a, const void *b) { return ((const Finding *)b)->level - ((const Finding *)a)->level; }

int sensor_sweep(Case *c, Sweep *w)
{
    memset(w, 0, sizeof *w);
    if (!c) return 0;
    coverage(c, w);
    schemes_solvers(c, w);
    pressure_units(c, w);
    decomposition(c, w);
    regions(c, w);
    output(c, w);
    leftovers(c, w);
    fourier_solids(c, w);
    knudsen(c, w);
    field_dims(c, w);
    control_keywords(c, w);
    qsort(w->f, (size_t)w->n, sizeof w->f[0], cmpf);
    for (int i = 0; i < w->n; i++) if (w->f[i].ask || w->f[i].level >= PC_BAD) w->ask = true;
    return w->worst;
}
