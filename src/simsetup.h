/* simsetup.h -- the whole simulation set up from choices, not files (user request, 2026-10-24): the flow (steady or
 * transient, incompressible), the fluid, the turbulence model, what each boundary is (an inlet at a speed, a flow rate
 * or a pressure, an outlet, a wall that sticks, slips or moves, a symmetry plane, an opening) with its values, the
 * starting values and the run controls, and heat (a temperature, with or without buoyancy). Kept in
 * <case>/simulation.ini; sim_write turns it into 0/, constant/ and system/ for simpleFoam, pimpleFoam or pisoFoam, or
 * with heat buoyantSimpleFoam or buoyantPimpleFoam, as a compressible gas rhoSimpleFoam or rhoPimpleFoam. The boundaries are the STL mesher's (their names and kinds come from it), the
 * wind tunnel's and any other patch the mesh has. */
#ifndef SIMSETUP_H
#define SIMSETUP_H

#include <stdbool.h>
#include "pl_util.h"

#define SIM_MAX_B 96

enum { FLOW_STEADY = 0, FLOW_TRANSIENT, FLOW__N };
/* the turbulence model; its approach (laminar, RANS, LES, DES: sim_approach) follows from it. New ones go at the end:
   simulation.ini keeps the model by name, the code by number */
enum { TURB_LAMINAR = 0, TURB_KOMEGASST, TURB_KEPSILON, TURB_REALIZABLE, TURB_SPALART,
       TURB_RNG, TURB_SSTLM,                              /* RANS: RNG k-epsilon, k-omega SST with transition (Langtry-Menter) */
       TURB_SMAGORINSKY, TURB_WALE, TURB_DYNKEQN,         /* LES                                                   */
       TURB_SADDES, TURB_SAIDDES, TURB_SSTDDES,           /* DES                                                   */
       TURB__N };
enum { APPR_LAMINAR = 0, APPR_RANS, APPR_LES, APPR_DES, APPR__N };
/* how a transient run steps: PISO (pisoFoam, small steps), PIMPLE (pimpleFoam), PIMPLE with many outer correctors
   (large steps, Courant 5 to 10) */
enum { ALG_PISO = 0, ALG_PIMPLE, ALG_LARGE, ALG__N };
enum { TS_AUTO = 0, TS_EULER, TS_BACKWARD, TS_CN, TS__N };
enum { FLUID_AIR = 0, FLUID_WATER, FLUID_CUSTOM, FLUID__N };
/* a boundary's role: the STL mesher's boundary types (BT_* in stlmesh.h), so they share one number */
enum { ROLE_WALL = 0, ROLE_OPENING, ROLE_INLET, ROLE_OUTLET, ROLE_SYMMETRY, ROLE__N };
/* IN_MACH last: a compressible gas's only (the menus of an incompressible setup stop before it) */
enum { IN_SPEED = 0, IN_VECTOR, IN_FLOW, IN_MASS, IN_PRESSURE, IN_MACH, IN__N };
enum { WL_NOSLIP = 0, WL_SLIP, WL_MOVING, WL__N };
/* heat (user plan step 3): off; temperature carried by the flow (forced convection, gravity off); with buoyancy (gravity
   on, warm fluid rises). Either runs buoyantSimpleFoam / buoyantPimpleFoam: v2606's incompressible solvers have no energy */
enum { EN_OFF = 0, EN_FORCED, EN_BUOYANT, EN__N };
/* a wall's heat: none crosses it, a fixed temperature, a heat flux (W/m²), a total heat rate (W), or convection to
   surroundings (h, ambient temperature) */
enum { HT_ADIABATIC = 0, HT_FIXED, HT_FLUX, HT_POWER, HT_CONVECT, HT__N };
#define SIM_PATM 1e5            /* with heat on, pressures are written absolute: this plus the gauge pressure, Pa */
/* compressibility (user plan step 4): one density (or a density that follows the temperature only, with heat), or an ideal
   gas whose density follows the pressure too (rhoSimpleFoam, rhoPimpleFoam: hePsiThermo, Sutherland's viscosity). A gas
   always carries its temperature, so its energy is EN_FORCED (gravity off) */
