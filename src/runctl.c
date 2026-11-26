/* runctl.c -- what a solver lets you control when it runs, and icoFoam/pisoFoam -> pimpleFoam (see runctl.h) */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "foamdict.h"
#include "runctl.h"

#define T  RC_KNOWN
#define AD RC_ADJUST
#define ST RC_STEADY
#define MP RC_MOMPRED
static const RcSolver TABLE[] = {
    /* incompressible */
    { "icoFoam",                  T | RC_PREF,                    RC_ALG_PISO,   "pimpleFoam" },
    { "nonNewtonianIcoFoam",      T | RC_PREF,                    RC_ALG_PISO,   "pimpleFoam" },
    { "pisoFoam",                 T | RC_PREF | MP,               RC_ALG_PISO,   "pimpleFoam" },
    { "pimpleFoam",               T | AD | RC_PREF | MP,          RC_ALG_PIMPLE, NULL },
    { "overPimpleDyMFoam",        T | AD | RC_PREF | MP,          RC_ALG_PIMPLE, NULL },
    { "SRFPimpleFoam",            T | AD | RC_PREF | MP,          RC_ALG_PIMPLE, NULL },
    { "simpleFoam",               T | ST | RC_PREF | MP,          RC_ALG_SIMPLE, NULL },
    { "overSimpleFoam",           T | ST | RC_PREF | MP,          RC_ALG_SIMPLE, NULL },
    { "SRFSimpleFoam",            T | ST | RC_PREF | MP,          RC_ALG_SIMPLE, NULL },
    { "porousSimpleFoam",         T | ST | RC_PREF | MP,          RC_ALG_SIMPLE, NULL },
    { "adjointShapeOptimizationFoam", T | ST | RC_PREF,           RC_ALG_SIMPLE, NULL },
    { "boundaryFoam",             T | ST,                         RC_ALG_NONE,   NULL },
    { "potentialFoam",            T | ST | RC_PREF,               RC_ALG_POTENTIAL, NULL },
    { "overPotentialFoam",        T | ST | RC_PREF,               RC_ALG_POTENTIAL, NULL },
    { "shallowWaterFoam",         T,                              RC_ALG_PIMPLE, NULL },
    /* heat transfer */
    { "buoyantSimpleFoam",        T | ST | MP,                    RC_ALG_SIMPLE, NULL },
    { "buoyantBoussinesqSimpleFoam", T | ST | MP,                 RC_ALG_SIMPLE, NULL },
    { "buoyantPimpleFoam",        T | AD | MP,                    RC_ALG_PIMPLE, NULL },
    { "buoyantBoussinesqPimpleFoam", T | AD | MP,                 RC_ALG_PIMPLE, NULL },
    { "overBuoyantPimpleDyMFoam", T | AD | MP,                    RC_ALG_PIMPLE, NULL },
    { "chtMultiRegionFoam",       T | AD | RC_DI,                 RC_ALG_PIMPLE, NULL },
    { "chtMultiRegionSimpleFoam", T | ST,                         RC_ALG_SIMPLE, NULL },
    { "chtMultiRegionTwoPhaseEulerFoam", T,                       RC_ALG_PIMPLE, NULL },
    /* compressible */
    { "rhoSimpleFoam",            T | ST | RC_TRANSONIC | MP,     RC_ALG_SIMPLE, NULL },
    { "rhoPorousSimpleFoam",      T | ST | RC_TRANSONIC | MP,     RC_ALG_SIMPLE, NULL },
    { "overRhoSimpleFoam",        T | ST | RC_TRANSONIC | MP,     RC_ALG_SIMPLE, NULL },
    { "rhoPimpleFoam",            T | AD | RC_TRANSONIC | MP,     RC_ALG_PIMPLE, NULL },
    { "overRhoPimpleDyMFoam",     T | AD | RC_TRANSONIC | MP,     RC_ALG_PIMPLE, NULL },
    { "rhoPimpleAdiabaticFoam",   T | AD | RC_TRANSONIC | MP,     RC_ALG_PIMPLE, NULL },
    { "rhoCentralFoam",           T | AD,                         RC_ALG_NONE,   NULL },
    { "sonicFoam",                T | RC_TRANSONIC | MP,          RC_ALG_PIMPLE, NULL },
    { "sonicDyMFoam",             T | AD | RC_TRANSONIC | MP,     RC_ALG_PIMPLE, NULL },
    { "sonicLiquidFoam",          T | MP,                         RC_ALG_PIMPLE, NULL },
    { "acousticFoam",             T,                              RC_ALG_PIMPLE, NULL },
    /* multiphase */
    { "interFoam",                T | AD | RC_ALPHACO | RC_VOF | MP | RC_PREF, RC_ALG_PIMPLE, NULL },
    { "interIsoFoam",             T | AD | RC_ALPHACO | RC_VOF | MP | RC_PREF, RC_ALG_PIMPLE, NULL },
    { "overInterDyMFoam",         T | AD | RC_ALPHACO | RC_VOF | MP | RC_PREF, RC_ALG_PIMPLE, NULL },
    { "interMixingFoam",          T | AD | RC_ALPHACO | RC_VOF | MP, RC_ALG_PIMPLE, NULL },
    { "interPhaseChangeFoam",     T | AD | RC_ALPHACO | RC_VOF | MP, RC_ALG_PIMPLE, NULL },
    { "interCondensatingEvaporatingFoam", T | AD | RC_ALPHACO | RC_VOF | MP, RC_ALG_PIMPLE, NULL },
    { "multiphaseInterFoam",      T | AD | RC_ALPHACO | RC_VOF | MP, RC_ALG_PIMPLE, NULL },
    { "compressibleInterFoam",    T | AD | RC_ALPHACO | RC_VOF | MP, RC_ALG_PIMPLE, NULL },
    { "compressibleInterIsoFoam", T | AD | RC_ALPHACO | RC_VOF | MP, RC_ALG_PIMPLE, NULL },
    { "compressibleMultiphaseInterFoam", T | AD | RC_ALPHACO | RC_VOF | MP, RC_ALG_PIMPLE, NULL },
    { "driftFluxFoam",            T | AD | RC_ALPHACO | RC_VOF | MP, RC_ALG_PIMPLE, NULL },
    { "twoLiquidMixingFoam",      T | AD | RC_ALPHACO | RC_VOF | MP, RC_ALG_PIMPLE, NULL },
    { "icoReactingMultiphaseInterFoam", T | AD | RC_ALPHACO | RC_DI | MP, RC_ALG_PIMPLE, NULL },
    { "cavitatingFoam",           T | AD | RC_ACOUSTIC | MP,      RC_ALG_PIMPLE, NULL },
    { "cavitatingDyMFoam",        T | AD | RC_ACOUSTIC | MP,      RC_ALG_PIMPLE, NULL },
    { "multiphaseEulerFoam",      T | AD,                         RC_ALG_PIMPLE, NULL },
    { "reactingMultiphaseEulerFoam", T | AD,                      RC_ALG_PIMPLE, NULL },
    { "twoPhaseEulerFoam",        T | AD,                         RC_ALG_PIMPLE, NULL },
    { "reactingTwoPhaseEulerFoam", T | AD,                        RC_ALG_PIMPLE, NULL },
    { "potentialFreeSurfaceFoam", T | AD | MP,                    RC_ALG_PIMPLE, NULL },
    { "MPPICInterFoam",           T | AD | RC_VOF,                RC_ALG_PIMPLE, NULL },
    /* combustion, particles */
    { "reactingFoam",             T | AD | RC_TRANSONIC | MP,     RC_ALG_PIMPLE, NULL },
    { "rhoReactingFoam",          T | AD | RC_TRANSONIC | MP,     RC_ALG_PIMPLE, NULL },
    { "rhoReactingBuoyantFoam",   T | AD | MP,                    RC_ALG_PIMPLE, NULL },
    { "XiFoam",                   T | AD | RC_TRANSONIC | MP,     RC_ALG_PIMPLE, NULL },
    { "PDRFoam",                  T | AD | MP,                    RC_ALG_PIMPLE, NULL },
    { "fireFoam",                 T | AD | MP,                    RC_ALG_PIMPLE, NULL },
    { "sprayFoam",                T | AD | MP,                    RC_ALG_PIMPLE, NULL },
    { "coalChemistryFoam",        T | AD | MP,                    RC_ALG_PIMPLE, NULL },
    { "reactingParcelFoam",       T | AD | MP,                    RC_ALG_PIMPLE, NULL },
    { "kinematicParcelFoam",      T | AD | MP,                    RC_ALG_PIMPLE, NULL },
    { "DPMFoam",                  T | AD | MP,                    RC_ALG_PIMPLE, NULL },
    { "MPPICFoam",                T | MP,                         RC_ALG_PIMPLE, NULL },
    { "simpleReactingParcelFoam", T | ST | MP,                    RC_ALG_SIMPLE, NULL },
    { "simpleCoalParcelFoam",     T | ST | MP,                    RC_ALG_SIMPLE, NULL },
    { "chemFoam",                 T | AD,                         RC_ALG_NONE,   NULL },
    /* basic, solids, electromagnetics */
    { "laplacianFoam",            T,                              RC_ALG_SIMPLE, NULL },
    { "overLaplacianDyMFoam",     T,                              RC_ALG_SIMPLE, NULL },
    { "scalarTransportFoam",      T,                              RC_ALG_SIMPLE, NULL },
    { "solidDisplacementFoam",    T,                              RC_ALG_STRESS, NULL },
    { "solidEquilibriumDisplacementFoam", T | ST,                 RC_ALG_STRESS, NULL },
    { "solidFoam",                T,                              RC_ALG_PIMPLE, NULL },
    { "thermoFoam",               T,                              RC_ALG_PIMPLE, NULL },
    { "mhdFoam",                  T,                              RC_ALG_PISO,   NULL },
    { "dnsFoam",                  T,                              RC_ALG_PISO,   NULL },
    { "electrostaticFoam",        T,                              RC_ALG_NONE,   NULL },
    { "magneticFoam",             T | ST,                         RC_ALG_SIMPLE, NULL },
    { "financialFoam",            T,                              RC_ALG_NONE,   NULL },
};
#undef T
#undef AD
#undef ST
#undef MP

