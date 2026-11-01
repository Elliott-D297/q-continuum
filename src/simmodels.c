/* simmodels.c -- the simulation setup's models and what goes with what (see simsetup.h): each turbulence model's
 * approach (laminar, RANS, LES, DES), its OpenFOAM name and the fields it solves; which solver a setup runs (simpleFoam,
 * pimpleFoam, pisoFoam, with heat buoyantSimpleFoam, buoyantPimpleFoam, as a gas rhoSimpleFoam, rhoPimpleFoam); a
 * compressible gas's properties (Sutherland's viscosity, the speed of sound, the Mach number); which choices a setup rules out and why; and the setup made consistent after a choice changes
 * (a steady run takes a LES or DES model's RANS counterpart). Every model named here ships with OpenFOAM v2606. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "simsetup.h"

typedef struct {
    const char *word, *name, *hint;
    int appr;
    int rans;                   /* LES and DES: the RANS model a steady run falls back to */
} Model;

static const Model M[TURB__N] = {
    [TURB_LAMINAR]     = { "laminar", "Laminar", "No turbulence model: slow flows, Reynolds number under about 2000 in a pipe.", APPR_LAMINAR, TURB_LAMINAR },
    [TURB_KOMEGASST]   = { "kOmegaSST", "k-omega SST", "The usual choice: good near walls and where the flow separates.", APPR_RANS, TURB_KOMEGASST },
    [TURB_KEPSILON]    = { "kEpsilon", "k-epsilon", "Robust for flows away from walls; weaker where the flow separates.", APPR_RANS, TURB_KEPSILON },
    [TURB_REALIZABLE]  = { "realizableKE", "Realizable k-epsilon", "k-epsilon that copes better with jets, swirl and separation.", APPR_RANS, TURB_REALIZABLE },
    [TURB_SPALART]     = { "SpalartAllmaras", "Spalart-Allmaras", "One equation, made for wings and attached flow; cheap.", APPR_RANS, TURB_SPALART },
    [TURB_RNG]         = { "RNGkEpsilon", "RNG k-epsilon", "k-epsilon tuned for strongly curved and swirling flow.", APPR_RANS, TURB_RNG },
    [TURB_SSTLM]       = { "kOmegaSSTLM", "k-omega SST with transition", "Finds where a laminar boundary layer turns turbulent; needs y+ about 1.", APPR_RANS, TURB_SSTLM },
    [TURB_SMAGORINSKY] = { "Smagorinsky", "Smagorinsky", "The classic LES model; too dissipative next to walls.", APPR_LES, TURB_KOMEGASST },
    [TURB_WALE]        = { "WALE", "WALE", "LES that behaves well next to walls: the usual LES choice.", APPR_LES, TURB_KOMEGASST },
    [TURB_DYNKEQN]     = { "dynamicKEqn", "Dynamic k-equation", "LES with a transport equation for the unresolved eddies' energy.", APPR_LES, TURB_KOMEGASST },
    [TURB_SADDES]      = { "SpalartAllmarasDDES", "Spalart-Allmaras DDES", "RANS in the boundary layers, LES away from them: the usual DES choice.", APPR_DES, TURB_SPALART },
    [TURB_SAIDDES]     = { "SpalartAllmarasIDDES", "Spalart-Allmaras IDDES", "DDES that also resolves the outer boundary layer on a fine mesh.", APPR_DES, TURB_SPALART },
    [TURB_SSTDDES]     = { "kOmegaSSTDDES", "k-omega SST DDES", "DDES built on k-omega SST.", APPR_DES, TURB_KOMEGASST },
};

const char *const APPR_NAME[APPR__N] = { "Laminar", "RANS", "LES", "DES" };
const char *const ALG_NAME[ALG__N] = { "PISO", "PIMPLE", "Large-step PIMPLE" };
const char *const TS_NAME[TS__N] = { "Automatic", "Euler", "Backward", "Crank-Nicolson" };
/* the names in the order of TURB_*: the menus show sim_approach_models' lists */
const char *const TURB_NAME[TURB__N] = { "Laminar", "k-omega SST", "k-epsilon", "Realizable k-epsilon", "Spalart-Allmaras", "RNG k-epsilon",
                                         "k-omega SST with transition", "Smagorinsky", "WALE", "Dynamic k-equation", "Spalart-Allmaras DDES",
                                         "Spalart-Allmaras IDDES", "k-omega SST DDES" };