enum { COMP_INCOMP = 0, COMP_GAS, COMP__N };
enum { GAS_AIR = 0, GAS_N2, GAS_CO2, GAS_CUSTOM, GAS__N };
#define SIM_R 8314.462618       /* the gas constant, J/(kmol K)                               */
#define SIM_POP 101325          /* a gas's default operating pressure, Pa absolute            */
#define SIM_MAX_F 12            /* the most fields a setup writes (sim_fields)                */
/* the walls (plan step 5): wall functions (the first cell at y+ 30 to 300) or resolved (y+ about 1); automatic is resolved
   for LES, DES and the transition model, wall functions for the rest */
enum { WT_AUTO = 0, WT_FUNCTIONS, WT_RESOLVED, WT__N };
/* how an inlet's turbulence is given: an intensity and a mixing length; an intensity and a turbulent viscosity ratio
   (nut / nu); or a pipe's hydraulic diameter (intensity 0.16 Re^-1/8, length 0.07 D) */
enum { TI_LENGTH = 0, TI_RATIO, TI_DIAMETER, TI__N };

/* what is being simulated (Setup's first question, user plan step 2): a template sets the models and their values. Those
   past TMPL_NATCONV need models still to come (compressible, VOF, MRF) and are shown greyed */
enum { TMPL_EXTERNAL = 0, TMPL_INTERNAL, TMPL_SHEDDING, TMPL_RESOLVED, TMPL_HEAT, TMPL_NATCONV, TMPL_GAS, TMPL_VOF, TMPL_ROTATING,
       TMPL_BLANK, TMPL__N };

typedef struct {
    char   name[64];
    int    role;                /* ROLE_*                                                     */
    char   meshType[24];        /* the mesh patch's type: wall, patch, symmetry, empty, cyclic ... */
    bool   fromStl;             /* a boundary of the STL: its role is the STL mesher's type   */
    double area;                /* m², when known (the STL's), for the mean speed of a flow rate */
    int    inlet;               /* IN_*                                                       */
    double speed;               /* m/s, into the domain, normal to the boundary               */
    double vec[3];              /* m/s                                                        */
    double flow;                /* m³/s                                                       */
    double mdot;                /* kg/s                                                       */
    double p;                   /* Pa (gauge): inlet and opening total pressure, outlet static pressure */
    double intensity;           /* turbulence coming in: %                                    */
    double length;              /* ... and its mixing length, m                               */
    int    wall;                /* WL_*                                                       */
    double wallU[3];            /* a moving wall's velocity, m/s                              */
    int    force;               /* in the forces: -1 the default (sim_force_on), 0 no, 1 yes  */
    int    heat;                /* a wall's: HT_*                                             */
    double T;                   /* K: an inlet's, a fixed-temperature wall's, what flows back in at an outlet or opening */
    double q, Q;                /* a wall's heat flux into the fluid, W/m², and total heat rate, W */
    double h, Ta;               /* convection: W/(m² K), the surroundings' temperature K      */
    double mach;                /* a gas inlet by Mach number (its T the static temperature)  */
    bool   nonRefl;             /* a gas outlet that lets pressure waves out (waveTransmissive, transient only) */
    int    turbSpec;            /* TI_*: an inlet's (and a free stream's) turbulence          */
    double ratio;               /* ... its turbulent viscosity ratio nut / nu                 */
    double dh;                  /* ... its hydraulic diameter, m (0: worked out from its area) */
    bool   farField;            /* an opening that is the free stream far from a body: vec its velocity, p its static
                                   pressure, T its temperature (freestreamVelocity, freestreamPressure); not with heat
                                   on an incompressible setup (sim_far_field) */
} SimBoundary;

/* the force coefficients' reference (forceCoeffs): Cd = drag / (½ rho Uinf² Aref), Cl the same across, Cm over lRef */
typedef struct SimForceRef {
    double drag[3], lift[3];    /* unit: along the free stream, and square to it (up)         */
    double CofR[3];             /* moments about this point, m                                */
    double Uinf, lRef, aRef;    /* m/s (0: the inflow speed), m, m²                           */
} SimForceRef;