const RcSolver *rc_solver(const char *app)
{
    if (!app) app = "";
    for (size_t i = 0; i < sizeof TABLE / sizeof TABLE[0]; i++) if (!strcmp(TABLE[i].app, app)) return &TABLE[i];
    /* a solver we don't know (a user's own, a newer one): guess from its name, the loop it reads is checked in the case */
    static RcSolver g;
    g.app = app; g.upgrade = NULL;
    if (strstr(app, "Simple") || strstr(app, "simple")) { g.flags = RC_STEADY | RC_MOMPRED; g.alg = RC_ALG_SIMPLE; }
    else if (strstr(app, "Pimple") || strstr(app, "pimple")) { g.flags = RC_ADJUST | RC_MOMPRED; g.alg = RC_ALG_PIMPLE; }
    else if (strstr(app, "Piso") || strstr(app, "piso")) { g.flags = RC_MOMPRED; g.alg = RC_ALG_PISO; }
    else if (strstr(app, "Inter") || strstr(app, "inter")) { g.flags = RC_ADJUST | RC_ALPHACO | RC_VOF | RC_MOMPRED; g.alg = RC_ALG_PIMPLE; }
    else { g.flags = 0; g.alg = RC_ALG_NONE; }
    return &g;
}

const char *rc_alg_dict(RcAlg a)
{
    switch (a) {
    case RC_ALG_PISO: return "PISO";
    case RC_ALG_PIMPLE: return "PIMPLE";
    case RC_ALG_SIMPLE: return "SIMPLE";
    case RC_ALG_POTENTIAL: return "potentialFlow";
    case RC_ALG_STRESS: return "stressAnalysis";
    default: return "";
    }
}