static bool ok_turb(int t) { return t >= 0 && t < TURB__N; }

int sim_approach(int turb) { return ok_turb(turb) ? M[turb].appr : APPR_RANS; }
const char *sim_model_word(int turb) { return ok_turb(turb) ? M[turb].word : "kOmegaSST"; }
const char *sim_model_hint(int turb) { return ok_turb(turb) ? M[turb].hint : ""; }

const char *sim_approach_label(const SimSetup *s, int appr)
{
    if (appr == APPR_RANS && s->flow == FLOW_TRANSIENT) return "URANS";
    return appr >= 0 && appr < APPR__N ? APPR_NAME[appr] : "";
}

int sim_approach_models(int appr, int out[TURB__N])
{
    static const int order[APPR__N][7] = {
        { TURB_LAMINAR, -1 },
        { TURB_KOMEGASST, TURB_KEPSILON, TURB_REALIZABLE, TURB_RNG, TURB_SPALART, TURB_SSTLM, -1 },
        { TURB_WALE, TURB_SMAGORINSKY, TURB_DYNKEQN, -1 },
        { TURB_SADDES, TURB_SAIDDES, TURB_SSTDDES, -1 },
    };
    int n = 0;
    if (appr < 0 || appr >= APPR__N) return 0;
    for (int i = 0; i < 7 && order[appr][i] >= 0; i++) out[n++] = order[appr][i];
    return n;
}

bool sim_two_d(const SimSetup *s)
{
    for (int i = 0; i < s->nB; i++) if (!strcmp(s->b[i].meshType, "empty")) return true;
    return false;
}

const char *sim_approach_why_not(const SimSetup *s, int appr)
{
    if (s->flow == FLOW_STEADY && appr == APPR_LES) return "LES follows eddies as they move, so it needs a transient run";
    if (s->flow == FLOW_STEADY && appr == APPR_DES) return "DES resolves eddies away from walls, so it needs a transient run";
    return NULL;
}

const char *sim_algo_why_not(const SimSetup *s, int algo)
{
    if (s->flow == FLOW_STEADY) return "A steady run always uses SIMPLE; the algorithm is for transient runs";
    if (algo == ALG_PISO && sim_is_gas(s)) return "A compressible gas runs on rhoPimpleFoam: PIMPLE (one outer corrector is PISO's way)";
    if (algo == ALG_PISO && s->energy != EN_OFF) return "With heat on, buoyantPimpleFoam runs it: PIMPLE (one outer corrector is PISO's way)";
    return NULL;
}

const char *sim_energy_why_not(const SimSetup *s, int en)
{
    if (sim_is_gas(s) && en == EN_OFF) return "A compressible gas always carries its temperature: its density follows it and the pressure";
    if (sim_is_gas(s) && en == EN_BUOYANT) return "rhoSimpleFoam and rhoPimpleFoam leave gravity out. For warm gas rising, pick Incompressible "
                                                  "with buoyancy: air is an ideal gas there too";
    return NULL;
}

/* ---- turbulence and walls (plan step 5) */
const char *const WALLT_NAME[WT__N] = { "Automatic", "Wall functions", "Resolved" };
const char *const TI_NAME[TI__N] = { "Intensity and length", "Viscosity ratio", "Hydraulic diameter" };

static bool k_epsilon(int t) { return t == TURB_KEPSILON || t == TURB_REALIZABLE || t == TURB_RNG; }

int sim_wall_treatment(const SimSetup *s)
{
    if (s->turb == TURB_LAMINAR) return WT_RESOLVED;              /* no model: the wall is resolved or it is nothing */
    if (k_epsilon(s->turb)) return WT_FUNCTIONS;                   /* high-Re models: wall functions only */
    if (s->turb == TURB_SSTLM) return WT_RESOLVED;                 /* transition needs the viscous sublayer */
    if (s->wallTreat == WT_FUNCTIONS || s->wallTreat == WT_RESOLVED) return s->wallTreat;
    int a = sim_approach(s->turb);
    return a == APPR_LES || a == APPR_DES ? WT_RESOLVED : WT_FUNCTIONS;
}

