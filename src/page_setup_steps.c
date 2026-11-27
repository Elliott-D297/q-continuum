#define _GNU_SOURCE
/* page_setup_steps.c -- Physics > Setup's steps, Models to Check (split from page_setup.c, improvement plan A5,
 * 2026-11-26): each step's rows. page_setup.c has the page around them, writing, adoption and the step list;
 * page_setup_int.h is what the two share. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include "app.h"
#include <ctype.h>
#include "safety.h"
#include "simsetup.h"
#include "foamdict.h"
#include "precheck.h"
#include "page_setup_int.h"

/* ---------------------------------------------------------------------------- 1 Models */
/* the water at the start of a new free-surface setup: everything below a third of the mesh's height */
void su_water_default(Case *c)
{
    SimSetup *s = &SU.s;
    double lo[3], hi[3];
    if (!mesh_bounds(c->s.path, lo, hi)) return;
    /* gravity along a 2-D mesh's depth (the default -z on an x-y mesh, with no STL mesher to say which way is up) would
       pull the water through the empty sides: down y instead (down z for a mesh thin along x or y) */
    double gtmp[3];
    int up = sim_up_axis(s), thin = 0;
    for (int k = 1; k < 3; k++) if (hi[k] - lo[k] < hi[thin] - lo[thin]) thin = k;
    double big = fmax(hi[0] - lo[0], fmax(hi[1] - lo[1], hi[2] - lo[2]));
    if (up == thin && hi[thin] - lo[thin] < 0.2 * big && !stlmesh_gravity(c, gtmp)) {
        double gm = sim_g_mag(s) > 0 ? sim_g_mag(s) : 9.81;
        int nu = thin == 2 ? 1 : 2;
        s->g[0] = s->g[1] = s->g[2] = 0; s->g[nu] = -gm;
        up = nu;
        ui_toast(TOAST_INFO, "Gravity set down %s (the mesh is flat along %s): change it in Models if that's not down", nu == 1 ? "y" : "z", thin == 0 ? "x" : thin == 1 ? "y" : "z");
    }
    if (s->nFill) return;
    s->nFill = 1;
    s->fill[0] = (SimFill){ .kind = FL_BELOW, .a = { lo[0], lo[1], lo[2] }, .b = { hi[0], hi[1], hi[2] } };
    s->fill[0].h = s->g[up] <= 0 ? lo[up] + (hi[up] - lo[up]) / 3 : hi[up] - (hi[up] - lo[up]) / 3;
    for (int k = 0; k < 3; k++) s->fill[0].b[k] = lo[k] + (hi[k] - lo[k]) / 4;   /* a box, if switched to one: a dam-break column in a corner */
    s->fill[0].b[up] = lo[up] + (hi[up] - lo[up]) / 2;
    for (int k = 0; k < 3; k++) if (k != up && hi[k] - lo[k] < 0.2 * big) s->fill[0].b[k] = hi[k];   /* a 2-D mesh's depth: all of it */
}

float models_step(Case *c, float x, float y, float w)
{
    float y0 = y;
    SimSetup *s = &SU.s;
    bool hints = w > 560;
    /* the template it started from */
    Rectangle cr = prop_row(x, &y, w, "Started from", hints ? sim_template_hint(s->tmpl) : NULL, 34);
    ui_icon(TMPL_ICON[s->tmpl >= 0 ? s->tmpl : TMPL_BLANK], cr.x + 12, cr.y + 17, 18, TH.accent);
    const char *tn = s->tmpl >= 0 && s->tmpl != TMPL_BLANK ? TMPL_NAME[s->tmpl] : "Your own choices";
    ui_text_clip(F_MED, 14, tn, cr.x + 30, cr.y + 8, cr.width - 30, TH.text);
    float tw = ui_text_w(F_MED, 14, tn);
    float bw = ui_button_width(ICON_REFRESH_CW, "Change");
    if (cr.x + 30 + tw + 16 + bw <= cr.x + cr.width && ui_button(ui_id("su-tmpl-change"), (Rectangle){ cr.x + 30 + tw + 16, cr.y, bw, 34 }, ICON_REFRESH_CW, "Change", BTN_SECONDARY))
        SU.picking = true;
    /* time */
    cr = prop_row(x, &y, w, "Time", hints ? "Steady: the flow settles to one answer. Transient: vortex shedding, start-up, anything that changes." : NULL, 34);
    int fl = s->flow;
    const char *fw[FLOW__N] = { sim_is_vof(s) ? "A free surface moves: the run is always transient" : NULL, NULL };
    if (ui_segmented_why(ui_id("su-flow"), (Rectangle){ cr.x, cr.y, fminf(260, cr.width), 34 }, (const char **)FLOW_NAME, FLOW__N, &fl, fw) && fl != s->flow) {
        s->flow = fl;
        const char *m = sim_settle(s);
        if (m) ui_toast(TOAST_INFO, "%s", m);
        su_save();
    }
    /* the approach, then the model within it */
    cr = prop_row(x, &y, w, "Turbulence", hints ? "RANS averages the turbulence out: fast, the usual choice. LES resolves the large eddies: a fine 3-D mesh and "
                                                  "small time steps. DES is RANS next to walls and LES away from them." : NULL, 34);
    const char *ap[APPR__N];
    for (int i = 0; i < APPR__N; i++) ap[i] = sim_approach_label(s, i);
    int a = sim_approach(s->turb), a0 = a;
    Rectangle ar = { cr.x, cr.y, fminf(360, cr.width), 34 };
    if (ui_segmented(ui_id("su-appr"), ar, ap, APPR__N, &a) && a != a0) {
        const char *m = sim_set_approach(s, a);
        if (m) ui_toast(TOAST_INFO, "%s", m);
        su_save();
    }
    if (s->flow == FLOW_STEADY) ui_tooltip(ar, "LES and DES need a transient run: picking one makes the run transient.");
    if (sim_approach(s->turb) != APPR_LAMINAR) {
        int ids[TURB__N], n = sim_approach_models(sim_approach(s->turb), ids), sel = 0;
        const char *names[TURB__N];
        for (int i = 0; i < n; i++) { names[i] = TURB_NAME[ids[i]]; if (ids[i] == s->turb) sel = i; }
        cr = prop_row(x, &y, w, "Model", sim_model_hint(s->turb), 34);
        if (ui_select(ui_id("su-turb"), (Rectangle){ cr.x, cr.y, fminf(260, cr.width), 34 }, names, n, &sel) && ids[sel] != s->turb) { s->turb = ids[sel]; sim_settle(s); su_save(); }
    } else cr = (Rectangle){ ar.x, ar.y, cr.width, 34 };
    double Re = su_reynolds();
    if (Re > 0) {
        float rx = sim_approach(s->turb) == APPR_LAMINAR ? ar.width + 16 : fminf(260, cr.width) + 16;
        char re[160]; pl_fmt_ui(re, sizeof re, "Re ≈ %.3g (%.3g m/s over %.3g m)%s", Re, sim_ref_speed(s), SU.size,
                                Re < 2000 && s->turb != TURB_LAMINAR ? ": laminar would do" : Re > 1e4 && s->turb == TURB_LAMINAR ? ": that is turbulent" : "");
        ui_text_clip(F_REG, 13, re, cr.x + rx, cr.y + 9, cr.width - rx, (Re < 2000) == (s->turb == TURB_LAMINAR) ? TH.text3 : TH.warn);
    }
    /* the walls (plan step 5): wall functions or resolved, and the y+ helper */
    if (s->turb != TURB_LAMINAR) {
        y += su_wall_rows(c, x, y, w, hints);
    }
    /* how a transient run steps */
    if (s->flow == FLOW_TRANSIENT) {
        static const char *const algHint[ALG__N] = {
            "pisoFoam: accurate, small fixed time steps (Courant under 1). Suits LES and DES.",
            "pimpleFoam: the usual transient choice; the time step can follow a Courant limit.",
            "pimpleFoam with many outer correctors per step: time steps 5 to 10 times larger, for slow changes." };
        cr = prop_row(x, &y, w, "Algorithm", (s->energy != EN_OFF || sim_is_vof(s)) && s->algo != ALG_PISO ? TextFormat("%s%s", sim_app(s), strchr(algHint[s->algo], ':')) : algHint[s->algo], 34);
        int al = s->algo;
        const char *aw[ALG__N];
        for (int i = 0; i < ALG__N; i++) aw[i] = i == ALG_PISO ? sim_algo_why_not(s, i) : NULL;   /* PISO: not with heat */
        if (ui_segmented_why(ui_id("su-algo"), (Rectangle){ cr.x, cr.y, fminf(420, cr.width), 34 }, (const char **)ALG_NAME, ALG__N, &al, aw) && al != s->algo) { sim_set_algo(s, al); su_save(); }
    }
    /* heat (plan step 3) */
    static const char *const enHint[EN__N] = {
        "No temperature: the flow is the same everywhere it is.",
        "The flow carries heat from warm walls and inlets (forced convection); the density stays put. buoyantSimpleFoam or buoyantPimpleFoam with gravity off.",
        "Warm fluid rises and cool sinks (natural convection): the density follows the temperature, under gravity." };
    /* compressibility (plan step 4): an ideal gas whose density follows the pressure, with the Mach number as it stands */
    cr = prop_row(x, &y, w, "Compressibility", hints ? "Incompressible: one density (or one that follows only the temperature). Ideal gas: the density follows "
                                                       "the pressure too, for gas above about Mach 0.3 (rhoSimpleFoam, rhoPimpleFoam)." : NULL, 34);
    int co = s->comp;
    float cw = fminf(320, cr.width);
    if (ui_segmented(ui_id("su-comp"), (Rectangle){ cr.x, cr.y, cw, 34 }, (const char **)COMP_NAME, COMP__N, &co) && co != s->comp) {
        int alg = s->algo;
        sim_set_comp(s, co);
        if (co == COMP_GAS && alg != s->algo) ui_toast(TOAST_INFO, "A compressible gas steps through time with rhoPimpleFoam: the algorithm is now PIMPLE");
        else ui_toast(TOAST_INFO, co == COMP_GAS ? "Ideal gas: the temperature is on, and Materials holds the gas" : "Incompressible: back to one density");
        su_save();
    }
    double Ma = sim_mach(s);
    if (Ma > 0 && cr.width - cw - 16 > 80) {
        char mt[160]; pl_fmt_ui(mt, sizeof mt, "Ma ≈ %.2g%s", Ma, Ma > 1 ? ": supersonic" : Ma > 0.8 ? ": transonic" : Ma > 0.3 && !sim_is_gas(s) ? ": compressible" : "");
        ui_text_clip(F_REG, 13, mt, cr.x + cw + 16, cr.y + 9, cr.width - cw - 16, Ma > 0.3 && !sim_is_gas(s) ? TH.warn : TH.text3);
    }
    cr = prop_row(x, &y, w, "Energy", hints ? (sim_is_gas(s) ? "A compressible gas always carries its temperature: its density follows it." : enHint[s->energy]) : NULL, 34);
    int en = s->energy;
    const char *ew[EN__N];
    for (int i = 0; i < EN__N; i++) ew[i] = sim_energy_why_not(s, i);
    if (ui_segmented_why(ui_id("su-energy"), (Rectangle){ cr.x, cr.y, fminf(420, cr.width), 34 }, (const char **)ENERGY_NAME, EN__N, &en, ew) && en != s->energy) {
        int alg = s->algo;
        sim_set_energy(s, en);
        su_gravity_auto(c);
        if (alg != s->algo) ui_toast(TOAST_INFO, "With heat on, buoyantPimpleFoam steps through time: the algorithm is now PIMPLE");
        su_save();
    }
    /* free surface (plan step 8): water and air with the surface between them */
    cr = prop_row(x, &y, w, "Multiphase", hints ? (sim_is_vof(s) ? "Water and air, the surface between them tracked as the share of water in each cell "
                                                                  "(VOF, interFoam). Always transient; no heat. Materials holds both, Starting values the water."
                                                                : "Free surface: water and air with the surface between them, a dam break, a tank, a channel.") : NULL, 34);
    {
        int ph = s->phase;
        const char *pw[PH__N]; for (int i = 0; i < PH__N; i++) pw[i] = sim_phase_why_not(s, i);
        if (ui_segmented_why(ui_id("su-multi"), (Rectangle){ cr.x, cr.y, fminf(420, cr.width), 34 }, (const char **)PHASE_NAME, PH__N, &ph, pw) && ph != s->phase) {
            sim_set_phase(s, ph);
            su_gravity_auto(c);
            if (ph == PH_VOF) su_water_default(c);
            ui_toast(TOAST_INFO, ph == PH_VOF ? "Free surface: transient, interFoam, gravity on. Starting values holds the water at the start"
                                              : "One fluid again");
            su_save();
        }
    }
    if (s->energy == EN_BUOYANT || sim_is_vof(s)) {
        double g[3];
        bool have = stlmesh_gravity(c, g);
        if (have) {                                         /* from the STL mesher's Up, or typed in */
            cr = prop_row(x, &y, w, "Gravity", hints ? "9.81 m/s² down the STL mesher's Up direction (Mesh > STL mesher, Flow), or typed in." : NULL, 34);
            static const char *GM[] = { "From the mesher's Up", "Typed in" };
            int mode = s->gAuto ? 0 : 1;
            float mw = fminf(320, cr.width);
            if (ui_segmented(ui_id("su-gmode"), (Rectangle){ cr.x, cr.y, mw, 34 }, GM, 2, &mode) && (mode == 0) != s->gAuto) { s->gAuto = mode == 0; su_gravity_auto(c); su_save(); }
            if (s->gAuto) {
                char t[96]; pl_fmt_ui(t, sizeof t, "(%.3g %.3g %.3g) m/s²", s->g[0], s->g[1], s->g[2]);
                if (cr.width - mw - 16 > 80) ui_text_clip(F_MONO, 13, t, cr.x + mw + 16, cr.y + 9, cr.width - mw - 16, TH.text3);
            } else su_row_vec(x, &y, w, hints, "su-g", "Gravity vector", NULL, s->g, "m/s²");
        } else su_row_vec(x, &y, w, hints, "su-g", "Gravity", "m/s², in the mesh's axes: (0 -9.81 0) when y is up.", s->g, "m/s²");
        /* how strong the buoyancy is: Ra (natural convection's own number), Ri (against the inflow) */
        double Ra = sim_rayleigh(s, SU.height), Ri = sim_richardson(s, SU.height);
        if (Ra > 0) {
            char t[240];
            pl_fmt_ui(t, sizeof t, "Ra ≈ %.3g over a height of %.3g m with %.3g K across: %s%s", Ra, SU.height, sim_delta_T(s),
                      Ra < 1e8 ? "laminar" : Ra < 1e9 ? "near the turn to turbulence" : "turbulent",
                      Ri > 0 ? TextFormat(". Ri ≈ %.3g: %s", Ri, Ri < 0.1 ? "the inflow carries the heat; buoyancy hardly matters" : Ri > 10 ? "buoyancy drives the flow"
                                                                                                                    : "buoyancy and the inflow both matter") : "");
            y += su_note(x, y + 4, w, t);
        }
    }
    y += 8;
    y += su_issues_callout(SS_MODELS, x, y, w);
    /* the physics still to come: there, greyed, saying so (rotating and porous zones have their own step) */
    y += 12;
    y += section_head(x, y, w, "Other physics", "Rotating and porous zones are in the Zones step. Hover an option for what it will do.");
    static const char *const EX[] = { "None", "Passive scalar" };
    static const char *const EXw[] = { NULL, "A dye or a concentration carried by the flow: a later update" };
    cr = prop_row(x, &y, w, "Extras", NULL, 34);
    int sel = 0;
    ui_segmented_why(ui_id("su-extras"), (Rectangle){ cr.x, cr.y, fminf(280, cr.width), 34 }, (const char **)EX, 2, &sel, EXw);
    return y - y0;
}

