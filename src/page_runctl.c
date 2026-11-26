/* page_runctl.c -- the Run page's run controls: the time step rows for what the solver can do (adaptive or fixed, and
 * the limits it reads), and folded groups for writing, start and stop, the solver's own loop (PISO / PIMPLE / SIMPLE /
 * potentialFlow / stressAnalysis), residual control and relaxation, and the linear solvers (runctl.h). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "foamdict.h"
#include "runctl.h"
#include "page_runctl.h"

#define CTL_W 120.0f

/* a turbulence model is on (not laminar, and the case has one) */
static bool has_turb(Case *c)
{
    FDFile *t = case_dict(c, case_rpath(c, "constant/turbulenceProperties"));
    if (!t) t = case_dict(c, case_rpath(c, "constant/momentumTransport"));
    char v[64] = "", w[32] = "";
    if (!t || !fd_get(t, "simulationType", v, sizeof v)) return false;
    fd_word(v, w, sizeof w);
    return strcmp(w, "laminar") != 0;
}

static bool on_word(const char *v) { char w[16]; fd_word(v, w, sizeof w); return !strcmp(w, "yes") || !strcmp(w, "on") || !strcmp(w, "true") || !strcmp(w, "1"); }

static float label(const char *text, const char *tip, float x, float y, float lw)
{
    ui_text_clip(F_REG, 13, text, x, y + 9, lw, TH.text2);
    if (tip) ui_tooltip((Rectangle){ x, y, lw, 34 }, tip);
    return 0;
}

static float row_num(FDFile *f, const char *path, const char *text, const char *ph, const char *tip, float x, float y, float w)
{
    label(text, tip, x, y, w - CTL_W - 12);
    dv_number(f, path, (Rectangle){ x + w - CTL_W, y, CTL_W, 34 }, ph);
    return 42;
}

/* a yes/no keyword: when the file doesn't set it, the switch shows OpenFOAM's default (dflt "yes" or "no") */
static float row_switch(FDFile *f, const char *path, const char *text, const char *dflt, const char *tip, float x, float y, float w)
{
    label(text, tip, x, y, w - CTL_W - 12);
    char v[64] = "";
    bool has = fd_get(f, path, v, sizeof v);
    bool on = has ? on_word(v) : on_word(dflt);
    char id[300]; snprintf(id, sizeof id, "rc-sw:%s", path);
    if (ui_switch(ui_id(id), x + w - CTL_W, y + 4, &on)) {
        char w0[16] = ""; if (has) fd_word(v, w0, sizeof w0);
        const char *nv = !strcmp(w0, "on") || !strcmp(w0, "off") ? (on ? "on" : "off")
                       : !strcmp(w0, "true") || !strcmp(w0, "false") ? (on ? "true" : "false") : (on ? "yes" : "no");
        fd_put(f, path, nv);
    }
    char shown[80]; snprintf(shown, sizeof shown, has ? "%s" : "default", v);
    ui_text_clip(F_MONO, 12.5f, shown, x + w - CTL_W + 44, y + 6, CTL_W - 44, TH.text3);
    return 38;
}

static float row_choice(FDFile *f, const char *path, const char *key, const char *text, const char *ph, const char *tip, float x, float y, float w)
{
    const char *const *items; int n = suggest_for_key(key, NULL, &items);
    float cw = fminf(170, w * 0.5f);
    label(text, tip, x, y, w - cw - 12);
    dv_choice(f, path, (Rectangle){ x + w - cw, y, cw, 34 }, items, n, true, ph);
    return 42;
}

static float hint(const char *t, float x, float y, float w)
{
    return ui_text_wrap(F_REG, 12.5f, t, (Rectangle){ x, y, w, 200 }, 17, TH.text3, true) + 8;
}

/* ============================================================ time step: adaptive, or why not */
static double armConvert = -1e9;          /* (0 would read as armed in the first 4 s after start) */