const char *rc_alg_in(FDFile *fs, const RcSolver *s)
{
    const char *want = rc_alg_dict(s->alg);
    if (fs && want[0] && fd_find(fs, want)) return want;
    static const char *const ALL[] = { "PIMPLE", "PISO", "SIMPLE", "potentialFlow", "stressAnalysis" };
    for (int i = 0; fs && i < 5; i++) { FDEntry *e = fd_find(fs, ALL[i]); if (e && e->kind == FD_DICT) return ALL[i]; }
    return want;
}

double rc_dt_for_courant(double dt, double co, double target)
{
    if (!(dt > 0) || !(co > 1e-12) || !(target > 0) || !isfinite(co)) return 0;
    double v = dt * target / co;
    /* two significant figures, rounded down: a tidy number that stays under the target */
    double p = pow(10, floor(log10(v)) - 1);
    return floor(v / p) * p;
}

static bool on_word(const char *v) { char w[16]; fd_word(v, w, sizeof w); return !strcmp(w, "yes") || !strcmp(w, "on") || !strcmp(w, "true") || !strcmp(w, "1"); }

bool rc_write_by_time(FDFile *cd, char *msg, int cap)
{
    char wc[32] = "", wi[64] = "", dt[64] = "";
    if (msg && cap) msg[0] = 0;
    if (!cd || !fd_get(cd, "writeControl", wc, sizeof wc)) return false;
    char w[32]; fd_word(wc, w, sizeof w);
    if (!strcmp(w, "runTime")) {                      /* same seconds, but written exactly at them */
        fd_put(cd, "writeControl", "adjustable");
        if (msg) snprintf(msg, (size_t)cap, "writeControl runTime -> adjustable");
        return true;
    }
    if (strcmp(w, "timeStep")) return false;
    fd_get_resolved(cd, "writeInterval", wi, sizeof wi);
    fd_get_resolved(cd, "deltaT", dt, sizeof dt);
    double n = atof(wi), d = atof(dt);
    if (!(n > 0) || !(d > 0)) return false;
    char nv[64]; snprintf(nv, sizeof nv, "%.6g", n * d);
    fd_put(cd, "writeControl", "adjustable");
    fd_put(cd, "writeInterval", nv);
    if (msg) snprintf(msg, (size_t)cap, "results written every %s s (was every %g steps)", nv, n);
    return true;
}