/* ---------------------------------------------------------------------------- 2 Materials */
/* a compressible gas: its preset (molar mass, gamma, Sutherland's viscosity) and the operating pressure; what follows at
   the start (density, viscosity, cp, conductivity, the speed of sound) shown as it is worked out */
static float gas_materials(float x, float y, float w)
{
    float y0 = y;
    SimSetup *s = &SU.s;
    y += su_issues_callout(SS_MATERIALS, x, y, w);
    Rectangle cr = prop_row(x, &y, w, "Gas", "An ideal gas: p = rho R T. Custom for your own molar mass, gamma and viscosity.", 34);
    int g = s->gas;
    if (ui_segmented(ui_id("su-gas"), (Rectangle){ cr.x, cr.y, fminf(400, cr.width), 34 }, (const char **)GAS_NAME, GAS__N, &g) && g != s->gas) {
        sim_gas_preset(s, g); sim_settle(s); su_save();
    }
    char a[96]; pl_fmt_ui(a, sizeof a, "%.5g atm", s->pOp / 101325);
    if (su_row_num(x, &y, w, true, "su-pop", "Operating pressure", "Absolute. Every pressure typed elsewhere is gauge, above this one: 0 at an outlet means this.",
                &s->pOp, 1, 1e9, "Pa", a)) { sim_settle(s); su_save(); }
    bool rows = s->gas == GAS_CUSTOM || SU.more[SS_MATERIALS];
    if (rows) {
        bool ch = false;
        ch |= su_row_num(x, &y, w, true, "su-molw", "Molar mass", "Air 28.96, nitrogen 28.01, CO2 44.01.", &s->molW, 1e-3, 1e4, "kg/kmol", NULL);
        ch |= su_row_num(x, &y, w, true, "su-gamma", "Ratio of specific heats", "gamma = cp / cv: 1.4 for air, 1.67 for a monatomic gas.", &s->gamma, 1.0001, 3, NULL, NULL);
        ch |= su_row_num(x, &y, w, true, "su-as", "Sutherland As", "mu = As √T / (1 + Ts / T).", &s->muAs, 1e-12, 1, "Pa·s/√K", NULL);
        ch |= su_row_num(x, &y, w, true, "su-ts", "Sutherland Ts", NULL, &s->muTs, 0, 1e5, "K", NULL);
        if (ch) { s->gas = GAS_CUSTOM; sim_settle(s); su_save(); }
    }
    char t[400];
    pl_fmt_ui(t, sizeof t, "%sAt %.6g Pa and %.4g °C: density %.4g kg/m³, viscosity %.4g Pa·s, specific heat %.4g J/(kg·K), conductivity %.3g W/(m·K), "
              "speed of sound %.4g m/s.", rows ? "" : TextFormat("Molar mass %.4g kg/kmol, gamma %.4g, Sutherland As %.4g, Ts %.4g K. ", s->molW, s->gamma, s->muAs, s->muTs),
              sim_pabs(s, s->p0), s->T0 - 273.15, s->rho, sim_gas_mu(s, s->T0), s->cp, s->kappa, sim_sound_speed(s, s->T0));
    y += su_note(x, y + 8, w, t) + 8;
    if (s->gas != GAS_CUSTOM) y += su_more_toggle(SS_MATERIALS, x, y);
    return y - y0;
}

/* a free surface: the liquid ("water", a preset or custom) and the gas ("air"), and the surface tension */
static float vof_materials(float x, float y, float w)
{
    float y0 = y;
    SimSetup *s = &SU.s;
    y += su_issues_callout(SS_MATERIALS, x, y, w);
    Rectangle cr = prop_row(x, &y, w, "Liquid", "The heavier phase, called water in the files (alpha.water is its share of each cell).", 34);
    int l = s->liquid;
    if (ui_segmented(ui_id("su-liq"), (Rectangle){ cr.x, cr.y, fminf(480, cr.width), 34 }, (const char **)LIQ_NAME, LIQ__N, &l) && l != s->liquid) {
        sim_liquid(s, l); sim_settle(s); su_save();
    }
    bool rows = s->liquid == LIQ_CUSTOM || SU.more[SS_MATERIALS];
    bool ch = false;
    if (rows) {
        ch |= su_row_num(x, &y, w, true, "su-rhol", "Liquid density", NULL, &s->rhoL, 1e-3, 1e5, "kg/m³", NULL);
        ch |= su_row_num(x, &y, w, true, "su-nul", "Liquid viscosity", "Kinematic, nu = mu / rho.", &s->nuL, 1e-12, 1e3, "m²/s", NULL);
        if (ch) { s->liquid = LIQ_CUSTOM; ch = false; }
    }
    if (rows || SU.more[SS_MATERIALS]) {
        ch |= su_row_num(x, &y, w, true, "su-rhog", "Gas density", "Air: 1.2 kg/m³ (the tutorials take 1).", &s->rhoG, 1e-6, 1e5, "kg/m³", NULL);
        ch |= su_row_num(x, &y, w, true, "su-nug", "Gas viscosity", "Air: 1.48e-5 m²/s.", &s->nuG, 1e-12, 1e3, "m²/s", NULL);
    }
    ch |= su_row_num(x, &y, w, true, "su-sigma", "Surface tension", "Water and air 0.07 N/m. It matters for drops, films and small ripples; a dam break hardly feels it.",
                  &s->sigma, 0, 10, "N/m", NULL);
    if (ch) { sim_settle(s); su_save(); }
    if (!rows) {
        char t[300]; pl_fmt_ui(t, sizeof t, "%s: %.5g kg/m³, %.4g m²/s. Air: %.4g kg/m³, %.4g m²/s.", LIQ_NAME[s->liquid], s->rhoL, s->nuL, s->rhoG, s->nuG);
        y += su_note(x, y + 8, w, t) + 8;
    }
    if (s->liquid != LIQ_CUSTOM) y += su_more_toggle(SS_MATERIALS, x, y);
    return y - y0;
}

float materials_step(float x, float y, float w)
{
    float y0 = y;
    SimSetup *s = &SU.s;
    if (sim_is_gas(s)) return gas_materials(x, y, w);
    if (sim_is_vof(s)) return vof_materials(x, y, w);
    bool heat = s->energy != EN_OFF, buoy = s->energy == EN_BUOYANT;
    y += su_issues_callout(SS_MATERIALS, x, y, w);
    Rectangle cr = prop_row(x, &y, w, "Fluid", heat ? "Air and water at 20 °C, their thermal properties too; Custom for your own."
                                                    : "Air and water at 20 °C; Custom for your own density and viscosity.", 34);
    int f = s->fluid;
    if (ui_segmented(ui_id("su-fluid"), (Rectangle){ cr.x, cr.y, fminf(360, cr.width), 34 }, (const char **)FLUID_NAME, FLUID__N, &f) && f != s->fluid) { sim_fluid(s, f); su_save(); }
    char mu[64]; pl_fmt_ui(mu, sizeof mu, "dynamic viscosity %.4g Pa·s", s->nu * s->rho);
    double Pr = s->kappa > 0 ? s->nu * s->rho * s->cp / s->kappa : 0;
    bool rows = s->fluid == FLUID_CUSTOM || SU.more[SS_MATERIALS];
    if (!rows) {
        char t[400]; pl_fmt_ui(t, sizeof t, "Density %.4g kg/m³, kinematic viscosity %.4g m²/s (%s).", s->rho, s->nu, mu);
        if (heat) pl_fmt_ui(t + strlen(t), sizeof t - strlen(t), " Specific heat %.4g J/(kg·K), conductivity %.3g W/(m·K) (Prandtl number %.3g)%s.",
                            s->cp, s->kappa, Pr, buoy ? TextFormat(", thermal expansion %.3g 1/K%s", sim_beta(s), s->fluid == FLUID_AIR ? " (an ideal gas: 1 / T)" : "") : "");
        y += su_note(x, y + 8, w, t) + 8;
    } else {
        if (su_row_num(x, &y, w, true, "su-rho", "Density", heat ? "At the reference temperature." : "The solvers work in pressure over density; this turns pascals into that.",
                    &s->rho, 1e-6, 1e5, "kg/m³", NULL)) { s->fluid = FLUID_CUSTOM; su_save(); }
        if (su_row_num(x, &y, w, true, "su-nu", "Kinematic viscosity", "nu = mu / rho.", &s->nu, 1e-12, 1e3, "m²/s", mu)) { s->fluid = FLUID_CUSTOM; su_save(); }
        if (heat) {
            if (su_row_num(x, &y, w, true, "su-cp", "Specific heat", "cp: the heat that warms a kilogram by one kelvin.", &s->cp, 1e-3, 1e7, "J/(kg·K)", NULL)) { s->fluid = FLUID_CUSTOM; su_save(); }
            char pr[64]; pl_fmt_ui(pr, sizeof pr, "Prandtl number %.3g", Pr);
            if (su_row_num(x, &y, w, true, "su-kappa", "Thermal conductivity", "k: how well it conducts heat.", &s->kappa, 1e-6, 1e4, "W/(m·K)", pr)) { s->fluid = FLUID_CUSTOM; su_save(); }
            if (buoy && s->fluid != FLUID_AIR && su_row_num(x, &y, w, true, "su-beta", "Thermal expansion", "beta: how much the density drops per kelvin warmer (an ideal gas: 1 / T).",
                                &s->beta, 0, 1, "1/K", NULL)) { s->fluid = FLUID_CUSTOM; su_save(); }
        }
    }
    if (buoy) {
        char c[48]; pl_fmt_ui(c, sizeof c, "%.4g °C", s->Tref - 273.15);
        su_row_num(x, &y, w, true, "su-tref", "Reference temperature", "Where the density is the one above: warmer fluid rises, cooler sinks.", &s->Tref, 1, 1e4, "K", c);
    }
    if (s->fluid != FLUID_CUSTOM) y += su_more_toggle(SS_MATERIALS, x, y);
    return y - y0;
}