float runctl_time_rows(Case *c, FDFile *cd, float x, float y, float w, bool locked, double coPeak, double dtSeen)
{
    float y0 = y;
    char app[64] = ""; fd_get(cd, "application", app, sizeof app);
    const RcSolver *s = rc_solver(app);
    if (s->flags & RC_STEADY) {
        y += hint("Steady: each \"time step\" is one iteration, so there is no step to adapt. Iterations stop at End time, "
                  "or earlier when Residual control (below) is met.", x, y, w);
        return y - y0;
    }
    char adj[32] = ""; bool has = fd_get(cd, "adjustTimeStep", adj, sizeof adj), on = has && on_word(adj);
    if (s->flags & RC_ADJUST) {
        ui_text(F_MED, 13, "Adaptive time step", x, y + 6, TH.text);
        ui_tooltip((Rectangle){ x, y, w - CTL_W, 30 }, "adjustTimeStep: the solver stretches or shrinks the step every time step to keep "
                                                     "the Courant number at Max Courant. Time step above is then only the first step.");
        bool v = on;
        ui_disable(locked);
        if (ui_switch(ui_id("rc-adjust"), x + w - CTL_W, y + 4, &v)) {
            fd_put(cd, "adjustTimeStep", v ? "yes" : "no");
            if (v) {
                if (!fd_find(cd, "maxCo")) fd_put(cd, "maxCo", "1");
                char m[200];
                if (rc_write_by_time(cd, m, sizeof m)) ui_toast(TOAST_INFO, "Adaptive step on: %s, so the writes stay evenly spaced in time", m);
            }
            case_rescan(c);
        }
        ui_text(F_MONO, 12.5f, has ? adj : "not set (no)", x + w - CTL_W + 44, y + 6, TH.text3);
        y += 38;
        if (on) {
            float ix = x + 16, iw = w - 16;
            y += row_num(cd, "maxCo", "Max Courant", "1", "maxCo: the largest Courant number the step may reach. 1 is the usual limit; "
                         "0.5 for sharper transients, up to 5-10 with several PIMPLE outer correctors.", ix, y, iw);
            y += row_num(cd, "maxDeltaT", "Max time step", "no limit", "maxDeltaT (s): the step never grows past this, whatever the Courant "
                         "number. Keep it at or under the write interval so slow flows still write on time.", ix, y, iw);
            if (s->flags & RC_ALPHACO)
                y += row_num(cd, "maxAlphaCo", "Max interface Courant", "1", "maxAlphaCo: the Courant limit at the free surface (cells where "
                             "alpha is between 0 and 1). The step takes the smaller of this and Max Courant.", ix, y, iw);
            if (s->flags & RC_DI)
                y += row_num(cd, "maxDi", "Max diffusion number", "10", "maxDi: the limit for heat diffusing through the solid regions "
                             "(alpha Δt / Δx²). Lower it if the solid temperatures oscillate.", ix, y, iw);
            if (s->flags & RC_ACOUSTIC)
                y += row_num(cd, "maxAcousticCo", "Max acoustic Courant", "50", "maxAcousticCo: the Courant number based on the speed of sound "
                             "in the cavitating mixture.", ix, y, iw);
            y += 4;
        }
        ui_disable(locked);
        return y - y0;
    }
    /* a solver with a fixed step: say so, and what to do instead */
    ui_text(F_MED, 13, "Adaptive time step", x, y + 6, TH.text3);
    bool off = false;
    ui_disable(true);
    ui_switch(ui_id("rc-adjust-off"), x + w - CTL_W, y + 4, &off);
    ui_disable(locked);
    ui_text(F_MONO, 12.5f, "fixed", x + w - CTL_W + 44, y + 6, TH.text3);
    y += 38;
    char m[600];
    if (s->upgrade)
        snprintf(m, sizeof m, "%s always takes the same time step: it never reads adjustTimeStep%s. %s solves the same flow and can stretch "
                 "or shrink the step to hold a Courant limit.", app, on ? " (this controlDict sets it, but it is ignored)" : "", s->upgrade);
    else if (!strcmp(app, "sonicFoam") || !strcmp(app, "sonicLiquidFoam"))
        snprintf(m, sizeof m, "%s always takes the same time step: it never reads adjustTimeStep. rhoPimpleFoam (transonic yes) solves "
                 "compressible flow with an adjustable step, but it needs its own thermophysical set-up.", app);
    else
        snprintf(m, sizeof m, "%s takes a fixed time step: it never reads adjustTimeStep%s. Keep the Courant number under 1 with Time step.",
                 app[0] ? app : "This solver", on ? " (this controlDict sets it, but it is ignored)" : "");
    float ch = callout_height(w, NULL, m);
    callout((Rectangle){ x, y, w, ch }, TOAST_INFO, ICON_INFO, NULL, m);
    y += ch + 10;
    if (s->upgrade) {
        bool armed = GetTime() - armConvert < 4;
        char lab[96]; snprintf(lab, sizeof lab, armed ? "Click again to switch to %s" : "Switch to %s (adaptive)", s->upgrade);
        float bw = fminf(ui_button_width(ICON_ZAP, lab), w);
        if (ui_button(ui_id("rc-to-pimple"), (Rectangle){ x, y, bw, 32 }, ICON_ZAP, lab, armed ? BTN_PRIMARY : BTN_SECONDARY)) {
            if (!armed) armConvert = GetTime();
            else {
                armConvert = 0;
                char msg[600];
                int n = rc_to_pimple(c, 1, msg, sizeof msg);
                if (n < 0) ui_toast(TOAST_ERR, "Not switched: %s", msg);
                else {
                    int saved = case_save_all(c);
                    case_rescan(c);
                    if (saved < 0) ui_toast(TOAST_WARN, "Switched to pimpleFoam (%s), but a file didn't save: press Save", msg);
                    else ui_toast(TOAST_OK, "Now pimpleFoam with an adaptive step, max Courant 1: %s", msg);
                }
            }
        }
        if (armed) ui_request_frames(2);
        ui_tooltip((Rectangle){ x, y, bw, 32 }, "Changes application to pimpleFoam, PISO to PIMPLE (1 outer corrector: the same algorithm), adds "
                                               "the Final solver entries, the laminar model and adjustTimeStep yes with maxCo 1. Undo puts each file back.");
        y += 40;
    }
    /* a fixed step chosen from the Courant number the last run reached */
    char dts[64] = ""; fd_get_resolved(cd, "deltaT", dts, sizeof dts);
    double dt = atof(dts), seen = dtSeen > 0 ? dtSeen : dt;
    double want = rc_dt_for_courant(seen, coPeak, 1.0);
    if (want > 0) {
        char t[200];
        snprintf(t, sizeof t, "The last run peaked at Courant %.3g with a step of %g s.", coPeak, seen);
        y += hint(t, x, y, w);
        char lab[96]; snprintf(lab, sizeof lab, "Set Time step %g (Courant ≈ 1)", want);
        float bw = fminf(ui_button_width(ICON_GAUGE, lab), w);
        ui_disable(locked || fabs(want - dt) < 1e-15);
        if (ui_button(ui_id("rc-dt-co"), (Rectangle){ x, y, bw, 32 }, ICON_GAUGE, lab, BTN_GHOST)) {
            char v[64]; snprintf(v, sizeof v, "%g", want);
            if (app_put_value(cd, "deltaT", v)) { case_rescan(c); ui_toast(TOAST_OK, "deltaT %s: press Save, then Run", v); }
        }
        ui_disable(locked);
        y += 40;
    }
    return y - y0;
}