const char *sim_wall_why_not(const SimSetup *s, int wt)
{
    if (s->turb == TURB_LAMINAR && wt != WT_AUTO) return "Laminar flow has no wall model: the mesh resolves the wall or it doesn't";
    if (k_epsilon(s->turb) && wt == WT_RESOLVED) return "k-epsilon models are made for wall functions. To resolve the wall, pick k-omega SST or Spalart-Allmaras";
    if (s->turb == TURB_SSTLM && wt == WT_FUNCTIONS) return "The transition model works out where the boundary layer turns turbulent: it needs the wall resolved (y+ about 1)";
    return NULL;
}

double sim_yplus_target(const SimSetup *s) { return sim_wall_treatment(s) == WT_RESOLVED ? 1 : 50; }

double sim_wall_spacing(const SimSetup *s, double U, double L, double yplus, double *firstH, double *delta, double *utau)
{
    if (!(U > 0) || !(L > 0) || !(s->nu > 0)) { if (firstH) *firstH = 0; if (delta) *delta = 0; if (utau) *utau = 0; return 0; }
    double Re = U * L / s->nu, Cf = 0.026 * pow(Re, -1.0 / 7), ut = U * sqrt(0.5 * Cf);
    if (utau) *utau = ut;
    if (firstH) *firstH = 2 * yplus * s->nu / ut;
    if (delta) *delta = Re < 5e5 ? 4.91 * L / sqrt(Re) : 0.37 * L * pow(Re, -0.2);
    return Re;
}

double sim_yplus_of(const SimSetup *s, double U, double L, double firstH)
{
    double ut = 0;
    if (!sim_wall_spacing(s, U, L, 1, NULL, NULL, &ut) || !(firstH > 0)) return 0;
    return 0.5 * firstH * ut / s->nu;
}

int sim_layer_count(double y1, double outer, double r, double *total)
{
    int n = 1;
    if (y1 > 0 && outer > y1) n = 1 + (int)ceil(log(outer / y1) / log(r > 1.0001 ? r : 1.0001));
    if (n < 1) n = 1;
    if (n > 60) n = 60;
    if (total) *total = r > 1.0001 ? y1 * (pow(r, n) - 1) / (r - 1) : y1 * n;
    return n;
}

void sim_inlet_turbulence(const SimSetup *s, const SimBoundary *b, double U, double *I, double *L)
{
    double i = b->intensity, l = b->length;
    if (b->turbSpec == TI_RATIO && b->ratio > 0) {
        /* nut = Cmu^1/4 k^1/2 L (the mixing length's k-omega and k-epsilon both give it): L from nut = ratio nu */
        double k = 1.5 * (U * i / 100) * (U * i / 100);
        l = k > 0 ? b->ratio * s->nu / (pow(0.09, 0.25) * sqrt(k)) : l;
    } else if (b->turbSpec == TI_DIAMETER) {
        double D = b->dh > 0 ? b->dh : b->area > 0 ? sqrt(4 * b->area / M_PI) : 0;
        if (D > 0 && s->nu > 0 && U > 0) { i = 16 * pow(U * D / s->nu, -0.125); l = 0.07 * D; }
    }
    if (I) *I = i;
    if (L) *L = l;
}

double sim_avg_start(const SimSetup *s) { return s->avgStart >= 0 ? s->avgStart : 0.4 * s->endTime; }

/* ---- a compressible gas (plan step 4) */
const char *const COMP_NAME[COMP__N] = { "Incompressible", "Ideal gas" };
const char *const GAS_NAME[GAS__N] = { "Air", "Nitrogen", "CO2", "Custom" };

bool sim_is_gas(const SimSetup *s) { return s->comp == COMP_GAS; }

/* molar mass (kg/kmol), cp / cv near room temperature, Sutherland's As (Pa s / K^½) and Ts (K): air and nitrogen as
   OpenFOAM's tutorials and White's Viscous Fluid Flow; CO2 from mu = 1.37e-5 Pa s at 273 K with S = 222 K */
void sim_gas_preset(SimSetup *s, int gas)
{
    static const double G[GAS_CUSTOM][4] = { { 28.96, 1.4, 1.458e-6, 110.4 }, { 28.013, 1.4, 1.407e-6, 111 }, { 44.01, 1.289, 1.503e-6, 222 } };
    if (gas < 0 || gas >= GAS__N) gas = GAS_AIR;
    s->gas = gas;
    if (gas < GAS_CUSTOM) { s->molW = G[gas][0]; s->gamma = G[gas][1]; s->muAs = G[gas][2]; s->muTs = G[gas][3]; }
    else if (!(s->molW > 0) || !(s->gamma > 1) || !(s->muAs > 0) || !(s->muTs >= 0)) { s->molW = 28.96; s->gamma = 1.4; s->muAs = 1.458e-6; s->muTs = 110.4; }
}