/* add "<key>Final" after solvers/<key>: the same settings with relTol 0 (PIMPLE solves its last corrector with it) */
static bool add_final(FDFile *fs, FDEntry *sol, int i)
{
    FDEntry *e = &sol->child[i];
    if (e->kind != FD_DICT) return false;
    char key[256]; snprintf(key, sizeof key, "%s", e->key);
    size_t kl = strlen(key);
    if (kl < 1 || kl > 200 || strstr(key, "Final")) return false;
    char fin[260];
    if (key[0] == '"' && key[kl - 1] == '"') snprintf(fin, sizeof fin, "%.*sFinal\"", (int)kl - 1, key);
    else snprintf(fin, sizeof fin, "%sFinal", key);
    /* already there? a literal "<key>Final", or a regular expression naming Final with this key in it */
    for (int j = 0; j < sol->nChild; j++) {
        const char *k = sol->child[j].key;
        if (!k || !strstr(k, "Final")) continue;
        if (!strcmp(k, fin)) return false;
        char bare[256]; snprintf(bare, sizeof bare, "%s", key[0] == '"' ? key + 1 : key);
        size_t bl = strlen(bare); if (bl && bare[bl - 1] == '"') bare[bl - 1] = 0;
        if (k[0] == '"' && strstr(k, bare)) return false;
    }
    int body0 = e->valStart, body1 = e->valEnd;
    char ind[64] = ""; { int a = e->start; while (a > 0 && fs->text[a - 1] != '\n') a--; int k = 0; while (a < e->start && k < 63) ind[k++] = fs->text[a++]; ind[k] = 0; }
    Str b; str_init(&b);
    str_catf(&b, "\n\n%s%s\n%s{", ind, fin, ind);
    str_catn(&b, fs->text + body0, (size_t)(body1 - body0));
    str_cat(&b, "}");
    int at = e->end;
    bool ok = fd_replace_span(fs, at, at, b.s);
    str_free(&b);
    if (!ok) return false;
    char p[300]; snprintf(p, sizeof p, "solvers/%s/relTol", fin);
    return fd_put(fs, p, "0");
}