typedef struct {
    int    tmpl;                /* TMPL_*, -1: none chosen yet (no simulation.ini): Setup asks first */
    int    flow, turb, fluid;
    double rho, nu;             /* kg/m³, m²/s; a gas's at the operating pressure and the starting temperature (sim_settle) */
    int    comp;                /* COMP_*                                                     */
    int    gas;                 /* GAS_*: the gas's molar mass, ratio of specific heats and Sutherland coefficients */
    double molW, gamma;         /* kg/kmol, cp / cv                                           */
    double muAs, muTs;          /* Sutherland: mu = As sqrt(T) / (1 + Ts / T), Pa s / K^½ and K */
    double pOp;                 /* a gas's operating pressure, Pa absolute: the gauge pressures are above it */
    int    energy;              /* EN_*                                                       */
    double cp, kappa, beta;     /* J/(kg K), W/(m K), 1/K (thermal expansion, for buoyancy)   */
    double Tref, T0;            /* K: where the density is rho (buoyancy), and the starting temperature */
    double g[3];                /* m/s², buoyancy only                                        */
    bool   gAuto;               /* g from the STL mesher's up direction, not typed in         */
    bool   heatReport;          /* the heat through each wall monitored (wallHeatFlux)        */
    /* the Nusselt number of each heated or cooled wall, Nu = q L / (kappa dT) with q its mean heat flux: the length, m
       (auto: the setup page keeps it at the mesh's height, or the part's size), and the temperature difference, K (0: the
       span the boundaries set, sim_delta_T) */
    bool   nuAuto;
    double nuLength, nuDT;
    double U0[3], p0;           /* the starting velocity (m/s) and pressure (Pa, gauge)       */
    double intensity, length;   /* the starting and default inlet turbulence: %, m            */
    double iterations, writeEvery;                    /* steady: SIMPLE iterations             */
    bool   resStop;             /* steady: stop early once every initial residual is under its target (residualControl) */
    double resP, resRest;       /* the targets: p, and U and the turbulence fields             */
    double endTime, deltaT, maxCo, writeTime;         /* transient: s, s, Courant, s           */
    bool   adjustDt;
    int    algo;                /* transient: ALG_*                                           */
    int    nOuter, nCorr;       /* transient: PIMPLE's outer correctors, the pressure correctors */
    int    timeScheme;          /* transient: TS_* (auto: backward for LES and DES, else Euler) */
    int    wallTreat;           /* WT_*                                                       */
    double avgStart;            /* LES and DES: the time the averages (UMean ...) start, s; < 0: automatic (sim_avg_start) */
    bool   forces;              /* lift and drag monitored: forces and forceCoeffs function objects */
    bool   forceAuto;           /* the reference from the part and the wind tunnel (stlmesh_force_ref), not typed in */
    SimForceRef fr;
    int    nB;
    SimBoundary b[SIM_MAX_B];
} SimSetup;

extern const char *const FLOW_NAME[FLOW__N];
extern const char *const TURB_NAME[TURB__N];
extern const char *const APPR_NAME[APPR__N];       /* "Laminar", "RANS", "LES", "DES" */
extern const char *const ALG_NAME[ALG__N];
extern const char *const TS_NAME[TS__N];
extern const char *const FLUID_NAME[FLUID__N];
extern const char *const ROLE_NAME[ROLE__N];
extern const char *const INLET_NAME[IN__N];
extern const char *const WALL_NAME[WL__N];
extern const char *const ENERGY_NAME[EN__N];
extern const char *const HEAT_NAME[HT__N];
extern const char *const COMP_NAME[COMP__N];
extern const char *const WALLT_NAME[WT__N];
extern const char *const TI_NAME[TI__N];
extern const char *const GAS_NAME[GAS__N];

void sim_defaults(SimSetup *s, double size);       /* size: the part's length, m (sets the turbulence length) */
void sim_fluid(SimSetup *s, int fluid);            /* the fluid's density and viscosity (custom keeps them) */
void sim_boundary_defaults(const SimSetup *s, SimBoundary *b);   /* the values for b->role */
bool sim_load(const char *casePath, SimSetup *s, double size);   /* false: no simulation.ini (s holds the defaults) */
bool sim_save(const char *casePath, const SimSetup *s);
void sim_ini_path(const char *casePath, char *out, size_t cap);

/* the boundaries are exactly these (names, roles, mesh types, from the STL, areas): an entry kept by name keeps its
   values; a new one starts from its role's defaults. Roles of entries not from the STL stay as the user set them. */