double sim_gas_mu(const SimSetup *s, double T) { return T > 0 ? s->muAs * sqrt(T) / (1 + s->muTs / T) : 0; }

/* the density, viscosity, cp and conductivity at the operating pressure (plus the starting gauge pressure) and the
   starting temperature: what the Reynolds number, the forces' rhoInf and the Nusselt number go by. The conductivity is
   the one OpenFOAM's sutherland transport works out, the modified Eucken: kappa = mu cv (1.32 + 1.77 R / cv) */
void sim_gas_props(SimSetup *s)
{
    if (!(s->molW > 0) || !(s->gamma > 1)) sim_gas_preset(s, GAS_AIR);
    double R = SIM_R / s->molW, T = s->T0 > 0 ? s->T0 : 293.15, p = sim_pabs(s, s->p0);
    double mu = sim_gas_mu(s, T), cp = s->gamma * R / (s->gamma - 1), cv = cp / s->gamma;
    s->rho = p > 0 ? p / (R * T) : 1.2;
    s->nu = s->rho > 0 ? mu / s->rho : 1.5e-5;
    s->cp = cp;
    s->kappa = mu * cv * (1.32 + 1.77 * R / cv);
    s->beta = 1 / T;
}

double sim_sound_speed(const SimSetup *s, double T)
{
    if (!(T > 0)) T = s->T0 > 0 ? s->T0 : 293.15;
    if (sim_is_gas(s)) return s->molW > 0 && s->gamma > 1 ? sqrt(s->gamma * SIM_R / s->molW * T) : 0;
    if (s->fluid == FLUID_AIR) return sqrt(1.4 * SIM_R / 28.96 * T);
    if (s->fluid == FLUID_WATER) return 1482;
    return 0;                                           /* a custom fluid: unknown */
}

double sim_pabs(const SimSetup *s, double gauge) { return (sim_is_gas(s) ? s->pOp : SIM_PATM) + gauge; }

bool sim_transonic(const SimSetup *s) { return sim_is_gas(s) && sim_mach(s) > 0.8; }

/* nothing rules a gas out yet; kept for the motion and multiphase models to come (VOF is incompressible) */
const char *sim_comp_why_not(const SimSetup *s, int comp) { (void)s; (void)comp; return NULL; }

void sim_set_comp(SimSetup *s, int comp)
{
    if (comp < 0 || comp >= COMP__N || comp == s->comp) return;
    s->comp = comp;
    if (comp == COMP_GAS) {
        if (s->gas < 0 || s->gas >= GAS__N) s->gas = GAS_AIR;
        sim_gas_preset(s, s->gas);
        if (!(s->pOp > 0)) s->pOp = SIM_POP;
        s->energy = EN_FORCED;
    } else {
        s->energy = EN_OFF;
        sim_fluid(s, s->fluid == FLUID_CUSTOM ? FLUID_AIR : s->fluid);   /* the gas's numbers were never a liquid's */
    }
    sim_settle(s);
}

void sim_set_energy(SimSetup *s, int en)
{
    if (en < 0 || en >= EN__N) return;
    s->energy = en;
    if (en != EN_OFF) s->heatReport = true;
    sim_settle(s);
}

/* air is an ideal gas: its expansion is 1 / T at the reference temperature, whatever was typed */
double sim_beta(const SimSetup *s) { return s->fluid == FLUID_AIR && s->Tref > 0 ? 1 / s->Tref : s->beta; }

double sim_nu_dT(const SimSetup *s) { return s->nuDT > 0 ? s->nuDT : sim_delta_T(s); }

bool sim_wall_heated(const SimBoundary *b) { return b->role == ROLE_WALL && b->heat != HT_ADIABATIC && !sim_constraint(b->meshType); }

double sim_g_mag(const SimSetup *s) { return sqrt(s->g[0] * s->g[0] + s->g[1] * s->g[1] + s->g[2] * s->g[2]); }

/* the span of the temperatures the start and the boundaries hold: fixed walls, inlets, convection's surroundings. A heat
   flux's wall temperature is not known before the run, so it adds nothing */