/* ---------------------------------------------------------------------------- 3 Boundaries */
float boundaries_step(Case *c, float x, float y, float w)
{
    float y0 = y;
    char mi[PL_PATH]; pl_join(mi, sizeof mi, c->s.path, "mesher/mesher.ini");
    if (pl_is_file(mi))
        y += su_note(x, y, w, "Rename them and set what each one is on Mesh > STL mesher, by clicking the part. The wind tunnel's own boundaries and any "
                           "other patch of the mesh are here too.");
    else
        y += su_note(x, y, w, "One per patch of the mesh: say what each one is (an inlet, an outlet, a wall ...) and give its values. They go into 0/ "
                           "when you write the case files.");
    y += su_issues_callout(SS_BOUNDARIES, x, y, w);
    if (!SU.s.nB) {
        const char *t = "No boundaries yet", *d = "Choose an STL on Mesh > STL mesher (or generate the mesh): its boundaries appear here.";
        float ch = callout_height(w, t, d);
        callout((Rectangle){ x, y, w, ch }, TOAST_INFO, ICON_LAYERS, t, d);
        return y + ch + 16 - y0;
    }
    bool meshed = c->nPatches > 0 && c->patchSource == 1;
    for (int i = 0; i < SU.s.nB; i++) {
        SimBoundary *b = &SU.s.b[i];
        ui_push_id(ui_idn(ui_id("su-b"), i));
        Rectangle card = { x, y, w, 0 };
        float cy = y + 14;
        float nw = fminf(260, w * 0.34f), sx = x + 16 + nw + 14;
        ui_text_clip(F_MONO_MED, 15, b->name, x + 16, cy + 7, nw, TH.text);
        int role = b->role;
        bool con = sim_constraint(b->meshType);
        ui_disable(con);
        if (ui_select(ui_id("role"), (Rectangle){ sx, cy, 180, 34 }, ROLE_NAME, ROLE__N, &role) && role != b->role) {
            if (b->fromStl) stlmesh_set_role(c, b->name, role);     /* the STL's type: the mesh patch follows on the next Generate */
            b->role = role; su_save();
        }
        ui_disable(false);
        char sub[200];
        bool inMesh = false;
        for (int k = 0; k < c->nPatches; k++) if (!strcmp(c->patches[k].name, b->name)) inMesh = true;
        pl_fmt_ui(sub, sizeof sub, "%s%s%s", b->fromStl ? "from the STL" : SU.external && !inMesh ? "the wind tunnel" : "a mesh patch",
                  b->area > 0 ? TextFormat("  ·  %.3g m²", b->area) : "", meshed && !inMesh ? "  ·  not in the mesh yet: generate it again" : "");
        float subx = sx + 196;
        if (x + w - 14 - subx >= 140) ui_text_clip(F_REG, 12.5f, sub, subx, cy + 9, x + w - 14 - subx, meshed && !inMesh ? TH.warn : TH.text3);
        else { cy += 40; ui_text_clip(F_REG, 12.5f, sub, x + 16, cy + 2, w - 32, meshed && !inMesh ? TH.warn : TH.text3); cy -= 14; }
        cy += 46;
        cy += setup_boundary_editor(c, b->name, x + 16, cy, w - 32);
        card.height = cy - y + 4;
        ui_rect_lines(card, 10, 1, TH.lineSoft);
        ui_pop_id();
        y += card.height + 12;
    }
    return y - y0;
}

/* the boundaries as the Boundary conditions page shows them by default (workflow plan W3): the same cards, for a case the
   setup owns */
float setup_boundaries_view(Case *c, float x, float y, float w)
{
    su_sync(c);
    return boundaries_step(c, x, y, w);
}

/* ---------------------------------------------------------------------------- 5 Reports: what is measured (plan step 6)
 * Each report added from a menu, with only its own rows: a probe's field and points, a surface's boundary and measure,
 * a pressure drop's two boundaries, a volume's measure and field. Each becomes a function object, and a Run-page monitor. */
static const char *field_label(const char *f)
{
    return !strcmp(f, "U") ? "Speed |U|" : !strcmp(f, "p") ? "Pressure" : !strcmp(f, "T") ? "Temperature" : !strcmp(f, "k") ? "Turbulent energy k" :
           !strcmp(f, "omega") ? "omega" : !strcmp(f, "epsilon") ? "epsilon" : !strcmp(f, "nuTilda") ? "nuTilda" : f;
}

/* a select of the setup's fields (r->field), and of its open boundaries (into dst); true when changed */
static bool field_select(UiId id, Rectangle r, SimReport *rp)
{
    const char *f[SIM_MAX_F], *lab[SIM_MAX_F];
    int n = sim_report_fields(&SU.s, f), sel = 0;
    for (int i = 0; i < n; i++) { lab[i] = field_label(f[i]); if (!strcmp(f[i], rp->field)) sel = i; }
    if (!ui_select(id, r, lab, n, &sel) || !strcmp(f[sel], rp->field)) return false;
    pl_strlcpy(rp->field, f[sel], sizeof rp->field);
    return true;
}

static bool patch_select(UiId id, Rectangle r, char *dst, size_t cap)
{
    const char *names[SIM_MAX_B]; int n = 0, sel = -1;
    for (int i = 0; i < SU.s.nB; i++) {
        if (sim_constraint(SU.s.b[i].meshType)) continue;
        if (!strcmp(SU.s.b[i].name, dst)) sel = n;
        names[n++] = SU.s.b[i].name;
    }
    if (!n) return false;
    int was = sel; if (sel < 0) sel = 0;
    if (!ui_select(id, r, names, n, &sel) || (sel == was && was >= 0)) return false;
    pl_strlcpy(dst, names[sel], cap);
    return true;
}

static float measurements(Case *c, float x, float y, float w)
{
    float y0 = y;
    SimSetup *s = &SU.s;
    y += section_head(x, y, w, "Measured while it runs", "Each report is worked out every iteration and shown on the Run page as it settles (Monitors), "
                                                         "where it can stop the run once it stops changing. The walls' y+ is always measured.");
    /* add one */
    static const char *ADD[RP__N + 1] = { "Add a report ...", "Probe points: values at points", "Surface: an average, a flow rate, the least or greatest on a boundary",
                                          "Pressure drop between two boundaries", "Volume: the average, least or greatest in the domain" };
    int pick = 0;
    ui_disable(s->nR >= SIM_MAX_R);
    if (ui_select(ui_id("su-rep-add"), (Rectangle){ x, y, fminf(420, w), 36 }, ADD, RP__N + 1, &pick) && pick > 0) {
        double lo[3], hi[3], mid[3] = { 0, 0, 0 };
        if (mesh_bounds(c->s.path, lo, hi)) for (int k = 0; k < 3; k++) mid[k] = 0.5 * (lo[k] + hi[k]);
        if (sim_report_add(s, pick - 1, mid) >= 0) su_save();
    }
    ui_disable(false);
    y += 50;
    int del = -1;
    for (int i = 0; i < s->nR; i++) {
        SimReport *r = &s->r[i];
        ui_push_id(ui_idn(ui_id("su-rep"), i));
        float cy = y + 12;
        char d[200]; sim_report_describe(s, r, d, sizeof d);
        ui_text_clip(F_SEMI, 14, d, x + 16, cy + 2, w - 80, TH.text);
        char sub[120]; pl_fmt_ui(sub, sizeof sub, "%s  ·  %s  ·  postProcessing/%s", REPORT_NAME[r->kind], sim_report_unit(s, r), r->name);
        ui_text_clip(F_MONO, 12, sub, x + 16, cy + 22, w - 80, TH.text3);
        if (ui_button(ui_id("del"), (Rectangle){ x + w - 46, cy, 34, 34 }, ICON_X, NULL, BTN_ICON)) del = i;
        ui_tooltip((Rectangle){ x + w - 46, cy, 34, 34 }, "Remove this report");
        cy += 50;
        float ix = x + 16, iw = w - 32;
        bool ch = false;
        Rectangle cr;
        switch (r->kind) {
        case RP_PROBE:
            cr = prop_row(ix, &cy, iw, "Field", NULL, 34);
            ch |= field_select(ui_id("field"), (Rectangle){ cr.x, cr.y, fminf(240, cr.width), 34 }, r);
            for (int k = 0; k < r->nPts; k++) {
                char id[32], lab[32]; snprintf(id, sizeof id, "pt%d", k); pl_fmt_ui(lab, sizeof lab, "Point %d", k + 1);
                ch |= su_row_vec(ix, &cy, iw, true, id, lab, k == 0 ? "In the mesh's axes, m." : NULL, r->pts[k], "m");
            }
            {
                float bw = ui_button_width(ICON_PLUS, "Add a point");
                ui_disable(r->nPts >= SIM_MAX_PROBES);
                if (ui_button(ui_id("pt-add"), (Rectangle){ ix, cy + 4, bw, 32 }, ICON_PLUS, "Add a point", BTN_GHOST)) {
                    memcpy(r->pts[r->nPts], r->pts[r->nPts - 1], sizeof r->pts[0]); r->nPts++; ch = true;
                }
                ui_disable(r->nPts <= 1);
                float rw = ui_button_width(ICON_X, "Remove the last");
                if (ui_button(ui_id("pt-del"), (Rectangle){ ix + bw + 8, cy + 4, rw, 32 }, ICON_X, "Remove the last", BTN_GHOST)) { r->nPts--; ch = true; }
                ui_disable(false);
                cy += 44;
            }
            break;
        case RP_SURFACE:
            cr = prop_row(ix, &cy, iw, "Boundary", NULL, 34);
            ch |= patch_select(ui_id("patch"), (Rectangle){ cr.x, cr.y, fminf(240, cr.width), 34 }, r->patch, sizeof r->patch);
            cr = prop_row(ix, &cy, iw, "Measure", r->op == SO_MASSAVG ? "Weighted by the flow through each face: the mixed-out value." :
                                                  r->op == SO_FLOW ? "Out of the domain positive, into it negative." : NULL, 34);
            { int op = r->op; if (ui_select(ui_id("op"), (Rectangle){ cr.x, cr.y, fminf(240, cr.width), 34 }, SURF_OP_NAME, SO__N, &op) && op != r->op) { r->op = op; ch = true; } }
            if (r->op != SO_FLOW) {
                cr = prop_row(ix, &cy, iw, "Field", NULL, 34);
                ch |= field_select(ui_id("field"), (Rectangle){ cr.x, cr.y, fminf(240, cr.width), 34 }, r);
            }
            break;
        case RP_DROP:
            cr = prop_row(ix, &cy, iw, "From", "Usually the inlet: its area-averaged pressure ...", 34);
            ch |= patch_select(ui_id("from"), (Rectangle){ cr.x, cr.y, fminf(240, cr.width), 34 }, r->patch, sizeof r->patch);
            cr = prop_row(ix, &cy, iw, "To", "... minus this one's (the outlet).", 34);
            ch |= patch_select(ui_id("to"), (Rectangle){ cr.x, cr.y, fminf(240, cr.width), 34 }, r->patch2, sizeof r->patch2);
            break;
        default:
            cr = prop_row(ix, &cy, iw, "Measure", NULL, 34);
            { int op = r->op; if (ui_select(ui_id("op"), (Rectangle){ cr.x, cr.y, fminf(240, cr.width), 34 }, VOL_OP_NAME, VO__N, &op) && op != r->op) { r->op = op; ch = true; } }
            cr = prop_row(ix, &cy, iw, "Field", NULL, 34);
            ch |= field_select(ui_id("field"), (Rectangle){ cr.x, cr.y, fminf(240, cr.width), 34 }, r);
            break;
        }
        if (ch) su_save();
        Rectangle card = { x, y, w, cy - y + 6 };
        ui_rect_lines(card, 10, 1, TH.lineSoft);
        ui_pop_id();
        y += card.height + 12;
    }
    if (del >= 0) { sim_report_remove(s, del); su_save(); }
    if (!s->nR) y += su_note(x, y, w, "None yet. \"What is my pressure drop?\" Add a Pressure drop. \"What is the speed here?\" Add Probe points there.") + 4;
    y += 12;
    return y - y0;
}

