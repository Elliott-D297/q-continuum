#define _GNU_SOURCE
/* simadopt.c -- reading a hand-made case into a SimSetup (workflow plan W2), split from simsetup.c (improvement plan A5,
 * 2026-11-26). The public functions are declared in simsetup.h. */
#include <ctype.h>
#include <regex.h>
#include <stdarg.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include "simsetup.h"
#include "foamcase.h"
#include "foamdict.h"

/* ============================================================================ adoption (workflow plan W2, 2026-11-14)
 * A case set up by hand (a tutorial, a folder from elsewhere) read into a setup: the solver says steady or transient, the
 * turbulence file the model, transportProperties the fluid, the controlDict the run's length, and each patch's conditions
 * in 0/U and 0/p its role and values (the way bc_entry writes them, read back). What the setup can't say is listed in
 * notes and stays as the case has it: sim_adopt_bases records what the setup would write for these settings as the merge's
 * base, so the next Write case files changes only what was changed in Setup. First version: incompressible single-phase
 * (simpleFoam, pimpleFoam, pisoFoam, icoFoam). */
static void note_add(char *notes, int cap, const char *fmt, ...)
{
    size_t n = strlen(notes);
    if ((int)n >= cap - 2) return;
    va_list ap; va_start(ap, fmt);
    vsnprintf(notes + n, (size_t)cap - n, fmt, ap);
    va_end(ap);
    n = strlen(notes);
    if ((int)n < cap - 1) { notes[n] = '\n'; notes[n + 1] = 0; }
}