/* ============================================================ the folded groups */
enum { G_WRITE, G_START, G_LOOP, G_CONV, G_SOLVERS, G_N };
static bool gOpen[G_N];

static float group_head(int g, const char *title, const char *summary, float x, float y, float w)
{
    Rectangle r = { x, y, w, 36 };
    if (ui_row(ui_idn(ui_id("rc-g"), g), r, false)) gOpen[g] = !gOpen[g];
    ui_icon(gOpen[g] ? ICON_CHEVRON_DOWN : ICON_CHEVRON_RIGHT, x + 12, y + 18, 15, TH.text2);
    float tw = ui_text_w(F_SEMI, 14, title);
    ui_text(F_SEMI, 14, title, x + 28, y + 9, TH.text);
    if (summary && *summary) {
        float sx = x + 28 + tw + 12;
        ui_text_clip(F_MONO, 12, summary, sx, y + 11, x + w - sx - 8, TH.text3);
    }
    return 40;
}

static float write_group(FDFile *cd, float x, float y, float w)
{
    float y0 = y;
    y += row_num(cd, "purgeWrite", "Keep the last", "0 (all)", "purgeWrite: how many written times to keep; older ones are deleted "
                 "as the run goes. 0 keeps every one.", x, y, w);
    y += row_choice(cd, "writeFormat", "writeFormat", "Format", "ascii", "writeFormat: ascii can be read in any editor; binary is smaller "
                    "and faster to write, exact to the last bit.", x, y, w);
    y += row_num(cd, "writePrecision", "Digits written", "6", "writePrecision: significant figures in ascii files.", x, y, w);
    y += row_choice(cd, "writeCompression", "writeCompression", "Compression", "off", "writeCompression: gzip each written file.", x, y, w);
    y += row_choice(cd, "timeFormat", "timeFormat", "Time folder names", "general", "timeFormat: how the time folders are named.", x, y, w);
    y += row_num(cd, "timePrecision", "Time name digits", "6", "timePrecision: digits in the time folder names. Raise it when a small "
                 "step makes two writes share a name.", x, y, w);
    return y - y0;
}