double sim_delta_T(const SimSetup *s)
{
    double lo = s->T0, hi = s->T0;
    for (int i = 0; i < s->nB; i++) {
        const SimBoundary *b = &s->b[i];
        double t = -1;
        if (sim_constraint(b->meshType)) continue;
        if (b->role == ROLE_INLET) t = b->T;
        else if (b->role == ROLE_WALL && b->heat == HT_FIXED) t = b->T;
        else if (b->role == ROLE_WALL && b->heat == HT_CONVECT) t = b->Ta;
        if (t > 0) { if (t < lo) lo = t; if (t > hi) hi = t; }
    }
    return hi - lo;
}

/* Ra = g beta dT L³ / (nu alpha), alpha = kappa / (rho cp) */
double sim_rayleigh(const SimSetup *s, double L)
{
    double a = s->rho > 0 && s->cp > 0 ? s->kappa / (s->rho * s->cp) : 0, dT = sim_delta_T(s);
    if (s->energy != EN_BUOYANT || !(a > 0) || !(s->nu > 0) || !(L > 0) || !(dT > 0)) return 0;
    return sim_g_mag(s) * sim_beta(s) * dT * L * L * L / (s->nu * a);
}

/* Ri = g beta dT L / U²: under about 0.1 the flow carries the heat (forced), over about 10 buoyancy drives it */
double sim_richardson(const SimSetup *s, double L)
{
    double U = 0, dT = sim_delta_T(s);
    for (int i = 0; i < s->nB; i++) if (s->b[i].role == ROLE_INLET && !sim_constraint(s->b[i].meshType)) U = sim_ref_speed(s);
    if (s->energy != EN_BUOYANT || !(U > 0) || !(L > 0) || !(dT > 0)) return 0;
    return sim_g_mag(s) * sim_beta(s) * dT * L / (U * U);
}

int sim_time_scheme(const SimSetup *s)
{
    if (s->timeScheme > TS_AUTO && s->timeScheme < TS__N) return s->timeScheme;
    int a = sim_approach(s->turb);
    return a == APPR_LES || a == APPR_DES ? TS_BACKWARD : TS_EULER;
}

const char *sim_app(const SimSetup *s)
{
    if (sim_is_gas(s)) return s->flow == FLOW_TRANSIENT ? "rhoPimpleFoam" : "rhoSimpleFoam";
    if (s->energy != EN_OFF) return s->flow == FLOW_TRANSIENT ? "buoyantPimpleFoam" : "buoyantSimpleFoam";
    if (s->flow != FLOW_TRANSIENT) return "simpleFoam";
    return s->algo == ALG_PISO ? "pisoFoam" : "pimpleFoam";
}

int sim_fields(const SimSetup *s, const char *out[SIM_MAX_F])
{
    int n = 0;
    out[n++] = "U";
    if (sim_is_gas(s)) { out[n++] = "p"; out[n++] = "T"; }        /* p absolute, solved; the density from p and T */
    else if (s->energy != EN_OFF) { out[n++] = "p_rgh"; out[n++] = "p"; out[n++] = "T"; }
    else out[n++] = "p";
    switch (s->turb) {
    case TURB_KOMEGASST: case TURB_SSTDDES: out[n++] = "k"; out[n++] = "omega"; break;
    case TURB_SSTLM: out[n++] = "k"; out[n++] = "omega"; out[n++] = "ReThetat"; out[n++] = "gammaInt"; break;
    case TURB_KEPSILON: case TURB_REALIZABLE: case TURB_RNG: out[n++] = "k"; out[n++] = "epsilon"; break;
    case TURB_SPALART: case TURB_SADDES: case TURB_SAIDDES: out[n++] = "nuTilda"; break;
    case TURB_DYNKEQN: out[n++] = "k"; break;
    default: break;                                     /* laminar; Smagorinsky and WALE work k out from the velocity */
    }
    if (s->turb != TURB_LAMINAR) out[n++] = "nut";
    if (s->turb != TURB_LAMINAR && s->energy != EN_OFF) out[n++] = "alphat";   /* the turbulence's heat diffusivity */
    return n;
}

/* ---- keeping the setup consistent */
static void clamp_int(int *v, int lo, int hi) { if (*v < lo) *v = lo; if (*v > hi) *v = hi; }