typedef struct SimPatch { const char *name; int role; const char *meshType; bool fromStl; double area; } SimPatch;
void sim_set_boundaries(SimSetup *s, const SimPatch *p, int n);
SimBoundary *sim_find(SimSetup *s, const char *name);
void sim_rename(SimSetup *s, const char *from, const char *to);

double sim_ref_speed(const SimSetup *s);           /* the inflow speed, for the turbulence and the time step */
void   sim_turbulence(const SimSetup *s, double U, double I, double L, double *k, double *omega, double *eps, double *nuTilda);
/* the fields of 0/: U, p (with energy p_rgh, p, T), the turbulence's, nut (and alphat with energy) */
int    sim_fields(const SimSetup *s, const char *out[SIM_MAX_F]);
const char *sim_app(const SimSetup *s);            /* simpleFoam / pimpleFoam / pisoFoam / buoyantSimpleFoam / buoyantPimpleFoam */
/* heat: the temperature span the boundaries set (K; 0: none), and from it the Rayleigh number over the length L, m
   (0: no buoyancy or no span) and the Richardson number (buoyancy over inertia at the inflow speed; 0: no inflow) */
double sim_delta_T(const SimSetup *s);
double sim_rayleigh(const SimSetup *s, double L);
double sim_richardson(const SimSetup *s, double L);
double sim_g_mag(const SimSetup *s);
double sim_beta(const SimSetup *s);              /* the thermal expansion in effect: air's is 1 / Tref (an ideal gas) */
double sim_nu_dT(const SimSetup *s);             /* the Nusselt number's temperature difference, K (0: none known)   */
/* a compressible gas (plan step 4) */
bool   sim_is_gas(const SimSetup *s);
void   sim_gas_preset(SimSetup *s, int gas);     /* the gas's molar mass, gamma and Sutherland's (custom keeps them) */
void   sim_gas_props(SimSetup *s);               /* rho, nu, cp, kappa at pOp and T0, from the gas's               */
double sim_gas_mu(const SimSetup *s, double T);  /* Sutherland's viscosity, Pa s                                    */
double sim_sound_speed(const SimSetup *s, double T);   /* sqrt(gamma R T / M), m/s; air's for an incompressible setup */
double sim_pabs(const SimSetup *s, double gauge);      /* the absolute pressure written for a gauge one, Pa       */
/* the fastest Mach number the setup brings in: the inlets' (by speed at their temperature, by Mach, by total pressure
   over the outlet's) and the start's; 0: none or a liquid */
double sim_mach(const SimSetup *s);
bool   sim_transonic(const SimSetup *s);           /* above Mach 0.8: the pressure equation takes the transonic form */
void   sim_set_comp(SimSetup *s, int comp);        /* and the energy (a gas always carries T), the algorithm, the properties */
const char *sim_comp_why_not(const SimSetup *s, int comp);
bool   sim_wall_heated(const SimBoundary *b);    /* a wall that passes heat (not adiabatic, not a constraint)        */
bool   sim_far_field(const SimSetup *s, const SimBoundary *b);   /* an opening written as the free stream           */

/* ---- turbulence and walls (plan step 5) */
int    sim_wall_treatment(const SimSetup *s);      /* WT_FUNCTIONS or WT_RESOLVED in effect (auto resolved)            */
const char *sim_wall_why_not(const SimSetup *s, int wt);   /* k-epsilon models can't resolve the wall; laminar has none */
double sim_yplus_target(const SimSetup *s);        /* the y+ the treatment wants at the first cell's centre: 50 or 1   */
/* a flat plate's turbulent boundary layer at speed U (m/s) over length L (m): the friction velocity (Cf = 0.026 Re^-1/7),
   the first cell's height (twice its centre's distance) for y+, and the layer's thickness 0.37 L Re^-1/5 (laminar below
   Re 5e5: 4.91 L Re^-1/2). Returns the Reynolds number (0: no speed, length or viscosity) */
double sim_wall_spacing(const SimSetup *s, double U, double L, double yplus, double *firstH, double *delta, double *utau);
double sim_yplus_of(const SimSetup *s, double U, double L, double firstH);   /* the inverse: the y+ a first cell gives  */
/* prism layers from the first height y1 growing by r until the last is about `outer` thick: their count (1 to 60) and
   total thickness */