int rc_to_pimple(Case *c, double maxCo, char *msg, int cap)
{
    if (msg && cap) msg[0] = 0;
    FDFile *cd = case_dict(c, "system/controlDict"), *fs = case_dict(c, "system/fvSolution");
    if (!cd || !fs) { if (msg) snprintf(msg, (size_t)cap, "the case has no system/%s", cd ? "fvSolution" : "controlDict"); return -1; }
    if (cd->binary || fs->binary || !cd->ok || !fs->ok) { if (msg) snprintf(msg, (size_t)cap, "system/%s can't be read cleanly", !cd->ok || cd->binary ? "controlDict" : "fvSolution"); return -1; }
    char app[64] = ""; fd_get(cd, "application", app, sizeof app);
    const RcSolver *s = rc_solver(app);
    if (!s->upgrade) { if (msg) snprintf(msg, (size_t)cap, "%s isn't one Q-Continuum can turn into pimpleFoam", app[0] ? app : "This solver"); return -1; }
    int n = 0;
    Str what; str_init(&what);
    /* fvSolution: PISO -> PIMPLE (one outer corrector: the same algorithm), and the Final solvers PIMPLE asks for */
    if (!fd_find(fs, "PIMPLE")) {
        FDEntry *pe = fd_find(fs, "PISO");
        if (pe) { fd_replace_span(fs, pe->keyStart, pe->keyEnd, "PIMPLE"); }
        else fd_ensure_dict(fs, "PIMPLE");
        n++; str_cat(&what, "PISO -> PIMPLE");
    }
    if (!fd_find(fs, "PIMPLE/nOuterCorrectors")) { fd_put(fs, "PIMPLE/nOuterCorrectors", "1"); n++; }
    if (!fd_find(fs, "PIMPLE/nCorrectors")) { fd_put(fs, "PIMPLE/nCorrectors", "2"); n++; }
    if ((s->flags & RC_PREF) && !fd_find(fs, "PIMPLE/pRefCell") && !fd_find(fs, "PIMPLE/pRefPoint")) {
        fd_put(fs, "PIMPLE/pRefCell", "0"); fd_put(fs, "PIMPLE/pRefValue", "0"); n++;
    }
    int nf = 0;
    for (int pass = 0; pass < 64; pass++) {           /* each insert re-parses: look again from the start */
        FDEntry *sol = fd_find(fs, "solvers");
        if (!sol || sol->kind != FD_DICT) break;
        bool added = false;
        for (int i = 0; i < sol->nChild && !added; i++) if (add_final(fs, sol, i)) { added = true; nf++; }
        if (!added) break;
    }
    if (nf) { n += nf; str_catf(&what, "%s%d Final solver%s", what.len ? ", " : "", nf, nf == 1 ? "" : "s"); }
    /* fvSchemes: pimpleFoam's laminar/turbulence model adds a div term icoFoam never needed */
    FDFile *sc = case_dict(c, "system/fvSchemes");
    if (sc && sc->ok && !sc->binary) {
        char def[64] = ""; fd_get(sc, "divSchemes/default", def, sizeof def);
        char dw[32] = ""; fd_word(def, dw, sizeof dw);
        if ((!def[0] || !strcmp(dw, "none")) && !fd_find(sc, "divSchemes/div((nuEff*dev2(T(grad(U)))))")) {
            fd_put(sc, "divSchemes/div((nuEff*dev2(T(grad(U)))))", "Gauss linear");
            n++; str_catf(&what, "%sdiv((nuEff*dev2(T(grad(U))))) scheme", what.len ? ", " : "");
        }
    }
    /* constant: a transport model, and laminar flow when the case has no turbulence model */
    FDFile *tp = case_dict(c, case_rpath(c, "constant/transportProperties"));
    if (tp && tp->ok && !tp->binary && !fd_find(tp, "transportModel")) {
        fd_put(tp, "transportModel", "Newtonian");
        n++; str_catf(&what, "%stransportModel Newtonian", what.len ? ", " : "");
    }
    if (!case_dict(c, case_rpath(c, "constant/turbulenceProperties")) && !case_dict(c, case_rpath(c, "constant/momentumTransport"))) {
        if (case_create_dict(c, case_rpath(c, "constant/turbulenceProperties"), "dictionary", "\nsimulationType  laminar;\n\n")) {
            n++; str_catf(&what, "%sconstant/turbulenceProperties (laminar)", what.len ? ", " : "");
        }
    }
    /* controlDict: the solver, an adjustable step, results written by time */
    fd_put(cd, "application", "pimpleFoam");
    char v[64] = "";
    if (!fd_get(cd, "adjustTimeStep", v, sizeof v) || !on_word(v)) fd_put(cd, "adjustTimeStep", "yes");
    if (!fd_find(cd, "maxCo")) { char m[32]; snprintf(m, sizeof m, "%g", maxCo > 0 ? maxCo : 1); fd_put(cd, "maxCo", m); }
    char wm[160];
    if (rc_write_by_time(cd, wm, sizeof wm)) str_catf(&what, "%s%s", what.len ? ", " : "", wm);
    n += 2;
    if (msg) snprintf(msg, (size_t)cap, "%s", what.len ? what.s : "controlDict only");
    str_free(&what);
    return n;
}