static float start_group(FDFile *cd, float x, float y, float w)
{
    float y0 = y;
    y += row_choice(cd, "startFrom", "startFrom", "Start from", "startTime", "startFrom: startTime (the time below), firstTime (the "
                    "earliest time folder) or latestTime (carry on from the last result).", x, y, w);
    y += row_num(cd, "startTime", "Start time", "0", "startTime: used when Start from is startTime.", x, y, w);
    y += row_choice(cd, "stopAt", "stopAt", "Stop at", "endTime", "stopAt: endTime runs to End time; writeNow stops at the next step "
                    "and writes; noWriteNow stops without writing; nextWrite runs to the next write. Set while running to stop cleanly.", x, y, w);
    y += row_switch(cd, "runTimeModifiable", "Read edits while running", "yes", "runTimeModifiable: the solver rereads its dictionaries "
                    "every step, so a change saved here takes effect without a restart.", x, y, w);
    return y - y0;
}

static float loop_group(Case *c, const RcSolver *s, const char *alg, FDFile *fs, float x, float y, float w)
{
    float y0 = y;
    if (!fs) return hint("The case has no system/fvSolution.", x, y, w);
    char p[160];
#define P(k) (snprintf(p, sizeof p, "%s/%s", alg, k), p)
    if (!strcmp(alg, "PIMPLE")) {
        y += row_num(fs, P("nOuterCorrectors"), "Outer correctors", "1", "nOuterCorrectors: 1 is the PISO algorithm (keep Courant under 1). "
                     "More loops each step converge the step itself, so the step may go well past Courant 1 (with relaxation).", x, y, w);
        y += row_num(fs, P("nCorrectors"), "Pressure correctors", "1", "nCorrectors: pressure solves per outer loop. 2-3 for an "
                     "accurate transient at Courant ~1.", x, y, w);
    } else if (!strcmp(alg, "PISO")) {
        y += row_num(fs, P("nCorrectors"), "Pressure correctors", "1", "nCorrectors: pressure solves per step. 2 is usual; more for "
                     "accuracy at a larger step.", x, y, w);
    } else if (!strcmp(alg, "stressAnalysis")) {
        y += row_num(fs, P("nCorrectors"), "Correctors", "1", "nCorrectors: displacement iterations per step.", x, y, w);
        y += row_num(fs, P("D"), "Converged at", "1e-6", "D: stop iterating a step when the displacement residual is under this.", x, y, w);
        y += row_switch(fs, P("compactNormalStress"), "Compact normal stress", "yes", "compactNormalStress: the more stable discretisation "
                        "of the normal stresses.", x, y, w);
    }
    if (strcmp(alg, "stressAnalysis"))
        y += row_num(fs, P("nNonOrthogonalCorrectors"), "Non-orthogonal correctors", "0", "nNonOrthogonalCorrectors: extra pressure solves "
                     "for a skewed mesh. 0 on a hex mesh; 1-2 when checkMesh reports non-orthogonality over ~60°.", x, y, w);
    if (!strcmp(alg, "SIMPLE"))
        y += row_switch(fs, P("consistent"), "SIMPLEC", "no", "consistent: the SIMPLEC variant, which converges with lighter relaxation "
                        "(p 1, U 0.9).", x, y, w);
    if ((s->flags & RC_MOMPRED) && strcmp(alg, "potentialFlow") && strcmp(alg, "stressAnalysis"))
        y += row_switch(fs, P("momentumPredictor"), "Momentum predictor", "yes", "momentumPredictor: solve momentum before pressure. On for "
                        "most flows; off can help low-speed buoyant or multiphase flows.", x, y, w);
    if (!strcmp(alg, "PIMPLE") && has_turb(c))
        y += row_switch(fs, P("turbOnFinalIterOnly"), "Turbulence on last loop only", "yes", "turbOnFinalIterOnly: solve the turbulence "
                        "model only on the last outer loop (cheaper).", x, y, w);
    if (s->flags & RC_TRANSONIC)
        y += row_switch(fs, P("transonic"), "Transonic", "no", "transonic: treats pressure as a convected quantity; on for flows near or above "
                        "Mach 1.", x, y, w);
    if (s->flags & RC_PREF) {
        y += row_num(fs, P("pRefCell"), "Pressure reference cell", "0", "pRefCell: with no fixed-pressure boundary, the cell whose pressure "
                     "is held at the reference value.", x, y, w);
        y += row_num(fs, P("pRefValue"), "Pressure reference value", "0", "pRefValue: the pressure held in that cell.", x, y, w);
    }
    if (s->flags & RC_VOF) {                         /* the alpha solver entry: "alpha.water.*" or similar */
        FDEntry *sol = fd_find(fs, "solvers");
        char ak[200] = "";
        for (int i = 0; sol && i < sol->nChild; i++) {
            const char *k = sol->child[i].key;
            if (k && sol->child[i].kind == FD_DICT && (!strncmp(k, "alpha", 5) || !strncmp(k, "\"alpha", 6))) { snprintf(ak, sizeof ak, "%s", k); break; }
        }
        if (ak[0]) {
            ui_text(F_MED, 12.5f, "Free surface", x, y + 4, TH.text);
            ui_text_clip(F_MONO, 12, ak, x + 110, y + 5, w - 110, TH.text3);
            y += 26;
#define A(k) (snprintf(p, sizeof p, "solvers/%s/%s", ak, k), p)
            y += row_num(fs, A("nAlphaCorr"), "Alpha correctors", "1", "nAlphaCorr: alpha solves per step.", x, y, w);
            y += row_num(fs, A("nAlphaSubCycles"), "Alpha sub-cycles", "1", "nAlphaSubCycles: splits each step into this many alpha steps "
                         "(a cheaper way to a sharp interface than a smaller step).", x, y, w);
            y += row_num(fs, A("cAlpha"), "Interface compression", "1", "cAlpha: 0 none, 1 the usual, up to ~1.5 sharper.", x, y, w);
            y += row_switch(fs, A("MULESCorr"), "Semi-implicit MULES", "no", "MULESCorr: semi-implicit alpha; with it the step can go to an "
                            "interface Courant of several.", x, y, w);
            y += row_num(fs, A("nLimiterIter"), "Limiter iterations", "3", "nLimiterIter: MULES limiter passes; 3 + Courant is usual.", x, y, w);
#undef A
        }
    }
#undef P
    return y - y0;
}