int    sim_layer_count(double y1, double outer, double r, double *total);
/* an inlet's turbulence as an intensity (%) and a mixing length (m) at speed U, whichever way it was given */
void   sim_inlet_turbulence(const SimSetup *s, const SimBoundary *b, double U, double *I, double *L);
double sim_avg_start(const SimSetup *s);           /* the averaging start in effect: typed, else 40 % of the end time  */

/* ---- the models and what goes with what (simmodels.c) */
int    sim_approach(int turb);                     /* APPR_* of a model                                     */
const char *sim_approach_label(const SimSetup *s, int appr);   /* "RANS", or "URANS" on a transient run       */
int    sim_approach_models(int appr, int out[TURB__N]);        /* the models of an approach, the default first */
const char *sim_model_word(int turb);              /* its OpenFOAM name: kOmegaSST, WALE, SpalartAllmarasDDES ... */
const char *sim_model_hint(int turb);              /* one line: what it is for                               */
/* NULL when the choice suits the setup, else why not ("LES needs a transient run") */
const char *sim_approach_why_not(const SimSetup *s, int appr);
const char *sim_algo_why_not(const SimSetup *s, int algo);
const char *sim_energy_why_not(const SimSetup *s, int en);
void   sim_set_energy(SimSetup *s, int en);         /* and the algorithm (PISO: PIMPLE) and gravity's default */
extern const char *const TMPL_NAME[TMPL__N];
const char *sim_template_hint(int t);              /* one line: what it sets                                  */
/* NULL when the template can be used here, else why not; twoD: the case is (or will be meshed) 2-D */
const char *sim_template_why_not(const SimSetup *s, int t, bool twoD);
/* the template's models and run controls (boundaries kept); size: the part's length, m (0: unknown) */
void   sim_template_apply(SimSetup *s, int t, double size);
bool   sim_two_d(const SimSetup *s);               /* the mesh has empty patches                             */
int    sim_time_scheme(const SimSetup *s);         /* TS_* in effect (TS_AUTO resolved)                      */
/* the setup made consistent after one choice changed: a steady run takes a LES or DES model's RANS counterpart, the
   algorithm's correctors and Courant limit move into its range. Returns a line saying what else changed, or NULL */
const char *sim_settle(SimSetup *s);
const char *sim_set_approach(SimSetup *s, int appr);   /* that approach's default model (the matching one where there is); a line when the time changed too */
void   sim_set_algo(SimSetup *s, int algo);        /* and its correctors and Courant limit                   */
bool   sim_constraint(const char *meshType);       /* empty, wedge, cyclic ...: every field takes the patch's type */
bool   sim_force_on(const SimSetup *s, const SimBoundary *b);   /* the boundary's forces count (the default: the part's walls) */
double sim_force_speed(const SimSetup *s);         /* fr.Uinf, else the inflow speed */
/* what will not work or is likely wrong (no way out for the flow, nothing coming in): one line each, returns count */
int    sim_check(const SimSetup *s, char *out, int cap);
/* the Setup step line i of the last sim_check is about */
enum { SA_MODELS = 0, SA_MATERIALS, SA_BOUNDARIES, SA_START, SA_REPORTS, SA_SOLVER };
int    sim_check_area(int i);

/* every file sim_write may write or remove, case-relative (NULL-terminated) */
extern const char *const SIM_FILES[];
/* 0/<fields>, constant/transportProperties and turbulenceProperties, system/controlDict, fvSchemes, fvSolution. A file
   that wasn't ours keeps its old text as <file>.orig (once); our old turbulence fields the new model doesn't solve go. */
bool sim_write(const char *casePath, const SimSetup *s, char *err, int cap);
/* after sim_write: the hand edits it kept, and the ones it had to replace (the setup changed the same setting), one per
   line ("system/fvSchemes: divSchemes div(phi,U)"); empty when there were none */
const char *sim_write_kept(void);
const char *sim_write_replaced(void);
/* the run controls of our system/controlDict as edited since (Run page, Files, an editor: end time, time step, write
   interval, Courant limit) taken into s, so writing again keeps them; true if any changed. Only with a simulation.ini and
   the setup's solver as the application (a tutorial's own controlDict, or one for another solver: false) */
bool sim_take_controls(const char *casePath, SimSetup *s);

#endif