const char *sim_settle(SimSetup *s)
{
    static char msg[200];
    msg[0] = 0;
    if (!ok_turb(s->turb)) s->turb = TURB_KOMEGASST;
    if (s->algo < 0 || s->algo >= ALG__N) s->algo = ALG_PIMPLE;
    if (s->timeScheme < 0 || s->timeScheme >= TS__N) s->timeScheme = TS_AUTO;
    if (s->energy < 0 || s->energy >= EN__N) s->energy = EN_OFF;
    if (s->comp < 0 || s->comp >= COMP__N) s->comp = COMP_INCOMP;
    if (s->wallTreat < 0 || s->wallTreat >= WT__N) s->wallTreat = WT_AUTO;
    if (sim_is_gas(s)) {                                /* a gas carries its temperature, gravity off; rhoPimpleFoam is PIMPLE */
        s->energy = EN_FORCED;
        if (!(s->pOp > 0)) s->pOp = SIM_POP;
        sim_gas_props(s);
    }
    if (s->energy != EN_OFF && s->algo == ALG_PISO) s->algo = ALG_PIMPLE;   /* buoyantPimpleFoam: PIMPLE with one outer corrector */
    int a = sim_approach(s->turb);
    if (s->flow == FLOW_STEADY && (a == APPR_LES || a == APPR_DES)) {
        int was = s->turb;
        s->turb = M[was].rans;
        snprintf(msg, sizeof msg, "A steady run cannot follow eddies: %s became %s", M[was].name, M[s->turb].name);
    }
    clamp_int(&s->nCorr, 1, 10);
    clamp_int(&s->nOuter, 1, 100);
    if (s->algo == ALG_PISO) s->nOuter = 1;
    return msg[0] ? msg : NULL;
}

const char *sim_set_approach(SimSetup *s, int appr)
{
    static char msg[200];
    msg[0] = 0;
    if (appr < 0 || appr >= APPR__N || sim_approach(s->turb) == appr) return NULL;
    int cur = s->turb;
    if (appr == APPR_LAMINAR) s->turb = TURB_LAMINAR;
    else if (appr == APPR_RANS) s->turb = sim_approach(cur) == APPR_LAMINAR ? TURB_KOMEGASST : M[cur].rans;
    else if (appr == APPR_LES) s->turb = TURB_WALE;
    else s->turb = M[cur].rans == TURB_KOMEGASST && cur != TURB_LAMINAR && sim_approach(cur) == APPR_RANS ? TURB_SSTDDES : TURB_SADDES;
    if ((appr == APPR_LES || appr == APPR_DES) && s->flow == FLOW_STEADY) {
        s->flow = FLOW_TRANSIENT;
        snprintf(msg, sizeof msg, "%s needs a transient run: Time is now Transient", APPR_NAME[appr]);
    }
    sim_settle(s);
    return msg[0] ? msg : NULL;
}

void sim_set_algo(SimSetup *s, int algo)
{
    if (algo < 0 || algo >= ALG__N) return;
    s->algo = algo;
    if (algo == ALG_PISO) { s->nOuter = 1; s->nCorr = 2; if (s->maxCo > 1) s->maxCo = 1; }
    else if (algo == ALG_PIMPLE) { if (s->nOuter > 3) s->nOuter = 1; s->nCorr = 2; if (s->maxCo > 2) s->maxCo = 1; }
    else { if (s->nOuter < 5) s->nOuter = 20; s->nCorr = 2; if (s->maxCo < 2) s->maxCo = 5; s->adjustDt = true; }
    sim_settle(s);
}

/* ---- templates: what is being simulated */
const char *const TMPL_NAME[TMPL__N] = { "External aero", "Internal flow", "Vortex shedding", "Resolved turbulence", "Heat transfer",
                                         "Natural convection", "High-speed gas", "Free surface", "Rotating machinery", "Start blank" };

const char *sim_template_hint(int t)
{
    static const char *const H[TMPL__N] = {
        "Air over a wing, a car, a building. Steady, k-omega SST, lift and drag monitored.",
        "A pipe, a duct, a manifold. Steady, k-omega SST, a pressure outlet.",
        "Flow that keeps changing behind a body. Transient (URANS), k-omega SST, PIMPLE.",
        "The eddies themselves, on a fine 3-D mesh. Transient LES (WALE), PISO.",
        "Heated or cooled walls in a flow: forced convection. Steady, k-omega SST, temperature, heat through each wall.",
        "Flow driven by warm fluid rising: a heated room, a radiator. Steady, laminar, gravity on.",
        "Gas fast enough to compress, above about Mach 0.3: a nozzle, a fast wing. Steady, k-omega SST, ideal-gas air.",
        "Water and air with the surface between them: a dam break, a tank.",
        "A fan, a pump, a mixer: a rotating zone.",
        "Keep the defaults and choose every model yourself.",
    };
    return t >= 0 && t < TMPL__N ? H[t] : "";
}