/* ---------------------------------------------------------------------------- 5 Reports: lift and drag (user request, 2026-10-26) */
float reports_step(Case *c, float x, float y, float w)
{
    float y0 = y;
    SimSetup *s = &SU.s;
    y += su_issues_callout(SS_REPORTS, x, y, w);
    y += measurements(c, x, y, w);
    if (s->energy != EN_OFF) {
        y += section_head(x, y, w, "Heat through the walls", "The heat each wall passes into the fluid (W; negative: out of it), and its least and greatest "
                                                             "flux (W/m²), at each write; for each heated or cooled wall, its mean flux, temperature and Nusselt number every "
                                                             "iteration. They are in postProcessing; the Run page shows the latest.");
        Rectangle hr = prop_row(x, &y, w, "Monitor wall heat", "Adds the wallHeatFlux function object to system/controlDict.", 24);
        if (ui_switch(ui_id("su-heat"), hr.x, hr.y + 2, &s->heatReport)) su_save();
        if (s->heatReport) {
            /* the Nusselt number of each wall that passes heat: Nu = q L / (kappa dT) */
            int nh = 0;
            for (int i = 0; i < s->nB; i++) nh += sim_wall_heated(&s->b[i]);
            Rectangle lr = prop_row(x, &y, w, "Nusselt length", "Nu = q L / (k ΔT), q the wall's mean heat flux. Usually the height of a cavity, the length of a plate "
                                                                 "or a pipe's diameter.", 34);
            int mode = s->nuAuto ? 0 : 1;
            static const char *MODE[] = { "Mesh height", "Typed in" };
            float sw = fminf(240, lr.width * 0.5f);
            if (ui_segmented(ui_id("su-nul"), (Rectangle){ lr.x, lr.y, sw, 34 }, MODE, 2, &mode) && (mode == 0) != s->nuAuto) {
                s->nuAuto = mode == 0;
                if (s->nuAuto && SU.height > 0) s->nuLength = SU.height;
                su_save();
            }
            ui_disable(s->nuAuto);
            if (su_num_in(ui_id("su-nulv"), (Rectangle){ lr.x + sw + 10, lr.y, fminf(160, lr.width - sw - 10), 34 }, &s->nuLength, "%.6g", 1e-12, 1e9, "m")) su_save();
            ui_disable(false);
            char a[120], d[48] = "";
            double span = sim_delta_T(s);
            if (span > 0) pl_fmt_ui(d, sizeof d, "%.4g K", span);
            pl_fmt_ui(a, sizeof a, s->nuDT > 0 ? "" : span > 0 ? "0: the span the boundaries set, %s" : "0: none known yet", d);
            su_row_num(x, &y, w, true, "su-nudt", "Nusselt ΔT", "Hot wall to cold wall, or wall to the incoming fluid.", &s->nuDT, 0, 1e5, "K", a);
            if (nh == 0) y += su_note(x, y + 4, w, "No wall heats or cools the fluid yet (Boundaries): there is no Nusselt number to report.") + 4;
            else if (!(sim_nu_dT(s) > 0)) y += su_note(x, y + 4, w, "Type a ΔT to get the Nusselt number: with only heat-flux walls the wall temperature isn't known "
                                                                 "before the run. The mean flux and temperature are reported anyway.") + 4;
        }
        y += 18;
    }
    y += section_head(x, y, w, "Fields written with the results", "Extra fields OpenFOAM works out at each write, next to U and p: for ParaView and "
                                  "Results > Visualization (which can also work out vorticity and Q itself).");
    {
        Rectangle cr = prop_row(x, &y, w, "Wall shear stress", "The friction the flow puts on each wall (wallShearStress): where a boundary layer separates "
                                "it falls to zero; its pull on a part is the skin-friction drag.", 24);
        if (ui_switch(ui_id("su-out-wss"), cr.x, cr.y + 2, &s->outWss)) su_save();
        cr = prop_row(x, &y, w, "Q-criterion", "Q = (|W|² − |S|²)/2: positive where rotation beats strain. An iso-surface of Q shows the vortex cores.", 24);
        if (ui_switch(ui_id("su-out-q"), cr.x, cr.y + 2, &s->outQ)) su_save();
        cr = prop_row(x, &y, w, "Vorticity", "The local spin of the flow, curl U (twice the angular velocity of a fluid particle).", 24);
        if (ui_switch(ui_id("su-out-vort"), cr.x, cr.y + 2, &s->outVort)) su_save();
        y += 14;
    }
    y += section_head(x, y, w, "Lift and drag", "The force of the flow on the part while it runs: drag along the flow, lift across it, and as "
                                                "coefficients, Cd = drag / (½ ρ U² A) and Cl the same with the lift. The Run page plots them as they settle.");
    Rectangle cr = prop_row(x, &y, w, "Monitor forces", "Adds the forces and forceCoeffs function objects to system/controlDict.", 24);
    if (ui_switch(ui_id("su-forces"), cr.x, cr.y + 2, &s->forces)) su_save();
    if (!s->forces) return y + 8 - y0;
    /* the walls they are summed over */
    float cw[SIM_MAX_B]; int nw = 0; float lineW = 0, lines = 1, room = w - fminf(300, w * 0.38f);
    for (int i = 0; i < s->nB; i++) {
        const SimBoundary *b = &s->b[i];
        if (b->role != ROLE_WALL || sim_constraint(b->meshType)) { cw[i] = 0; continue; }
        cw[i] = ui_text_w(F_REG, 13.5f, b->name) + 44;
        if (lineW + cw[i] > room && lineW > 0) { lines++; lineW = 0; }
        lineW += cw[i]; nw++;
    }
    cr = prop_row(x, &y, w, "On these walls", "The part's walls; leave out a wind tunnel's ground.", nw ? lines * 30 - 6 : 24);
    if (!nw) ui_text(F_REG, 13, "No walls: make the part's boundaries Walls first.", cr.x, cr.y + 4, TH.warn);
    float bx = cr.x, by = cr.y;
    for (int i = 0; i < s->nB; i++) {
        SimBoundary *b = &s->b[i];
        if (!cw[i]) continue;  // NOLINT(clang-analyzer-core.uninitialized.Branch): false positive, cw[i] is set for every i < nB in the loop above
        if (bx + cw[i] > cr.x + cr.width && bx > cr.x) { bx = cr.x; by += 30; }
        bool on = sim_force_on(s, b), was = on;
        ui_checkbox(ui_idn(ui_id("su-fw"), i), (Rectangle){ bx, by, cw[i] - 14, 24 }, &on, b->name);
        if (on != was) { b->force = on; su_save(); }
        bx += cw[i];
    }
    /* the reference: from the part (and the tunnel), or typed in */
    static SimForceRef auto_r; static bool twoD, have; static double at = -10; static char atCase[PL_PATH];
    if (ui_time() - at > 0.5 || strcmp(atCase, c->s.path)) {   /* the frontal area walks every triangle: twice a second */
        at = ui_time(); pl_strlcpy(atCase, c->s.path, sizeof atCase);
        have = stlmesh_force_ref(c, &auto_r, &twoD);
    }
    cr = prop_row(x, &y, w, "Reference values", have ? "From the part: its length along the flow, its frontal area (2-D: length × depth), the tunnel's flow and up."
                                                     : "Type them in: there is no STL to measure them from.", 34);
    int mode = have && s->forceAuto ? 0 : 1;
    static const char *MODE[] = { "From the part", "Typed in" };
    ui_disable(!have);
    if (ui_segmented(ui_id("su-fref"), (Rectangle){ cr.x, cr.y, fminf(280, cr.width), 34 }, MODE, 2, &mode) && (mode == 0) != s->forceAuto) { s->forceAuto = mode == 0; su_save(); }
    ui_disable(false);
    bool locked = have && s->forceAuto;
    if (locked) {
        SimForceRef a = auto_r;
        a.Uinf = s->fr.Uinf;                               /* the speed stays the user's (0: the inflow's) */
        if (memcmp(&a, &s->fr, sizeof a)) { s->fr = a; su_save(); }
    }
    SimForceRef *r = &s->fr;
    ui_disable(locked);
    su_row_num(x, &y, w, true, "su-flref", "Reference length", "The chord of a wing, the length of a car.", &r->lRef, 1e-12, 1e9, "m", NULL);
    su_row_num(x, &y, w, true, "su-faref", "Reference area", twoD ? "2-D: the chord times the mesh's depth, so the coefficients are the usual per-span ones."
                                                               : "The frontal area seen along the flow (a wing's is often its planform area instead).", &r->aRef, 1e-15, 1e12, "m²", NULL);
    bool all = !locked || SU.more[SS_REPORTS];              /* worked out from the part: the directions are under More */
    if (all) {
        su_row_vec(x, &y, w, true, "su-fdrag", "Drag direction", "Along the free stream.", r->drag, NULL);
        su_row_vec(x, &y, w, true, "su-flift", "Lift direction", "Square to the drag; up for a wing.", r->lift, NULL);
        su_row_vec(x, &y, w, true, "su-fcofr", "Moment centre", "The pitching moment Cm is about this point: a quarter of the length from the front.", r->CofR, "m");
    }
    ui_disable(false);
    if (all) {
        char a[96]; pl_fmt_ui(a, sizeof a, r->Uinf > 0 ? "" : "0: the inflow speed, %.4g m/s", sim_ref_speed(s));
        su_row_num(x, &y, w, true, "su-fu", "Reference speed", "The free stream's speed.", &r->Uinf, 0, 1e6, "m/s", a);
    }
    if (twoD) {
        const char *t = "2-D", *d = "Cl and Cd are those of the profile (per unit span); there is no side force. Cm is about the moment centre, nose up positive.";
        float ch = callout_height(w, t, d);
        callout((Rectangle){ x, y + 6, w, ch }, TOAST_INFO, ICON_INFO, t, d);
        y += ch + 12;
    }
    if (locked) y += su_more_toggle(SS_REPORTS, x, y);
    return y - y0;
}

/* ---------------------------------------------------------------------------- 4 Zones (plan step 7) */
/* a name in a space-separated list of names */
static bool in_list(const char *list, const char *name)
{
    size_t n = strlen(name);
    for (const char *p = list; (p = strstr(p, name)); p += n)
        if ((p == list || p[-1] == ' ') && (p[n] == 0 || p[n] == ' ')) return true;
    return false;
}