/* residual control and relaxation, as the case has them (plus a start when SIMPLE has none) */
static float values_of(FDFile *f, const char *path, const char *prefix, float x, float y, float w)
{
    float y0 = y;
    FDEntry *d = fd_find(f, path);
    if (!d || d->kind != FD_DICT) return 0;
    int n = d->nChild;
    for (int i = 0; i < n && i < 40; i++) {
        d = fd_find(f, path); if (!d || i >= d->nChild) break;      /* an edit re-parses: find it again */
        FDEntry *e = &d->child[i];
        if (e->kind == FD_DIRECTIVE || !e->key) continue;
        char key[200]; snprintf(key, sizeof key, "%s", e->key);
        char p[400]; snprintf(p, sizeof p, "%s/%s", path, key);
        char lab[260]; snprintf(lab, sizeof lab, "%s%s", prefix, key);
        if (e->kind == FD_VALUE) y += row_num(f, p, lab, "", NULL, x, y, w);
        else {                                                    /* outerCorrectorResidualControl: p { tolerance; relTol; } */
            char p2[440];
            snprintf(p2, sizeof p2, "%s/tolerance", p); snprintf(lab, sizeof lab, "%s%s tolerance", prefix, key);
            y += row_num(f, p2, lab, "", NULL, x, y, w);
            snprintf(p2, sizeof p2, "%s/relTol", p); snprintf(lab, sizeof lab, "%s%s relTol", prefix, key);
            y += row_num(f, p2, lab, "", NULL, x, y, w);
        }
    }
    return y - y0;
}