/* a patch's entry in a field's boundaryField: by name, else the first quoted pattern ("(inlet|outlet)", ".*Wall") that matches */
static FDEntry *patch_entry(FDFile *f, const char *patch)
{
    FDEntry *bf = f ? fd_find(f, "boundaryField") : NULL;
    if (!bf || bf->kind != FD_DICT) return NULL;
    FDEntry *e = fd_child(bf, patch);
    if (e) return e;
    for (int i = 0; i < bf->nChild; i++) {
        FDEntry *c = &bf->child[i];
        if (!c->key || c->key[0] != '"') continue;
        char pat[256]; size_t n = strlen(c->key);
        if (n < 3 || n - 2 >= sizeof pat - 3) continue;
        snprintf(pat, sizeof pat, "^(%.*s)$", (int)(n - 2), c->key + 1);
        regex_t re;
        if (regcomp(&re, pat, REG_EXTENDED | REG_NOSUB)) continue;
        bool m = !regexec(&re, patch, 0, NULL, 0);
        regfree(&re);
        if (m) return c;
    }
    return NULL;
}
static bool entry_get(FDFile *f, FDEntry *e, const char *key, char *buf, int cap)
{
    FDEntry *c = e && e->kind == FD_DICT ? fd_child(e, key) : NULL;
    buf[0] = 0;
    return c && fd_entry_value(f, c, buf, cap);
}
/* "uniform (1 2 3)" or "(1 2 3)", "uniform 5" or "5": false for nonuniform values and expressions */
static bool uniform_vec(const char *v, double out[3])
{
    const char *q = strstr(v, "uniform"); q = q ? q + 7 : v;
    while (*q == ' ' || *q == '\t') q++;
    return sscanf(q, "(%lf %lf %lf)", &out[0], &out[1], &out[2]) == 3;
}
static bool uniform_num(const char *v, double *out)
{
    if (strstr(v, "nonuniform") || strchr(v, '(') || strchr(v, '#') || strchr(v, '$')) return false;
    const char *q = strstr(v, "uniform"); q = q ? q + 7 : v;
    char *e; double d = strtod(q, &e);
    if (e == q) return false;
    *out = d;
    return true;
}
static double vlen3(const double v[3]) { return sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

static FDFile *case_file(const char *casePath, const char *dir, const char *name)
{
    char rel[PL_PATH], p[PL_PATH];
    snprintf(rel, sizeof rel, "%s/%s", dir, name);
    pl_join(p, sizeof p, casePath, rel);
    return pl_is_file(p) ? fd_load(p) : NULL;
}

int sim_adopt(const char *casePath, const char *fieldDir, SimSetup *s, char *notes, int cap)
{
    notes[0] = 0;
    const char *fd = fieldDir && *fieldDir ? fieldDir : "0";
    FDFile *cd = case_file(casePath, "system", "controlDict");
    char app[64] = "";
    if (!cd || !fd_get(cd, "application", app, sizeof app)) { fd_free(cd); snprintf(notes, (size_t)cap, "No application in system/controlDict"); return 0; }
    bool steady = !strcmp(app, "simpleFoam"), piso = !strcmp(app, "pisoFoam"), ico = !strcmp(app, "icoFoam"), pimple = !strcmp(app, "pimpleFoam");
    if (!steady && !piso && !ico && !pimple) {
        fd_free(cd);
        snprintf(notes, (size_t)cap, "Setup reads incompressible single-phase cases for now (simpleFoam, pimpleFoam, pisoFoam, icoFoam); this one runs %s", app);
        return 0;
    }
    /* the turbulence model */
    int turb = TURB_LAMINAR;
    if (!ico) {
        FDFile *t = case_file(casePath, "constant", "momentumTransport");
        if (!t) t = case_file(casePath, "constant", "turbulenceProperties");
        char st[32] = "", model[64] = "";
        if (t && fd_get(t, "simulationType", st, sizeof st) && strcmp(st, "laminar")) {
            char k1[48], k2[48];
            snprintf(k1, sizeof k1, "%s/model", st); snprintf(k2, sizeof k2, "%s/%sModel", st, st);
            if (!fd_get(t, k1, model, sizeof model)) fd_get(t, k2, model, sizeof model);
            turb = -1;
            for (int k = 0; k < TURB__N; k++) if (model[0] && !strcmp(sim_model_word(k), model)) turb = k;
            if (turb < 0) {
                fd_free(t); fd_free(cd);
                snprintf(notes, (size_t)cap, "Setup has no %s %s model; this case keeps its dictionaries", st, model[0] ? model : "(unnamed)");
                return 0;
            }
        }
        fd_free(t);
    }
    s->turb = turb;
    s->flow = steady ? FLOW_STEADY : FLOW_TRANSIENT;
    s->algo = piso || ico ? ALG_PISO : ALG_PIMPLE;
    s->energy = EN_OFF; s->comp = COMP_INCOMP; s->phase = PH_SINGLE;
    s->tmpl = TMPL_BLANK;
    if (ico) note_add(notes, cap, "icoFoam: Setup writes this laminar transient case for pisoFoam; the case keeps running icoFoam until you change the solver");
    /* the run's length */
    char b[64];
    double et = 0, dt = 0, wi = 0;
    if (fd_get(cd, "endTime", b, sizeof b)) et = atof(b);
    if (fd_get(cd, "deltaT", b, sizeof b)) dt = atof(b);
    if (fd_get(cd, "writeInterval", b, sizeof b)) wi = atof(b);
    char wc[32] = ""; fd_get(cd, "writeControl", wc, sizeof wc);
    if (steady) {
        if (et > 0) s->iterations = et / (dt > 0 ? dt : 1);
        if (wi > 0) s->writeEvery = !strcmp(wc, "timeStep") ? wi : wi / (dt > 0 ? dt : 1);
    } else {
        if (et > 0) s->endTime = et;
        if (dt > 0) s->deltaT = dt;
        if (wi > 0) s->writeTime = !strcmp(wc, "timeStep") ? wi * (dt > 0 ? dt : 1) : wi;
        if (fd_get(cd, "maxCo", b, sizeof b)) s->maxCo = atof(b);
        if (fd_get(cd, "adjustTimeStep", b, sizeof b)) s->adjustDt = !strcmp(b, "yes") || !strcmp(b, "on") || !strcmp(b, "true");
        else s->adjustDt = false;
    }
    if (fd_find(cd, "functions")) note_add(notes, cap, "The controlDict's own function objects are kept as written");
    fd_free(cd);
    /* the fluid: incompressible cases give the kinematic viscosity only */
    FDFile *tp = case_file(casePath, "constant", "transportProperties");
    if (!tp) tp = case_file(casePath, "constant", "physicalProperties");
    double nu = 0;
    if (tp && fd_get(tp, "nu", b, sizeof b)) {
        const char *q = strrchr(b, ']'); nu = atof(q ? q + 1 : b);
    }
    char vm[48] = "";
    if (tp && fd_get(tp, "transportModel", vm, sizeof vm) && strcmp(vm, "Newtonian")) note_add(notes, cap, "The %s viscosity model is kept as written", vm);
    fd_free(tp);
    if (nu > 0) {
        if (fabs(nu - 1.516e-5) < 0.05 * 1.516e-5) sim_fluid(s, FLUID_AIR);
        else if (fabs(nu - 1.004e-6) < 0.05 * 1.004e-6) sim_fluid(s, FLUID_WATER);
        else {
            s->fluid = FLUID_CUSTOM; s->nu = nu;
            s->rho = nu < 5e-6 ? 1000 : 1.2;                /* the case has no density: a liquid's or a gas's by the viscosity */
            sim_fluid(s, FLUID_CUSTOM);
            note_add(notes, cap, "The case gives no density (its pressure is per unit density): %.4g kg/m³ assumed for reports in Pa; set it on Materials", s->rho);
        }
    } else note_add(notes, cap, "No viscosity found in constant/: Setup's default fluid shown");
    /* the starting values and each patch's conditions */
    FDFile *U = case_file(casePath, fd, "U"), *Pf = case_file(casePath, fd, "p");
    if (!U || !Pf) {
        fd_free(U); fd_free(Pf);
        snprintf(notes, (size_t)cap, "No %s/U or %s/p to read the boundaries from", fd, fd);
        return 0;
    }
    FDFile *K = case_file(casePath, fd, "k"), *EP = case_file(casePath, fd, "epsilon"), *OM = case_file(casePath, fd, "omega");
    if (fd_get(U, "internalField", b, sizeof b)) uniform_vec(b, s->U0);
    double p0;
    if (fd_get(Pf, "internalField", b, sizeof b) && uniform_num(b, &p0)) s->p0 = p0 * s->rho;
    for (int i = 0; i < s->nB; i++) {
        SimBoundary *bd = &s->b[i];
        if (sim_constraint(bd->meshType)) continue;
        FDEntry *ue = patch_entry(U, bd->name), *pe = patch_entry(Pf, bd->name);
        char ut[64] = "", pt[64] = "", v[128];
        entry_get(U, ue, "type", ut, sizeof ut);
        entry_get(Pf, pe, "type", pt, sizeof pt);
        bool wallPatch = !strcmp(bd->meshType, "wall");
        double vec[3] = { 0, 0, 0 }, pv = 0;
        bool hasVec = entry_get(U, ue, "value", v, sizeof v) && uniform_vec(v, vec);
        bool pFixed = !strcmp(pt, "fixedValue") && entry_get(Pf, pe, "value", v, sizeof v) && uniform_num(v, &pv);
        bool pTotal = !strcmp(pt, "totalPressure") && entry_get(Pf, pe, "p0", v, sizeof v) && uniform_num(v, &pv);
        int role = -1;
        if (!strcmp(ut, "noSlip") || (!strcmp(ut, "fixedValue") && hasVec && vlen3(vec) == 0 && (wallPatch || strcmp(pt, "fixedValue")))) {
            role = ROLE_WALL; bd->wall = WL_NOSLIP;
        } else if (!strcmp(ut, "movingWallVelocity") || (!strcmp(ut, "fixedValue") && hasVec && wallPatch)) {
            role = ROLE_WALL; bd->wall = WL_MOVING; memcpy(bd->wallU, vec, sizeof vec);
        } else if (!strcmp(ut, "slip")) {
            role = strstr(bd->name, "sym") ? ROLE_SYMMETRY : ROLE_WALL; bd->wall = WL_SLIP;
        } else if (!strcmp(ut, "fixedValue") && hasVec) {
            role = ROLE_INLET; bd->inlet = IN_VECTOR; memcpy(bd->vec, vec, sizeof vec);
        } else if (!strcmp(ut, "surfaceNormalFixedValue") && entry_get(U, ue, "refValue", v, sizeof v) && uniform_num(v, &bd->speed)) {
            role = ROLE_INLET; bd->inlet = IN_SPEED; bd->speed = fabs(bd->speed);
        } else if (!strcmp(ut, "flowRateInletVelocity")) {
            role = ROLE_INLET;
            if (entry_get(U, ue, "volumetricFlowRate", v, sizeof v) && uniform_num(v, &bd->flow)) bd->inlet = IN_FLOW;
            else if (entry_get(U, ue, "massFlowRate", v, sizeof v) && uniform_num(v, &bd->mdot)) bd->inlet = IN_MASS;
            else { role = -1; }
        } else if (!strcmp(ut, "freestreamVelocity") || !strcmp(ut, "freestream")) {
            role = ROLE_OPENING; bd->farField = true;
            if (entry_get(U, ue, "freestreamValue", v, sizeof v)) uniform_vec(v, bd->vec); else memcpy(bd->vec, vec, sizeof vec);
            if (entry_get(Pf, pe, "freestreamValue", v, sizeof v) && uniform_num(v, &pv)) bd->p = pv * s->rho;
        } else if (!strcmp(ut, "pressureInletOutletVelocity") && pTotal) {
            bool in = strstr(bd->name, "inlet") || strstr(bd->name, "Inlet") || strstr(bd->name, "inflow");
            role = in ? ROLE_INLET : ROLE_OPENING; if (in) bd->inlet = IN_PRESSURE; bd->p = pv * s->rho;
        } else if ((!strcmp(ut, "zeroGradient") || !strcmp(ut, "inletOutlet") || !strcmp(ut, "pressureInletOutletVelocity")) && (pFixed || pTotal)) {
            role = pTotal ? ROLE_OPENING : ROLE_OUTLET; bd->p = pv * s->rho;
        }
        if (role < 0) {
            note_add(notes, cap, "%s: U is %s and p is %s, which Setup doesn't name; its conditions are kept as written", bd->name, ut[0] ? ut : "(none)", pt[0] ? pt : "(none)");
            continue;
        }
        bd->role = role;
        /* an inlet's turbulence: an intensity from k (given, or from its value and the speed), a length from epsilon or omega */
        if (role == ROLE_INLET && turb != TURB_LAMINAR && K) {
            FDEntry *ke = patch_entry(K, bd->name);
            char kt[64] = ""; entry_get(K, ke, "type", kt, sizeof kt);
            double Ub = bd->inlet == IN_VECTOR ? vlen3(bd->vec) : bd->inlet == IN_SPEED ? bd->speed : 0, kv = 0, I = 0;
            if (!strcmp(kt, "turbulentIntensityKineticEnergyInlet") && entry_get(K, ke, "intensity", v, sizeof v)) I = atof(v) * 100;
            else if (entry_get(K, ke, "value", v, sizeof v) && uniform_num(v, &kv) && kv > 0 && Ub > 0) I = 100 * sqrt(2 * kv / 3) / Ub;
            if (I > 0 && I < 100) { bd->turbSpec = TI_LENGTH; bd->intensity = I; }
            double L = 0, ev = 0, ov = 0;
            if (EP) {
                FDEntry *ee = patch_entry(EP, bd->name); char et2[64] = ""; entry_get(EP, ee, "type", et2, sizeof et2);
                if (!strcmp(et2, "turbulentMixingLengthDissipationRateInlet") && entry_get(EP, ee, "mixingLength", v, sizeof v)) L = atof(v);
                else if (kv > 0 && entry_get(EP, ee, "value", v, sizeof v) && uniform_num(v, &ev) && ev > 0) L = 0.1643 * pow(kv, 1.5) / ev;
            }
            if (!(L > 0) && OM) {
                FDEntry *oe = patch_entry(OM, bd->name); char ot[64] = ""; entry_get(OM, oe, "type", ot, sizeof ot);
                if (!strcmp(ot, "turbulentMixingLengthFrequencyInlet") && entry_get(OM, oe, "mixingLength", v, sizeof v)) L = atof(v);
                else if (kv > 0 && entry_get(OM, oe, "value", v, sizeof v) && uniform_num(v, &ov) && ov > 0) L = sqrt(kv) / (0.5477 * ov);
            }
            if (L > 0) bd->length = L;
            if (I > 0 && I < 100) { s->intensity = I; if (L > 0) s->length = L; }
        }
    }
    fd_free(U); fd_free(Pf); fd_free(K); fd_free(EP); fd_free(OM);
    note_add(notes, cap, "The case's own fvSchemes and fvSolution are kept as written (Numerics shows them as Custom)");
    return 1;
}

/* sim_adopt_bases: in simsetup.c, beside the writer whose base-only mode it uses */