void su_list_set(char *list, size_t cap, const char *name, bool on)
{
    char out[512] = "", w[128];
    for (const char *p = list; *p; ) {
        while (*p == ' ') p++;
        size_t k = strcspn(p, " ");
        if (!k) break;
        snprintf(w, sizeof w, "%.*s", (int)k, p);
        p += k;
        if (!strcmp(w, name)) continue;
        snprintf(out + strlen(out), sizeof out - strlen(out), "%s%s", out[0] ? " " : "", w);
    }
    if (on) snprintf(out + strlen(out), sizeof out - strlen(out), "%s%s", out[0] ? " " : "", name);
    pl_strlcpy(list, out, cap);
}

/* the walls as check boxes that wrap, ticked when in the list; true when one changed */
static bool wall_checks(float x, float *y, float w, const char *id, const char *label, const char *hint, char *list, size_t cap)
{
    SimSetup *s = &SU.s;
    float room = w - fminf(300, w * 0.38f), lineW = 0, lines = 1;
    int nw = 0;
    for (int i = 0; i < s->nB; i++) {
        const SimBoundary *b = &s->b[i];
        if (b->role != ROLE_WALL || sim_constraint(b->meshType)) continue;
        float cw = ui_text_w(F_REG, 13.5f, b->name) + 44;
        if (lineW + cw > room && lineW > 0) { lines++; lineW = 0; }
        lineW += cw; nw++;
    }
    Rectangle cr = prop_row(x, y, w, label, hint, nw ? lines * 30 - 6 : 24);
    if (!nw) { ui_text(F_REG, 13, "No walls.", cr.x, cr.y + 4, TH.text3); return false; }
    float bx = cr.x, by = cr.y;
    bool ch = false;
    for (int i = 0; i < s->nB; i++) {
        const SimBoundary *b = &s->b[i];
        if (b->role != ROLE_WALL || sim_constraint(b->meshType)) continue;
        float cw = ui_text_w(F_REG, 13.5f, b->name) + 44;
        if (bx + cw > cr.x + cr.width && bx > cr.x) { bx = cr.x; by += 30; }
        bool on = in_list(list, b->name), was = on;
        ui_checkbox(ui_idn(ui_id(id), i), (Rectangle){ bx, by, cw - 14, 24 }, &on, b->name);
        if (on != was) { su_list_set(list, cap, b->name, on); ch = true; }
        bx += cw;
    }
    return ch;
}

/* the mesh's cellZones (constant/polyMesh/cellZones), read again when it changes */
int su_mesh_zones(const char *casePath, char (*out)[64])
{
    static char z[16][64], at[PL_PATH]; static int n; static double mt = -1;
    char p[PL_PATH]; pl_join(p, sizeof p, casePath, "constant/polyMesh/cellZones");
    double m = su_mtime_ns(p);
    if (strcmp(at, casePath) || m != mt) { pl_strlcpy(at, casePath, sizeof at); mt = m; n = m > 0 ? sim_mesh_cell_zones(casePath, z, 16) : 0; }
    memcpy(out, z, sizeof z);
    return n;
}

float zones_step(Case *c, float x, float y, float w)
{
    float y0 = y;
    SimSetup *s = &SU.s;
    y += su_issues_callout(SS_ZONES, x, y, w);
    y += su_note(x, y, w, "Parts of the fluid that turn (a fan, an impeller, a mixer: a rotating reference frame, the blades held still and the "
                       "flow round them solved as it turns) or that resist the flow (a filter, a screen, a radiator, a packed bed: a porous "
                       "medium). Most cases need none.");
    char mz[16][64]; int nmz = su_mesh_zones(c->s.path, mz);
    static const char *ADD[ZN__N + 1] = { "Add a zone ...", "Rotating zone: a fan, an impeller, a mixer", "Porous zone: a filter, a screen, a radiator" };
    int pick = 0;
    ui_disable(s->nZ >= SIM_MAX_Z);
    if (ui_select(ui_id("su-zone-add"), (Rectangle){ x, y + 6, fminf(420, w), 36 }, ADD, ZN__N + 1, &pick) && pick > 0) {
        double lo[3], hi[3]; bool hb = mesh_bounds(c->s.path, lo, hi);
        /* the mesh's own zone, when it has one no zone uses yet */
        const char *free_z = NULL;
        for (int k = 0; k < nmz && !free_z; k++) {
            bool used = false;
            for (int i = 0; i < s->nZ; i++) if (s->z[i].shape == ZS_MESH && !strcmp(s->z[i].zone, mz[k])) used = true;
            if (!used) free_z = mz[k];
        }
        int i = sim_zone_add(s, pick - 1, hb ? lo : NULL, hb ? hi : NULL, free_z);
        if (i >= 0) { sim_settle(s); su_save(); }
    }
    ui_disable(false);
    y += 56;
    int del = -1;
    for (int i = 0; i < s->nZ; i++) {
        SimZone *z = &s->z[i];
        ui_push_id(ui_idn(ui_id("su-z"), i));
        float cy = y + 12;
        char t[160], sub[200];
        if (z->kind == ZN_ROTATE) pl_fmt_ui(t, sizeof t, "Rotating zone %s  ·  %.6g rpm", z->name, z->rpm);
        else { double d, f; sim_zone_porous(s, z, &d, &f); pl_fmt_ui(t, sizeof t, "Porous zone %s  ·  %.4g Pa at %.4g m/s", z->name, sim_zone_drop(s, z, z->U1 > 0 ? z->U1 : 1), z->U1 > 0 ? z->U1 : 1); }
        ui_text_clip(F_SEMI, 14, t, x + 16, cy + 2, w - 80, TH.text);
        pl_fmt_ui(sub, sizeof sub, "cellZone %s  ·  %s%s", z->shape == ZS_MESH ? z->zone : z->name, z->kind == ZN_ROTATE ? "constant/MRFProperties" : "system/fvOptions",
                  z->shape == ZS_MESH ? "" : "  ·  marked by topoSet before the run");
        ui_text_clip(F_MONO, 12, sub, x + 16, cy + 22, w - 80, TH.text3);
        if (ui_button(ui_id("del"), (Rectangle){ x + w - 46, cy, 34, 34 }, ICON_X, NULL, BTN_ICON)) del = i;
        ui_tooltip((Rectangle){ x + w - 46, cy, 34, 34 }, "Remove this zone");
        cy += 50;
        float ix = x + 16, iw = w - 32;
        bool ch = false;
        /* the name: the cellZone and the dictionary entry */
        Rectangle cr = prop_row(ix, &cy, iw, "Name", "Letters, digits and _.", 34);
        {
            char b[48]; pl_strlcpy(b, z->name, sizeof b);
            if (ui_input(ui_id("name"), (Rectangle){ cr.x, cr.y, fminf(240, cr.width), 34 }, b, sizeof b, NULL, INPUT_MONO)) {
                bool okn = b[0] && !isdigit((unsigned char)b[0]);
                for (char *q = b; *q; q++) if (!isalnum((unsigned char)*q) && *q != '_') okn = false;
                for (int j = 0; j < s->nZ; j++) if (j != i && !strcmp(s->z[j].name, b)) okn = false;
                if (okn) { pl_strlcpy(z->name, b, sizeof z->name); ch = true; }
                else ui_toast(TOAST_ERR, "A zone's name: letters, digits and _, not starting with a digit, not another zone's");
            }
        }
        /* the cells */
        static const char *SH[ZS__N];
        for (int k = 0; k < ZS__N; k++) SH[k] = ZONE_SHAPE_NAME[k];
        int nsh = nmz > 0 || z->shape == ZS_MESH ? ZS__N : ZS_MESH;
        cr = prop_row(ix, &cy, iw, "Cells", nmz ? "A shape, or a cell zone the mesh already has." : "The cells inside this shape (in the mesh's axes, m).", 34);
        int sh = z->shape;
        if (ui_select(ui_id("shape"), (Rectangle){ cr.x, cr.y, fminf(240, cr.width), 34 }, SH, nsh, &sh) && sh != z->shape) {
            z->shape = sh;
            if (sh == ZS_MESH && nmz && !z->zone[0]) pl_strlcpy(z->zone, mz[0], sizeof z->zone);
            if (sh != ZS_CYLINDER) z->axisAuto = false;
            if (sh == ZS_SPHERE && !(z->r > 0)) z->r = 0.1;
            ch = true;
        }
        switch (z->shape) {
        case ZS_CYLINDER:
            ch |= su_row_vec(ix, &cy, iw, true, "a", "End 1", "The centre of one end.", z->a, "m");
            ch |= su_row_vec(ix, &cy, iw, true, "b", "End 2", "The centre of the other.", z->b, "m");
            ch |= su_row_num(ix, &cy, iw, true, "r", "Radius", NULL, &z->r, 1e-12, 1e9, "m", NULL);
            break;
        case ZS_BOX:
            ch |= su_row_vec(ix, &cy, iw, true, "a", "Least corner", NULL, z->a, "m");
            ch |= su_row_vec(ix, &cy, iw, true, "b", "Greatest corner", NULL, z->b, "m");
            break;
        case ZS_SPHERE:
            ch |= su_row_vec(ix, &cy, iw, true, "a", "Centre", NULL, z->a, "m");
            ch |= su_row_num(ix, &cy, iw, true, "r", "Radius", NULL, &z->r, 1e-12, 1e9, "m", NULL);
            break;
        default: {
            cr = prop_row(ix, &cy, iw, "Cell zone", nmz ? NULL : "The mesh has no cell zones now: mesh it, or pick a shape.", 34);
            const char *names[16]; int cur = -1;
            for (int k = 0; k < nmz; k++) { names[k] = mz[k]; if (!strcmp(mz[k], z->zone)) cur = k; }
            if (nmz) {
                int pz = cur < 0 ? 0 : cur;
                if (ui_select(ui_id("zone"), (Rectangle){ cr.x, cr.y, fminf(240, cr.width), 34 }, names, nmz, &pz) && (pz != cur || cur < 0)) {
                    pl_strlcpy(z->zone, mz[pz], sizeof z->zone); ch = true;
                }
            }
            break;
        }
        }
        if (z->kind == ZN_ROTATE) {
            char a[64]; pl_fmt_ui(a, sizeof a, "%.5g rad/s", sim_zone_omega(z));
            ch |= su_row_num(ix, &cy, iw, true, "rpm", "Speed", "Revolutions per minute; negative turns the other way (right-hand rule about the axis).",
                          &z->rpm, -1e7, 1e7, "rpm", a);
            /* the axis: the cylinder's, or along x, y, z through a point, or typed */
            static const char *AXN[5] = { "The cylinder's", "x", "y", "z", "Typed in" };
            int ax = z->axisAuto && z->shape == ZS_CYLINDER ? 0 : 4;
            if (!z->axisAuto || z->shape != ZS_CYLINDER)
                for (int k = 0; k < 3; k++) if (fabs(z->axis[k]) == 1 && fabs(z->axis[(k + 1) % 3]) + fabs(z->axis[(k + 2) % 3]) == 0) ax = k + 1;
            cr = prop_row(ix, &cy, iw, "Axis", "It turns about this line.", 34);
            const char **opts = z->shape == ZS_CYLINDER ? AXN : AXN + 1;
            int nopt = z->shape == ZS_CYLINDER ? 5 : 4, sel = z->shape == ZS_CYLINDER ? ax : ax - 1;
            if (ui_segmented(ui_id("axis"), (Rectangle){ cr.x, cr.y, fminf(420, cr.width), 34 }, opts, nopt, &sel)) {
                int na = z->shape == ZS_CYLINDER ? sel : sel + 1;
                if (na != ax) {
                    double a3[3], o3[3]; sim_zone_axis(z, a3, o3);
                    z->axisAuto = na == 0;
                    if (na >= 1 && na <= 3) { z->axis[0] = z->axis[1] = z->axis[2] = 0; z->axis[na - 1] = 1; }
                    else if (na == 4) memcpy(z->axis, a3, sizeof a3);
                    if (na != 0) memcpy(z->origin, o3, sizeof o3);
                    ch = true;
                }
            }
            if (!(z->axisAuto && z->shape == ZS_CYLINDER)) {
                if (ax == 4) ch |= su_row_vec(ix, &cy, iw, true, "axv", "Axis direction", NULL, z->axis, NULL);
                ch |= su_row_vec(ix, &cy, iw, true, "org", "Through", "A point on the axis.", z->origin, "m");
            }
            ch |= wall_checks(ix, &cy, iw, "still", "Walls kept still", "Walls inside the zone turn with it (the blades, the hub); tick any that "
                              "stand still there, like a casing or a shaft's housing.", z->still, sizeof z->still);
            ch |= wall_checks(ix, &cy, iw, "tq", "Torque on", "The torque of the flow on these walls about the axis, plotted on the Run page "
                              "(times the speed: the power).", z->torque, sizeof z->torque);
        } else {
            cr = prop_row(ix, &cy, iw, "Flow passes", "Along one axis (across it the resistance is 1000 times, like a honeycomb or a radiator), "
                                                      "or the same every way (a foam, a packed bed).", 34);
            int pd = z->pdir;
            if (ui_segmented(ui_id("pdir"), (Rectangle){ cr.x, cr.y, fminf(420, cr.width), 34 }, (const char **)PDIR_NAME, PD__N, &pd) && pd != z->pdir) { z->pdir = pd; ch = true; }
            if (!z->coeffs) {
                ch |= su_row_num(ix, &cy, iw, true, "th", "Thickness", "The depth of the medium the pressure drops were measured across.", &z->thick, 1e-9, 1e6, "m", NULL);
                ch |= su_row_num(ix, &cy, iw, true, "dp1", "Pressure drop", "From a datasheet or a test.", &z->dp1, 0, 1e12, "Pa", NULL);
                ch |= su_row_num(ix, &cy, iw, true, "u1", "at the speed", "The approach (superficial) speed: the flow rate over the frontal area.", &z->U1, 1e-9, 1e6, "m/s", NULL);
                ch |= su_row_num(ix, &cy, iw, true, "dp2", "Second drop", "Optional: a second point gives both the viscous and the inertial "
                              "resistance; with one, the drop is taken to grow with the speed squared.", &z->dp2, 0, 1e12, "Pa", z->dp2 > 0 ? NULL : "0: none");
                if (z->dp2 > 0) ch |= su_row_num(ix, &cy, iw, true, "u2", "at the speed", NULL, &z->U2, 0, 1e6, "m/s", NULL);
            } else {
                ch |= su_row_num(ix, &cy, iw, true, "d", "Darcy d", "Viscous resistance: 1 / permeability.", &z->d, 0, 1e30, "1/m²", NULL);
                ch |= su_row_num(ix, &cy, iw, true, "f", "Forchheimer f", "Inertial resistance.", &z->f, 0, 1e30, "1/m", NULL);
                ch |= su_row_num(ix, &cy, iw, true, "th", "Thickness", "For the drop shown below.", &z->thick, 1e-9, 1e6, "m", NULL);
            }
            double d, f; const char *why = sim_zone_porous(s, z, &d, &f);
            char line[300];
            double U = z->U1 > 0 ? z->U1 : 1;
            pl_fmt_ui(line, sizeof line, "d = %.4g 1/m², f = %.4g 1/m: %.4g Pa at %.4g m/s, %.4g Pa at twice that%s%s", d, f, sim_zone_drop(s, z, U), U,
                      sim_zone_drop(s, z, 2 * U), why ? ". " : "", why ? why : "");
            cy += su_note(ix, cy + 2, iw, line) + 6;
            Rectangle mr = prop_row(ix, &cy, iw, "Type the coefficients", "Darcy-Forchheimer's d and f instead of pressure drops.", 24);
            bool co = z->coeffs;
            if (ui_switch(ui_id("coeffs"), mr.x, mr.y + 2, &co)) { if (co) { z->d = d; z->f = f; } z->coeffs = co; ch = true; }
        }
        if (ch) { sim_settle(s); su_save(); }
        Rectangle card = { x, y, w, cy - y + 6 };
        ui_rect_lines(card, 10, 1, TH.lineSoft);
        ui_pop_id();
        y += card.height + 12;
    }
    if (del >= 0) { sim_zone_remove(s, del); su_save(); }
    if (!s->nZ) y += su_note(x, y, w, "No zones: the whole domain is plain, still-standing fluid.") + 4;
    return y + 8 - y0;
}