const char *sim_template_why_not(const SimSetup *s, int t, bool twoD)
{
    (void)s;
    switch (t) {
    case TMPL_RESOLVED: return twoD ? "This case is 2-D: LES resolves eddies in 3-D, so it needs a 3-D mesh" : NULL;
    case TMPL_VOF: return "Free surface (VOF) comes in a later update of Q-Continuum";
    case TMPL_ROTATING: return "Rotating zones come in a later update of Q-Continuum";
    default: return t >= 0 && t < TMPL__N ? NULL : "";
    }
}

/* the transient run's length and step from the time the flow takes to cross the part: shedding needs some 20 crossings
   to settle, LES 10 (then averaging); the first step a Courant number about 1 on a cell a hundredth of the part */
static void transient_times(SimSetup *s, double size, double crossings, double perCross)
{
    double U = sim_ref_speed(s), tc = size > 0 && U > 0 ? size / U : 0;
    if (tc <= 0) return;
    s->endTime = crossings * tc;
    s->deltaT = tc / perCross;
    s->writeTime = s->endTime / 20;
}

void sim_template_apply(SimSetup *s, int t, double size)
{
    if (t < 0 || t >= TMPL__N || sim_template_why_not(s, t, false)) return;
    s->tmpl = t;
    if (t != TMPL_GAS && t != TMPL_BLANK && sim_is_gas(s)) sim_set_comp(s, COMP_INCOMP);   /* the other templates are incompressible */
    switch (t) {
    case TMPL_EXTERNAL:
        s->energy = EN_OFF;
        s->flow = FLOW_STEADY; s->turb = TURB_KOMEGASST; s->resStop = true;
        s->forces = true; s->forceAuto = true;
        break;
    case TMPL_INTERNAL:
        s->energy = EN_OFF;
        s->flow = FLOW_STEADY; s->turb = TURB_KOMEGASST; s->resStop = true;
        s->forces = false;
        break;
    case TMPL_SHEDDING:
        s->energy = EN_OFF;
        s->flow = FLOW_TRANSIENT; s->turb = TURB_KOMEGASST;
        sim_set_algo(s, ALG_PIMPLE); s->adjustDt = true; s->maxCo = 1; s->timeScheme = TS_AUTO;
        s->forces = true; s->forceAuto = true;
        transient_times(s, size, 20, 100);
        break;
    case TMPL_RESOLVED:
        s->flow = FLOW_TRANSIENT; s->turb = TURB_WALE; s->energy = EN_OFF;
        sim_set_algo(s, ALG_PISO); s->timeScheme = TS_AUTO;
        transient_times(s, size, 10, 200);
        break;
    case TMPL_HEAT:
        s->flow = FLOW_STEADY; s->turb = TURB_KOMEGASST; s->resStop = true;
        s->energy = EN_FORCED; s->heatReport = true; s->forces = false;
        break;
    case TMPL_NATCONV:
        /* laminar: a room-sized flow (Ra above about 1e9) is turbulent, and the Models step says so */
        s->flow = FLOW_STEADY; s->turb = TURB_LAMINAR; s->resStop = true;
        s->energy = EN_BUOYANT; s->heatReport = true; s->forces = false;
        s->U0[0] = s->U0[1] = s->U0[2] = 0;
        if (s->iterations < 2000) s->iterations = 2000;   /* buoyant flows settle slowly under SIMPLE's relaxation */
        break;
    case TMPL_GAS:
        /* rhoSimpleFoam settles more slowly than simpleFoam: its density and energy relax as well */
        s->flow = FLOW_STEADY; s->turb = TURB_KOMEGASST; s->resStop = true; s->forces = false;
        s->comp = COMP_INCOMP;                          /* so sim_set_comp sets the gas up afresh */
        sim_set_comp(s, COMP_GAS);
        if (s->iterations < 2000) s->iterations = 2000;
        break;
    default: break;                                     /* blank: the models as they are */
    }
    sim_settle(s);
}