static float conv_group(Case *c, const char *alg, FDFile *fs, float x, float y, float w, bool locked)
{
    float y0 = y;
    char p[160];
    bool steady = !strcmp(alg, "SIMPLE");
    snprintf(p, sizeof p, "%s/residualControl", alg);
    bool hasRC = fd_find(fs, p) != NULL;
    char p2[160]; snprintf(p2, sizeof p2, "%s/outerCorrectorResidualControl", alg);
    bool hasOC = fd_find(fs, p2) != NULL;
    if (hasRC || hasOC) {
        ui_text(F_MED, 12.5f, steady ? "Stop when every residual is under" : "Outer loops stop when residuals are under", x, y + 4, TH.text);
        y += 26;
        if (hasRC) y += values_of(fs, p, "", x, y, w);
        if (hasOC) y += values_of(fs, p2, "", x, y, w);
    } else if (steady) {
        y += hint("No residual control: the run goes to End time even when it has converged.", x, y, w);
        float bw = ui_button_width(NULL, "Stop when converged (1e-4)");
        ui_disable(locked);
        if (ui_button(ui_id("rc-add-resid"), (Rectangle){ x, y, bw, 30 }, NULL, "Stop when converged (1e-4)", BTN_SECONDARY)) {
            char q[200];
            snprintf(q, sizeof q, "%s/p", p); fd_put(fs, q, "1e-4");
            snprintf(q, sizeof q, "%s/U", p); fd_put(fs, q, "1e-4");
            if (has_turb(c)) { snprintf(q, sizeof q, "%s/\"(k|epsilon|omega|nuTilda|h|e)\"", p); fd_put(fs, q, "1e-4"); }
        }
        y += 40;
    }
    if (fd_find(fs, "relaxationFactors")) {
        ui_text(F_MED, 12.5f, "Relaxation factors", x, y + 4, TH.text);
        ui_tooltip((Rectangle){ x, y, w, 24 }, "How much of each new solution is taken per iteration (1 = all). Lower values are steadier and slower.");
        y += 26;
        y += values_of(fs, "relaxationFactors/fields", "field ", x, y, w);
        y += values_of(fs, "relaxationFactors/equations", "equation ", x, y, w);
        /* old style: relaxationFactors { p 0.3; U 0.7; } */
        FDEntry *rf = fd_find(fs, "relaxationFactors");
        bool flat = false;
        for (int i = 0; rf && i < rf->nChild; i++) if (rf->child[i].kind == FD_VALUE) flat = true;
        if (flat) {
            int n = rf->nChild;
            for (int i = 0; i < n; i++) {
                rf = fd_find(fs, "relaxationFactors"); if (!rf || i >= rf->nChild) break;
                if (rf->child[i].kind != FD_VALUE) continue;
                char k[200]; snprintf(k, sizeof k, "%s", rf->child[i].key);
                char q[260]; snprintf(q, sizeof q, "relaxationFactors/%s", k);
                y += row_num(fs, q, k, "", NULL, x, y, w);
            }
        }
    } else if (steady) {
        y += hint("No relaxation factors: SIMPLE needs them (p 0.3, U 0.7 is the usual start).", x, y, w);
    }
    return y - y0;
}