/* ---------------------------------------------------------------------------- 4 Starting values */
/* a free surface's water at the start: boxes, or all below a height (setFields fills alpha.water before the run) */
/* the regions at the start (plan steps 8 and 9): a free surface's water, or a single fluid's own temperatures and velocities
   (a hot block, a jet); each a box, everything below a height, or a zone's cells */
static float regions_start(float x, float y, float w)
{
    float y0 = y;
    SimSetup *s = &SU.s;
    bool vof = sim_is_vof(s), hasT = s->energy != EN_OFF || sim_is_gas(s);
    y += 10;
    if (vof) y += section_head(x, y, w, "Water at the start", "Air everywhere else. Boxes and heights are in the mesh's axes; the Run page's setFields step "
                                                            "puts the water in before each fresh run.");
    else y += section_head(x, y, w, "Regions", hasT ? "Parts of the fluid that start at their own temperature or velocity (a hot block, a jet). The Run page's "
                                                       "setFields step sets them before each fresh run."
                                                     : "Parts of the fluid that start at their own velocity (a jet, a moving slug). The Run page's setFields step "
                                                       "sets them before each fresh run.");
    static const char *const AX[3] = { "x", "y", "z" };
    /* the zones a region can be: the Zones step's and the mesh's own cellZones */
    char zn[SIM_MAX_Z + 16][64]; int nzn = 0;
    for (int i = 0; i < s->nZ; i++) pl_strlcpy(zn[nzn++], s->z[i].name, 64);
    if (A.cs) {
        char mz[16][64]; int nm = su_mesh_zones(A.cs->s.path, mz);
        for (int i = 0; i < nm && nzn < SIM_MAX_Z + 16; i++) {
            bool dup = false;
            for (int k = 0; k < nzn; k++) if (!strcmp(zn[k], mz[i])) dup = true;
            if (!dup) pl_strlcpy(zn[nzn++], mz[i], 64);
        }
    }
    int up = sim_up_axis(s), del = -1;
    for (int i = 0; i < s->nFill; i++) {
        SimFill *f = &s->fill[i];
        ui_push_id(ui_idn(ui_id("su-w"), i));
        float cy = y + 10;
        char t[96]; pl_fmt_ui(t, sizeof t, vof ? "Water %d" : "Region %d", i + 1);
        ui_text(F_SEMI, 14, t, x + 16, cy + 4, TH.text);
        int k = f->kind;
        const char *kw[FL__N] = { NULL, NULL, nzn ? NULL : "No zones: add one on the Zones step, or mesh with cellZones" };
        if (ui_segmented_why(ui_id("kind"), (Rectangle){ x + 110, cy, fminf(380, w - 180), 32 }, (const char **)FILL_NAME, FL__N, &k, kw) && k != f->kind) {
            f->kind = k;
            if (k == FL_ZONE && !f->zone[0] && nzn) pl_strlcpy(f->zone, zn[0], sizeof f->zone);
            su_save();
        }
        if (ui_button(ui_id("del"), (Rectangle){ x + w - 46, cy, 34, 34 }, ICON_X, NULL, BTN_ICON)) del = i;
        ui_tooltip((Rectangle){ x + w - 46, cy, 34, 34 }, vof ? "Remove this water" : "Remove this region");
        cy += 46;
        float ix = x + 16, iw = w - 32;
        if (f->kind == FL_BOX) {
            su_row_vec(ix, &cy, iw, true, "a", "Least corner", NULL, f->a, "m");
            su_row_vec(ix, &cy, iw, true, "b", "Greatest corner", NULL, f->b, "m");
        } else if (f->kind == FL_BELOW) {
            char h[64]; pl_fmt_ui(h, sizeof h, "Along %s, against gravity", AX[up]);
            su_row_num(ix, &cy, iw, true, "h", vof ? "Water up to" : "Up to", h, &f->h, -1e6, 1e6, "m", NULL);
        } else {
            Rectangle zr = prop_row(ix, &cy, iw, "Zone", "Its cells: a zone of the Zones step (marked by the Run page's Cell zones step first) or of the mesh.", 34);
            const char *items[SIM_MAX_Z + 16]; int sel = -1;
            for (int z = 0; z < nzn; z++) { items[z] = zn[z]; if (!strcmp(zn[z], f->zone)) sel = z; }
            if (nzn && ui_select(ui_id("zone"), (Rectangle){ zr.x, zr.y, fminf(260, zr.width), 34 }, items, nzn, &sel) && sel >= 0) {
                pl_strlcpy(f->zone, zn[sel], sizeof f->zone); su_save();
            }
            if (!nzn) ui_text_clip(F_REG, 13, "No zones yet", zr.x, zr.y + 9, zr.width, TH.warn);
        }
        if (!vof) {
            if (hasT) {
                Rectangle tr = prop_row(ix, &cy, iw, "Own temperature", NULL, 34);
                if (ui_switch(ui_id("sett"), tr.x, tr.y + 6, &f->setT)) { if (f->setT && !(f->T > 0)) f->T = s->T0 + 20; su_save(); }
                if (f->setT) {
                    char cc[48]; pl_fmt_ui(cc, sizeof cc, "%.4g °C", f->T - 273.15);
                    if (su_num_in(ui_id("t"), (Rectangle){ tr.x + 56, tr.y, fminf(160, tr.width - 56), 34 }, &f->T, "%.6g", 1, 1e4, "K")) su_save();
                    if (tr.width > 330) ui_text_clip(F_REG, 13, cc, tr.x + 228, tr.y + 9, tr.width - 228, TH.text3);
                }
            }
            Rectangle ur = prop_row(ix, &cy, iw, "Own velocity", NULL, 34);
            if (ui_switch(ui_id("setu"), ur.x, ur.y + 6, &f->setU)) su_save();
            if (f->setU) su_row_vec(ix, &cy, iw, true, "u", "Velocity", NULL, f->U, "m/s");
        }
        Rectangle card = { x, y, w, cy - y + 4 };
        ui_rect_lines(card, 10, 1, TH.lineSoft);
        ui_pop_id();
        y += card.height + 10;
    }
    if (del >= 0) { memmove(&s->fill[del], &s->fill[del + 1], sizeof s->fill[0] * (size_t)(s->nFill - del - 1)); s->nFill--; su_save(); }
    const char *addL = vof ? "Add water" : "Add a region";
    float bw = ui_button_width(ICON_PLUS, addL);
    ui_disable(s->nFill >= SIM_MAX_FILL);
    if (ui_button(ui_id("su-w-add"), (Rectangle){ x, y + 2, bw, 34 }, ICON_PLUS, addL, BTN_SECONDARY)) {
        double lo[3], hi[3];
        SimFill *f = &s->fill[s->nFill++];
        memset(f, 0, sizeof *f);
        if (A.cs && mesh_bounds(A.cs->s.path, lo, hi)) {
            f->kind = FL_BOX;
            for (int k = 0; k < 3; k++) { f->a[k] = lo[k]; f->b[k] = lo[k] + (hi[k] - lo[k]) / 4; }
            for (int k = 0; k < 3; k++) if (hi[k] - lo[k] < 0.2 * (hi[up] - lo[up])) { f->a[k] = lo[k]; f->b[k] = hi[k]; }   /* a 2-D mesh's depth: all of it */
            f->h = lo[up] + (hi[up] - lo[up]) / 3;
        } else { f->kind = FL_BELOW; f->h = 0.1; }
        if (!vof) { f->setT = hasT; f->T = s->T0 + 20; f->setU = !hasT; }   /* a hot block; without heat, a moving one */
        su_save();
    }
    ui_disable(false);
    y += 48;
    double lo[3], hi[3];
    if (vof && A.cs && mesh_bounds(A.cs->s.path, lo, hi)) {
        double V = 1; for (int k = 0; k < 3; k++) V *= hi[k] - lo[k];
        double vw = sim_fill_volume(s, lo, hi);
        char t[200];
        if (s->nFill) pl_fmt_ui(t, sizeof t, "About %.4g m³ of water (%.3g litres), %.0f %% of the mesh's box.", vw, vw * 1000, V > 0 ? 100 * vw / V : 0);
        else pl_strlcpy(t, "No water at the start: it comes in through an inlet, or add some.", sizeof t);
        y += su_note(x, y, w, t) + 6;
    }
    if (vof) su_row_vec(x, &y, w, true, "su-uw", "Water velocity", "The starting water's own velocity (a moving slug, a wave); the air starts with the velocity above.", s->Uw, "m/s");
    return y - y0;
}