static float solvers_group(FDFile *fs, float x, float y, float w)
{
    float y0 = y;
    FDEntry *sol = fd_find(fs, "solvers");
    if (!sol || sol->kind != FD_DICT) return hint("fvSolution has no solvers dictionary.", x, y, w);
    const char *const *items; int ni = suggest_for_key("solver", "solvers", &items);
    int n = sol->nChild;
    float hw = (w - 12) / 2;
    for (int i = 0; i < n && i < 40; i++) {
        sol = fd_find(fs, "solvers"); if (!sol || i >= sol->nChild) break;
        FDEntry *e = &sol->child[i];
        if (e->kind != FD_DICT || !e->key) continue;
        char key[200]; snprintf(key, sizeof key, "%s", e->key);
        bool inherits = memchr(fs->text + e->valStart, '$', (size_t)(e->valEnd - e->valStart)) != NULL;
        char p[300];
        float cw = fminf(170, w * 0.5f);
        ui_text_clip(F_MONO_MED, 13, key, x, y + 9, w - cw - 12, TH.text);
        snprintf(p, sizeof p, "solvers/%s/solver", key);
        dv_choice(fs, p, (Rectangle){ x + w - cw, y, cw, 34 }, items, ni, true, inherits ? "inherited" : "solver");
        y += 40;
        ui_text(F_REG, 12, "tolerance", x, y, TH.text3);
        ui_text(F_REG, 12, "relTol", x + hw + 12, y, TH.text3);
        y += 18;
        snprintf(p, sizeof p, "solvers/%s/tolerance", key);
        dv_number(fs, p, (Rectangle){ x, y, hw, 32 }, inherits ? "inherited" : "1e-6");
        snprintf(p, sizeof p, "solvers/%s/relTol", key);
        dv_number(fs, p, (Rectangle){ x + hw + 12, y, hw, 32 }, inherits ? "inherited" : "0");
        y += 44;
    }
    y += hint("tolerance: solve until the residual is under it. relTol: or until it has fallen by this fraction (0 = always to "
              "tolerance; the Final entries use 0).", x, y, w);
    return y - y0;
}

float runctl_panel(Case *c, float x, float y, float w, bool locked)
{
    float y0 = y;
    FDFile *cd = case_dict(c, "system/controlDict"), *fs = case_dict(c, "system/fvSolution");
    if (!cd) return 0;
    char app[64] = ""; fd_get(cd, "application", app, sizeof app);
    const RcSolver *s = rc_solver(app);
    const char *alg = rc_alg_in(fs, s);
    ui_disable(locked);
    char sum[160];
    {   char a[32] = "", b[32] = "", f[32] = "";
        fd_get(cd, "purgeWrite", a, sizeof a); fd_get(cd, "writeFormat", f, sizeof f); fd_get(cd, "writePrecision", b, sizeof b);
        snprintf(sum, sizeof sum, "%s%s%s", f[0] ? f : "ascii", atoi(a) > 0 ? ", keep " : "", atoi(a) > 0 ? a : "");
        y += group_head(G_WRITE, "Writing", sum, x, y, w);
        if (gOpen[G_WRITE]) y += write_group(cd, x + 16, y, w - 16) + 6;
    }
    {   char a[32] = "", b[32] = ""; fd_get(cd, "startFrom", a, sizeof a); fd_get(cd, "stopAt", b, sizeof b);
        snprintf(sum, sizeof sum, "%s, %s", a[0] ? a : "startTime", b[0] ? b : "endTime");
        y += group_head(G_START, "Start and stop", sum, x, y, w);
        if (gOpen[G_START]) y += start_group(cd, x + 16, y, w - 16) + 6;
    }
    if (alg[0] || !strcmp(app, "rhoCentralFoam")) {
        char t[64]; snprintf(t, sizeof t, "%s loop", alg[0] ? alg : "Flux scheme");
        if (!strcmp(alg, "potentialFlow")) snprintf(t, sizeof t, "potentialFlow");
        else if (!strcmp(alg, "stressAnalysis")) snprintf(t, sizeof t, "Stress analysis");
        char q[160], v1[16] = "", v2[16] = "";
        if (!strcmp(alg, "PIMPLE")) { snprintf(q, sizeof q, "%s/nOuterCorrectors", alg); fd_get(fs, q, v1, sizeof v1);
                                      snprintf(q, sizeof q, "%s/nCorrectors", alg); fd_get(fs, q, v2, sizeof v2);
                                      snprintf(sum, sizeof sum, "%s outer x %s p", v1[0] ? v1 : "1", v2[0] ? v2 : "1"); }
        else if (!strcmp(alg, "PISO")) { snprintf(q, sizeof q, "%s/nCorrectors", alg); fd_get(fs, q, v2, sizeof v2);
                                         snprintf(sum, sizeof sum, "%s p correctors", v2[0] ? v2 : "1"); }
        else if (!strcmp(alg, "SIMPLE")) { snprintf(q, sizeof q, "%s/consistent", alg); fd_get(fs, q, v1, sizeof v1);
                                           snprintf(sum, sizeof sum, "%s", on_word(v1) ? "SIMPLEC" : "SIMPLE"); }
        else sum[0] = 0;
        y += group_head(G_LOOP, t, sum, x, y, w);
        if (gOpen[G_LOOP]) {
            float ix = x + 16, iw = w - 16;
            if (!strcmp(app, "rhoCentralFoam")) {
                FDFile *sc = case_dict(c, "system/fvSchemes");
                static const char *const FLUX[] = { "Kurganov", "Tadmor" };
                if (sc) {
                    float cw = fminf(170, iw * 0.5f);
                    label("Flux scheme", "fluxScheme: Kurganov (less diffusive, the usual) or Tadmor.", ix, y, iw - cw - 12);
                    dv_choice(sc, "fluxScheme", (Rectangle){ ix + iw - cw, y, cw, 34 }, FLUX, 2, false, "Kurganov");
                    y += 42;
                }
            } else y += loop_group(c, s, alg, fs, ix, y, iw);
            y += 6;
        }
        if (fs && (!strcmp(alg, "SIMPLE") || !strcmp(alg, "PIMPLE"))) {
            char q[160]; snprintf(q, sizeof q, "%s/residualControl", alg);
            snprintf(sum, sizeof sum, "%s%s", fd_find(fs, q) ? "residual control" : !strcmp(alg, "SIMPLE") ? "runs to End time" : "",
                     fd_find(fs, "relaxationFactors") ? (fd_find(fs, q) || !strcmp(alg, "SIMPLE") ? ", relaxed" : "relaxed") : "");
            y += group_head(G_CONV, "Convergence", sum, x, y, w);
            if (gOpen[G_CONV]) y += conv_group(c, alg, fs, x + 16, y, w - 16, locked) + 6;
        }
    }
    if (fs) {
        FDEntry *sol = fd_find(fs, "solvers");
        int n = 0; for (int i = 0; sol && i < sol->nChild; i++) if (sol->child[i].kind == FD_DICT) n++;
        snprintf(sum, sizeof sum, "%d field%s", n, n == 1 ? "" : "s");
        y += group_head(G_SOLVERS, "Linear solvers", sum, x, y, w);
        if (gOpen[G_SOLVERS]) y += solvers_group(fs, x + 16, y, w - 16) + 6;
    }
    ui_disable(locked);
    return y - y0;
}