/* another case's results to start from (plan step 9): the workspace's OpenFOAM cases with results, or a folder typed in */
static float map_source(Case *c, float x, float y, float w)
{
    float y0 = y;
    SimSetup *s = &SU.s;
    static const char *items[64]; static char names[64][96]; static int idx[64];
    int n = 0, sel = -1;
    /* the cases as they are now, each time this comes into view (the Cases list may be from before the other case ran) */
    static double lastDrawn = -10;
    if (ui_time() - lastDrawn > 1) app_rescan_cases(true);
    lastDrawn = ui_time();
    for (int i = 0; i < A.nList && n < 64; i++) {
        const CaseSummary *cs = &A.list[i];
        if (cs->prog != 0 || cs->nTimes <= 0 || (c && !strcmp(cs->path, c->s.path))) continue;
        pl_strlcpy(names[n], cs->name, sizeof names[n]);
        items[n] = names[n]; idx[n] = i;
        if (!strcmp(cs->path, s->mapFrom)) sel = n;
        n++;
    }
    Rectangle cr = prop_row(x, &y, w, "Case", "One of your cases with results; its last results are interpolated onto this mesh. The boundaries keep this setup's conditions.", 34);
    if (n) {
        if (ui_select(ui_id("su-map-case"), (Rectangle){ cr.x, cr.y, fminf(300, cr.width), 34 }, items, n, &sel) && sel >= 0) {
            pl_strlcpy(s->mapFrom, A.list[idx[sel]].path, sizeof s->mapFrom); su_save();
        }
    } else ui_text_clip(F_REG, 13, "None of your other cases has results yet: type a folder below", cr.x, cr.y + 9, cr.width, TH.text3);
    cr = prop_row(x, &y, w, "Folder", NULL, 34);
    char b[512]; pl_strlcpy(b, s->mapFrom, sizeof b);
    if (ui_input(ui_id("su-map-dir"), (Rectangle){ cr.x, cr.y, cr.width, 34 }, b, sizeof b, "/path/to/case", INPUT_MONO)) {
        pl_trim_inplace(b); pl_strlcpy(s->mapFrom, b, sizeof s->mapFrom); su_save();
    }
    char t[64], why[600], line[700];
    bool ok = sim_map_source(s->mapFrom, t, sizeof t, why, sizeof why);
    if (ok && c && !strcmp(s->mapFrom, c->s.path)) { ok = false; pl_strlcpy(why, "That is this case: choose another", sizeof why); }
    if (ok) pl_fmt_ui(line, sizeof line, "Starts from %s at t = %s (the Run page's Starting values step runs mapFields before the solver).", pl_basename(s->mapFrom), t);
    else pl_strlcpy(line, why, sizeof line);
    float ch = callout_height(w, NULL, line);
    callout((Rectangle){ x, y, w, ch }, ok ? TOAST_INFO : TOAST_WARN, ok ? ICON_INFO : ICON_TRIANGLE_ALERT, NULL, line);
    y += ch + 10;
    return y - y0;
}

float start_step(float x, float y, float w)
{
    float y0 = y;
    SimSetup *s = &SU.s;
    Case *c = A.cs;
    y += su_note(x, y, w, "What the fluid inside starts as. A steady run forgets them; a good guess only gets there sooner.");
    y += su_issues_callout(SS_START, x, y, w);
    /* plan step 9: uniform, potential flow, or another case's results */
    Rectangle sr = prop_row(x, &y, w, "Start from", "Uniform: the values below. Potential flow: potentialFoam works out a flow that already goes round the part. "
                                                    "Another case: its last results, interpolated onto this mesh.", 34);
    {
        int sv = s->start;
        const char *sw[SV__N]; for (int i = 0; i < SV__N; i++) sw[i] = sim_start_why_not(s, i);
        if (ui_segmented_why(ui_id("su-start"), (Rectangle){ sr.x, sr.y, fminf(480, sr.width), 34 }, (const char **)START_NAME, SV__N, &sv, sw) && sv != s->start) {
            s->start = sv; su_save();
        }
    }
    bool plainPot = s->start == SV_POTENTIAL && s->energy == EN_OFF && !sim_is_gas(s);
    if (s->start == SV_MAP) y += map_source(c, x, y, w);
    if (s->start == SV_POTENTIAL)
        y += su_note(x, y, w, plainPot ? "The Run page's Starting values step runs potentialFoam before the solver: the velocity, the pressure and the flux of an "
                                      "inviscid flow from these boundaries. The turbulence below starts uniform."
                                    : "The Run page's Starting values step runs potentialFoam before the solver: the velocity of an inviscid flow from these "
                                      "boundaries. The pressure and temperature start at the values below.") + 6;
    if (s->start == SV_UNIFORM)
        su_row_vec(x, &y, w, true, "su-u0", "Velocity", SU.external ? "The free stream, for a body in a wind tunnel." : "Usually still (0 0 0) inside a part.", s->U0, "m/s");
    if (s->start != SV_MAP && !plainPot)
        su_row_num(x, &y, w, true, "su-p0", "Pressure", sim_is_vof(s) ? "Gauge, without the water's weight (p_rgh: the hydrostatic part is added by itself)." : "Gauge.",
                &s->p0, -1e9, 1e9, "Pa", NULL);
    if (s->energy != EN_OFF && s->start != SV_MAP) {
        char cc[48]; pl_fmt_ui(cc, sizeof cc, "%.4g °C", s->T0 - 273.15);
        double was = s->T0;
        if (su_row_num(x, &y, w, true, "su-T0", "Temperature", "In kelvin. New boundaries start at it too.", &s->T0, 1, 1e4, "K", cc)) {
            /* the boundaries still at the old start follow it (they were never set) */
            for (int i = 0; i < s->nB; i++) {
                SimBoundary *b = &s->b[i];
                if (b->T == was) b->T = s->T0;
                if (b->Ta == was) b->Ta = s->T0;
            }
            su_save();
        }
    }
    /* a steady run's inlets ramped up over the first iterations: for a case that diverges early */
    if (s->flow == FLOW_STEADY) {
        Rectangle rr = prop_row(x, &y, w, "Ramp the inlets", "The inlet speeds and flow rates rise from a tenth to their value over the first iterations, "
                                                          "for a run that diverges early. Free-stream openings stay as they are.", 24);
        bool on = s->ramp >= 1;
        if (ui_switch(ui_id("su-ramp"), rr.x, rr.y + 2, &on)) { s->ramp = on ? fmax(50, fmin(200, 0.1 * s->iterations)) : 0; su_save(); }
        if (on) su_row_num(x, &y, w, true, "su-rampn", "Over", NULL, &s->ramp, 1, 1e7, "iterations", NULL);
    }
    y += regions_start(x, y, w);
    if (s->turb == TURB_LAMINAR || s->start == SV_MAP) return y - y0;
    if (SU.more[SS_START]) {
        double k, om, ep, nt;
        sim_turbulence(s, sim_ref_speed(s), s->intensity, s->length, &k, &om, &ep, &nt);
        char a[120];
        if (s->turb == TURB_SPALART) pl_fmt_ui(a, sizeof a, "nuTilda %.3g m²/s", nt);
        else pl_fmt_ui(a, sizeof a, "k %.3g m²/s², %s %.3g", k, s->turb == TURB_KOMEGASST ? "omega" : "epsilon", s->turb == TURB_KOMEGASST ? om : ep);
        su_row_num(x, &y, w, true, "su-ti", "Turbulence intensity", "Inside at the start, and the default for new inlets.", &s->intensity, 0.01, 100, "%", a);
        su_row_num(x, &y, w, true, "su-tl", "Mixing length", NULL, &s->length, 1e-9, 1e6, "m", NULL);
    }
    y += su_more_toggle(SS_START, x, y);
    return y - y0;
}

/* ---------------------------------------------------------------------------- 6 Solver */
/* the numerics preset (plan step 9): schemes, linear solvers and relaxation together. Hand edits to system/fvSchemes or
   fvSolution since the setup wrote them make it Custom: they are kept, and Back to the preset drops them */
static const char *const NUM_HINT[NUM__N] = {
    "First-order upwind and firmly relaxed SIMPLE: gets a hard case going (a coarse mesh, a high speed, a run that diverged). Less "
    "accurate: once it runs, Balanced from its results (Starting values, Another case) sharpens it.",
    "Second order for the velocity, first order for the turbulence, SIMPLEC: the usual choice.",
    "Second order everywhere and little limiting: for a good mesh. Can diverge on a poor one." };

static float numerics_rows(float x, float y, float w)
{
    float y0 = y;
    SimSetup *s = &SU.s;
    Case *c = A.cs;
    bool edited = c && sim_numerics_edited(c->s.path);
    Rectangle nr = prop_row(x, &y, w, "Numerics", NUM_HINT[s->numerics], 34);
    int nm = s->numerics;
    if (ui_segmented(ui_id("su-num"), (Rectangle){ nr.x, nr.y, fminf(360, nr.width), 34 }, (const char **)NUM_NAME, NUM__N, &nm) && nm != s->numerics) {
        s->numerics = nm; su_save();
    }
    if (edited) {
        char t[300]; pl_fmt_ui(t, sizeof t, "Custom: system/fvSchemes or fvSolution has hand edits since the setup wrote them. Writing the case files keeps them "
                                             "(where %s sets the same setting, %s's value goes in). Back to %s drops them.", NUM_NAME[s->numerics], NUM_NAME[s->numerics], NUM_NAME[s->numerics]);
        char back[48]; pl_fmt_ui(back, sizeof back, "Back to %s", NUM_NAME[s->numerics]);
        float b1 = ui_button_width(ICON_ROTATE_CCW, back), b2 = ui_button_width(ICON_ARROW_RIGHT, "Numerics page");
        float bw = fmaxf(b1, b2);
        bool side = w - bw - 16 >= 360;
        float cw = side ? w - bw - 16 : w;
        float ch = fmaxf(callout_height(cw, "Custom numerics", t), side ? 2 * 36 + 8 : 0);
        callout((Rectangle){ x, y, cw, ch }, TOAST_INFO, ICON_PENCIL, "Custom numerics", t);
        float bx = side ? x + w - bw : x, by = side ? y : y + ch + 8;
        ui_disable(A.cj->active);
        if (ui_button(ui_id("su-num-back"), (Rectangle){ bx, by, bw, 36 }, ICON_ROTATE_CCW, back, BTN_SECONDARY)) {
            sim_numerics_reset(c->s.path);
            case_drop_file(c, "system/fvSchemes"); case_drop_file(c, "system/fvSolution");
            if (su_write_files(c, false)) ui_toast(TOAST_OK, "fvSchemes and fvSolution are %s's again", NUM_NAME[s->numerics]);
        }
        ui_disable(false);
        if (ui_button(ui_id("su-num-open"), side ? (Rectangle){ bx, by + 44, bw, 36 } : (Rectangle){ bx + bw + 8, by, bw, 36 }, ICON_ARROW_RIGHT, "Numerics page", BTN_GHOST))
            app_go(PG_NUMERICS);
        y += side ? ch + 12 : ch + 56;
    }
    return y - y0;
}

float solver_step(float x, float y, float w)
{
    float y0 = y;
    SimSetup *s = &SU.s;
    y += su_note(x, y, w, s->flow == FLOW_STEADY ? TextFormat("%s stops at this many iterations, or sooner once the residuals are under their targets (when that is on).", sim_app(s))
                                              : TextFormat("%s steps through time%s.", sim_app(s), s->algo != ALG_PISO ? "; with an adjustable step it keeps the Courant number under the limit" : ""));
    y += su_issues_callout(SS_SOLVER, x, y, w);
    y += numerics_rows(x, y, w);
    if (s->flow == FLOW_STEADY) {
        su_row_num(x, &y, w, true, "su-it", "Iterations", NULL, &s->iterations, 1, 1e8, NULL, NULL);
        su_row_num(x, &y, w, true, "su-we", "Write every", "Results are saved this often (iterations).", &s->writeEvery, 1, 1e8, NULL,
                s->writeEvery > s->iterations ? "More than the iterations: saved at the end" : NULL);
        Rectangle rr = prop_row(x, &y, w, "Stop when the residuals are small",
                                "The solver ends the run early, writing that iteration, once every residual is under its target. Off: it runs all the iterations.", 24);
        if (ui_switch(ui_id("su-res"), rr.x, rr.y + 2, &s->resStop)) su_save();
        if (s->resStop) {
            su_row_num(x, &y, w, true, "su-resp", "Pressure target", "The p residual to get under; 1e-4 is usual.", &s->resP, 1e-15, 1, NULL, NULL);
            su_row_num(x, &y, w, true, "su-resu", "Velocity and turbulence target", "U, k, omega ...; 1e-5 is usual.", &s->resRest, 1e-15, 1, NULL, NULL);
        }
        return y - y0;
    }
    su_row_num(x, &y, w, true, "su-et", "End time", NULL, &s->endTime, 1e-12, 1e12, "s", NULL);
    char a[96] = "";
    if (SU.size > 0) pl_fmt_ui(a, sizeof a, "%.3g s for the flow to cross the part once", SU.size / sim_ref_speed(s));
    su_row_num(x, &y, w, true, "su-dt", "Time step", s->algo == ALG_PISO ? "Fixed: choose it for a Courant number under 1 in the smallest cells." : "The first step, when it adjusts.",
            &s->deltaT, 1e-15, 1e9, "s", a);
    if (s->algo == ALG_PISO) {
        prop_row(x, &y, w, "Fixed time step", "pisoFoam keeps the time step above fixed: choose it for a Courant number under 1 in the smallest cells.", 0);
    } else {
        Rectangle cr = prop_row(x, &y, w, "Adjust the time step", "Keeps the Courant number (cells crossed a step) under the limit.", 24);
        if (ui_switch(ui_id("su-adj"), cr.x, cr.y + 2, &s->adjustDt)) su_save();
        if (s->adjustDt) su_row_num(x, &y, w, true, "su-co", "Max Courant number", s->algo == ALG_LARGE ? "5 to 10 with large-step PIMPLE." : "1 is safe; up to 2 or 3 with outer correctors.",
                                 &s->maxCo, 0.01, 100, NULL, NULL);
        su_row_count(x, &y, w, true, "su-nouter", "Outer correctors", s->algo == ALG_LARGE ? "Up to this many per time step; it moves on once the residuals are under the targets below."
                                                                                       : "1 is PISO-like; 2 or 3 steady a larger time step.", &s->nOuter, 1, 100);
    }
    if (s->algo == ALG_LARGE) {
        su_row_num(x, &y, w, true, "su-oresp", "Pressure target, each step", "The outer correctors stop once p's residual is under this.", &s->resP, 1e-15, 1, NULL, NULL);
        su_row_num(x, &y, w, true, "su-oresu", "Velocity and turbulence target, each step", NULL, &s->resRest, 1e-15, 1, NULL, NULL);
    }
    su_row_num(x, &y, w, true, "su-wt", "Write every", "Results are saved this often.", &s->writeTime, 1e-12, 1e12, "s",
            s->writeTime > s->endTime ? "Past the end time: saved at the end" : NULL);
    if (sim_is_vof(s)) {
        /* the surface's own limits: interFoam keeps both Courant numbers under theirs when the step adjusts */
        if (s->adjustDt) su_row_num(x, &y, w, true, "su-aco", "Max interface Courant", "The Courant number at the water's surface: 1 is safe, 0.5 for a sharper "
                                 "surface. The step is cut to keep both under their limits.", &s->maxAlphaCo, 0.01, 10, NULL, NULL);
        if (SU.more[SS_SOLVER]) {
            Rectangle ir = prop_row(x, &y, w, "Sharper surface (interIsoFoam)", "Geometric isoAdvector instead of MULES: a surface one cell thin, for waves "
                                                                                 "and sloshing. Slower per step.", 24);
            if (ui_switch(ui_id("su-iso"), ir.x, ir.y + 2, &s->iso)) su_save();
            if (!s->iso) {
                su_row_count(x, &y, w, true, "su-nac", "Alpha correctors", "MULES passes over the water fraction a step: 2 is usual (nAlphaCorr).", &s->nAlphaCorr, 1, 5);
                su_row_num(x, &y, w, true, "su-calpha", "Interface compression", "cAlpha: 1 keeps the surface sharp, 0 lets it smear.", &s->cAlpha, 0, 4, NULL, NULL);
            }
            su_row_count(x, &y, w, true, "su-nas", "Alpha sub-cycles", "Water-fraction steps a time step (nAlphaSubCycles): 1, more for a big Courant number.", &s->nAlphaSub, 1, 10);
        }
    }
    int ap = sim_approach(s->turb);
    if (ap == APPR_LES || ap == APPR_DES) {
        /* plan step 5: the time averages (fieldAverage) start once the start-up has washed out */
        Rectangle ar = prop_row(x, &y, w, "Averaging starts", "UMean, pMean and UPrime2Mean (fieldAverage) are averaged from then on: after the start-up has "
                                                               "washed out, a few times the flow takes to cross the part.", 34);
        int mode = s->avgStart < 0 ? 0 : 1;
        static const char *AM[] = { "Automatic", "Typed in" };
        float sw = fminf(240, ar.width * 0.5f);
        if (ui_segmented(ui_id("su-avgmode"), (Rectangle){ ar.x, ar.y, sw, 34 }, AM, 2, &mode) && (mode == 1) != (s->avgStart >= 0)) {
            s->avgStart = mode ? 0.4 * s->endTime : -1; su_save();
        }
        double v = sim_avg_start(s);
        ui_disable(s->avgStart < 0);
        if (su_num_in(ui_id("su-avg"), (Rectangle){ ar.x + sw + 10, ar.y, fminf(160, ar.width - sw - 10), 34 }, &v, "%.6g", 0, 1e12, "s")) { s->avgStart = v; su_save(); }
        ui_disable(false);
        double tc = SU.size > 0 ? SU.size / sim_ref_speed(s) : 0;
        if (tc > 0 && ar.width - sw - 180 > 120) {
            char t[96]; pl_fmt_ui(t, sizeof t, "%.3g crossings in%s", v / tc, s->avgStart < 0 ? ": 40 % of the end time" : "");
            ui_text_clip(F_REG, 12.5f, t, ar.x + sw + 180, ar.y + 9, ar.width - sw - 180, v / tc < 3 ? TH.warn : TH.text3);
        }
    }
    if (SU.more[SS_SOLVER]) {
        su_row_count(x, &y, w, true, "su-ncorr", "Pressure correctors", "2 is usual; 3 for a tighter pressure-velocity coupling each step.", &s->nCorr, 1, 10);
        char autoTs[64]; pl_fmt_ui(autoTs, sizeof autoTs, "Automatic (%s)", TS_NAME[sim_time_scheme(&(SimSetup){ .turb = s->turb, .timeScheme = TS_AUTO, .numerics = s->numerics })]);
        const char *ts[TS__N] = { autoTs, TS_NAME[TS_EULER], TS_NAME[TS_BACKWARD], TS_NAME[TS_CN] };
        Rectangle cr = prop_row(x, &y, w, "Time scheme", "Euler: first order, robust. Backward: second order, for LES and DES. Crank-Nicolson (0.9): second order, less damping.", 34);
        int tsel = s->timeScheme;
        if (ui_select(ui_id("su-ts"), (Rectangle){ cr.x, cr.y, fminf(260, cr.width), 34 }, ts, TS__N, &tsel) && tsel != s->timeScheme) { s->timeScheme = tsel; su_save(); }
    }
    y += su_more_toggle(SS_SOLVER, x, y);
    return y - y0;
}

/* ---------------------------------------------------------------------------- 7 Check */
float check_step(Case *c, float x, float y, float w, const SsStatus st[SS__N])
{
    float y0 = y;
    int nAll = 0;
    for (int k = 0; k < SS_CHECK; k++) {
        char t[1200];
        int n = su_step_issues(k, t, sizeof t);
        if (!n) continue;
        nAll += n;
        char go[64]; pl_fmt_ui(go, sizeof go, "Go to %s", SS_TITLE[k]);
        float bw = ui_button_width(ICON_ARROW_RIGHT, go);
        bool side = w - bw - 16 >= 360;
        float cw = side ? w - bw - 16 : w;
        float ch = fmaxf(callout_height(cw, SS_TITLE[k], t), 52);
        callout((Rectangle){ x, y, cw, ch }, st[k].state == SS_ERR ? TOAST_ERR : TOAST_WARN, ICON_TRIANGLE_ALERT, SS_TITLE[k], t);
        Rectangle br = side ? (Rectangle){ x + w - bw, y + (ch - 36) / 2, bw, 36 } : (Rectangle){ x, y + ch + 8, bw, 36 };
        if (ui_button(ui_idn(ui_id("su-check-go"), k), br, ICON_ARROW_RIGHT, go, BTN_SECONDARY)) SU.step = k;
        y += ch + (side ? 12 : 56);
    }
    if (!nAll) {
        const char *t = "Nothing to fix", *d = "Every step is set. Write case files, then run.";
        float ch = callout_height(w, t, d);
        callout((Rectangle){ x, y, w, ch }, TOAST_OK, ICON_CIRCLE_CHECK, t, d);
        y += ch + 12;
    }
    /* what Q-Continuum chose: the solver, the fields, the files */
    const char *f[SIM_MAX_F]; int nf = sim_fields(&SU.s, f);
    char fl[96] = "";
    for (int i = 0; i < nf; i++) snprintf(fl + strlen(fl), sizeof fl - strlen(fl), "%s%s", i ? ", " : "", f[i]);
    y += 6;
    y += section_head(x, y, w, "What will be written", NULL);
    Rectangle cr = prop_row(x, &y, w, "Solver", NULL, 20);
    ui_text_clip(F_MONO, 13.5f, sim_app(&SU.s), cr.x, cr.y + 2, cr.width, TH.text);
    cr = prop_row(x, &y, w, "Fields in 0/", NULL, 20);
    ui_text_clip(F_MONO, 13.5f, fl, cr.x, cr.y + 2, cr.width, TH.text);
    const char *files = sim_is_gas(&SU.s)
        ? "constant/turbulenceProperties, thermophysicalProperties; system/controlDict, fvSchemes, fvSolution, fvOptions"
        : SU.s.energy != EN_OFF
        ? "constant/transportProperties, turbulenceProperties, thermophysicalProperties, g; system/controlDict, fvSchemes, fvSolution"
        : "constant/transportProperties, turbulenceProperties; system/controlDict, fvSchemes, fvSolution";
    float lw = w - fminf(300, w * 0.38f);                    /* prop_row's control column */
    cr = prop_row(x, &y, w, "Files", NULL, ui_text_wrap(F_MONO, 13.5f, files, (Rectangle){ 0, 0, lw, 400 }, 20, TH.text2, false));
    ui_text_wrap(F_MONO, 13.5f, files, (Rectangle){ cr.x, cr.y + 2, cr.width, 400 }, 20, TH.text2, true);
    cr = prop_row(x, &y, w, "Case files", NULL, 20);
    bool stale = su_files_stale(c);
    ui_text_clip(F_REG, 13.5f, stale ? "Not written since the setup or the mesh changed" : "Written: they match this setup", cr.x, cr.y + 2, cr.width, stale ? TH.text2 : TH.ok);
    y += 10;
    ui_disable(A.cj->active);
    float bw = ui_button_width(ICON_PLAY, "Write and go to Run");
    if (ui_button(ui_id("su-check-run"), (Rectangle){ x, y, bw, 40 }, ICON_PLAY, "Write and go to Run", BTN_PRIMARY)) su_write_files(c, true);
    ui_disable(false);
    y += 52;
    return y - y0;
}

