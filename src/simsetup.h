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
    int    phaseIn;             /* free surface: what an inlet brings in, PI_*                 */
    bool   farField;            /* an opening that is the free stream far from a body: vec its velocity, p its static
                                   pressure, T its temperature (freestreamVelocity, freestreamPressure); not with heat
                                   on an incompressible setup (sim_far_field) */
} SimBoundary;

/* free surface (plan step 8): two phases, water and air, and the surface between them (VOF, interFoam): alpha.water is
   the share of water in a cell. The water at the start: boxes, or all below a height (setFields, run before the solver) */
enum { PH_SINGLE = 0, PH_VOF, PH__N };
enum { PI_WATER = 0, PI_AIR, PI_EITHER, PI__N };
enum { FL_BOX = 0, FL_BELOW, FL_ZONE, FL__N };          /* FL_ZONE: a zone's cells (plan step 9) */
enum { LIQ_WATER = 0, LIQ_SEAWATER, LIQ_OIL, LIQ_CUSTOM, LIQ__N };
#define SIM_MAX_FILL 4
typedef struct {
    int    kind;                /* FL_*                                                       */
    double a[3], b[3];          /* a box's least and greatest corners, m                      */
    double h;                   /* below this height (along up, against gravity), m           */
    /* plan step 9: a region of a single fluid starts at its own temperature or velocity (a hot block, a jet), of a free
       surface it is water; FL_ZONE: the cells of a zone of the Zones step or of the mesh (zoneToCell) */
    char   zone[48];
    bool   setT, setU;
    double T, U[3];             /* K, m/s                                                     */
} SimFill;

/* starting values (plan step 9): uniform (the values typed in), a potential-flow solution (potentialFoam, run before the
   solver: a flow that already goes round the part), or another case's last results (mapFields, interpolated onto this mesh:
   the boundaries keep this setup's conditions) */
enum { SV_UNIFORM = 0, SV_POTENTIAL, SV_MAP, SV__N };
/* the numerics (plan step 9): schemes, linear solvers and relaxation set together. Robust: first order, firmly relaxed
   SIMPLE (gets a hard case going); Balanced: second order for the velocity, SIMPLEC (the default); Accurate: second order
   everywhere, little limiting (a good mesh) */
enum { NUM_ROBUST = 0, NUM_BALANCED, NUM_ACCURATE, NUM__N };

/* reports (user plan step 6): what is measured while the case runs, each a function object in system/controlDict whose
   postProcessing/<name> file the Run page shows as a monitor. A probe: values at points; a surface: an average (area or
   mass weighted), the flow rate, the least or greatest value on a boundary; a pressure drop: the area-averaged pressure
   of one boundary minus another's; a volume: the average, least or greatest value in the whole domain */
enum { RP_PROBE = 0, RP_SURFACE, RP_DROP, RP_VOLUME, RP__N };
enum { SO_AVG = 0, SO_MASSAVG, SO_FLOW, SO_MIN, SO_MAX, SO__N };
enum { VO_AVG = 0, VO_MIN, VO_MAX, VO__N };
#define SIM_MAX_R 24
#define SIM_MAX_PROBES 16
typedef struct {
    int    kind;                /* RP_*                                                       */
    char   name[48];            /* the function object's name, and its postProcessing folder  */
    char   field[32];           /* U (reported as the speed |U|), p (in Pa), T, k ...; not for a flow rate */
    char   patch[64], patch2[64];   /* a surface's boundary; a pressure drop's from and to    */
    int    op;                  /* SO_* (surface) or VO_* (volume)                            */
    int    nPts;
    double pts[SIM_MAX_PROBES][3];   /* a probe's points, m                                   */
} SimReport;

/* zones (plan step 7): cells marked by a shape (a cylinder, box or sphere: topoSet, run before the solver) or a cellZone
   the mesh has, that rotate (a moving reference frame, MRF: the flow in the zone is solved in its turning frame, its
   walls turn with it) or resist the flow (a porous medium, Darcy-Forchheimer: dp/L = mu d U + rho f U² / 2) */
enum { ZN_ROTATE = 0, ZN_POROUS, ZN__N };
enum { ZS_CYLINDER = 0, ZS_BOX, ZS_SPHERE, ZS_MESH, ZS__N };
enum { PD_ISO = 0, PD_X, PD_Y, PD_Z, PD__N };   /* porous: the same resistance every way, or the flow passes along one axis */
#define SIM_MAX_Z 8
typedef struct {
    int    kind;                /* ZN_*                                                       */
    char   name[48];            /* the cellZone's name (and the MRF or porosity entry's)      */
    int    shape;               /* ZS_*                                                       */
    char   zone[64];            /* ZS_MESH: the mesh's cellZone                               */
    double a[3], b[3], r;       /* cylinder: the end centres and radius; box: least and greatest corners; sphere: centre, radius; m */
    /* rotating */
    double axis[3], origin[3];  /* the axis it turns about (right hand) and a point on it     */
    bool   axisAuto;            /* cylinder: the axis and origin are the cylinder's            */
    double rpm;                 /* revolutions per minute                                     */
    char   still[256];          /* walls in the zone that stay still (nonRotatingPatches), space separated */
    char   torque[256];         /* walls whose torque about the axis is reported, space separated ("": none) */
    /* porous */
    int    pdir;                /* PD_*: across it the resistance is 1000 times                */
    bool   coeffs;              /* d and f typed in, not fitted                                */
    double U1, dp1, U2, dp2;    /* pressure drops (Pa) at speeds (m/s) through the thickness; U2 0: one point (inertial only) */
    double thick;               /* the thickness those drops were measured over, m            */
    double d, f;                /* Darcy (1/m²) and Forchheimer (1/m) coefficients            */
} SimZone;

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
    int    nR;
    SimReport r[SIM_MAX_R];
    int    nZ;
    SimZone z[SIM_MAX_Z];
    /* free surface (plan step 8) */
    int    phase;               /* PH_*                                                       */
    int    liquid;              /* LIQ_*: the water phase's preset (custom keeps its values)  */
    double rhoL, nuL, rhoG, nuG;   /* the liquid's ("water") and the gas's ("air") density and viscosity */
    double sigma;               /* surface tension, N/m                                       */
    int    nFill;
    SimFill fill[SIM_MAX_FILL];
    double Uw[3];               /* the starting water's velocity, m/s                         */
    double maxAlphaCo;          /* the interface's Courant limit                              */
    int    nAlphaCorr, nAlphaSub;   /* MULES: alpha correctors, sub-cycles                   */
    double cAlpha;              /* interface compression                                      */
    bool   iso;                 /* interIsoFoam (geometric isoAdvector): a sharper surface    */
    /* plan step 9 */
    int    start;               /* SV_*                                                       */
    char   mapFrom[512];        /* SV_MAP: the other case's folder                            */
    double ramp;                /* steady: the inlets rise from a tenth to their value over this many iterations (0: off) */
    int    numerics;            /* NUM_*                                                      */
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
extern const char *const REPORT_NAME[RP__N];     /* "Probe points", "Surface", "Pressure drop", "Volume" */
extern const char *const SURF_OP_NAME[SO__N];
extern const char *const VOL_OP_NAME[VO__N];
extern const char *const TI_NAME[TI__N];
extern const char *const GAS_NAME[GAS__N];
extern const char *const START_NAME[SV__N];      /* "Uniform", "Potential flow", "Another case's results" */
extern const char *const NUM_NAME[NUM__N];       /* "Robust", "Balanced", "Accurate" */

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

/* ---- reports (plan step 6, simsetup.c) */
/* a new report of that kind with defaults (a probe at centre; a surface on the outlet; a drop from the inlet to the
   outlet; the domain's greatest speed): its index, -1 when the list is full */
int    sim_report_add(SimSetup *s, int kind, const double centre[3]);
void   sim_report_remove(SimSetup *s, int i);
/* the fields a report can measure, as OpenFOAM names them in this setup (U, p, T, k ...; no turbulence viscosity) */
int    sim_report_fields(const SimSetup *s, const char *out[SIM_MAX_F]);
/* what the report's numbers are in: "m/s", "Pa", "K", "m³/s" or "kg/s" (a flow rate: the mass flux with heat or a gas) */
const char *sim_report_unit(const SimSetup *s, const SimReport *r);
/* one line saying what it measures: "Speed at 2 points", "Pressure drop from inlet to outlet" */
void   sim_report_describe(const SimSetup *s, const SimReport *r, char *out, int cap);
/* the field name the function objects measure: mag(U) for U, pPa (the static pressure in Pa) for an incompressible
   setup's p, else the field itself */
const char *sim_report_of(const SimSetup *s, const char *field);

/* ---- zones (plan step 7, simsetup.c) */
extern const char *const ZONE_KIND_NAME[ZN__N];  /* "Rotating zone", "Porous zone" */
extern const char *const ZONE_SHAPE_NAME[ZS__N]; /* "Cylinder", "Box", "Sphere", "Mesh cell zone" */
extern const char *const PDIR_NAME[PD__N];
/* a new zone of that kind: a cylinder (rotating) or box (porous) a fifth of the mesh's size about its centre, or the mesh's
   first cellZone when it has one (meshZone, NULL: none); its index, -1 when the list is full */
int    sim_zone_add(SimSetup *s, int kind, const double lo[3], const double hi[3], const char *meshZone);
void   sim_zone_remove(SimSetup *s, int i);
/* the rotation's axis (unit) and origin in effect: the cylinder's own when axisAuto */
void   sim_zone_axis(const SimZone *z, double axis[3], double origin[3]);
double sim_zone_omega(const SimZone *z);           /* rad/s                                                  */
/* the porous coefficients in effect: typed, or fitted to the pressure drops (two points: both; one: inertial only).
   Returns NULL, or why the fit is off (a drop that falls with speed: a coefficient came out below zero, set to 0) */
const char *sim_zone_porous(const SimSetup *s, const SimZone *z, double *d, double *f);
/* the pressure drop through the zone's thickness at speed U with those coefficients, Pa */
double sim_zone_drop(const SimSetup *s, const SimZone *z, double U);
bool   sim_has_rotation(const SimSetup *s);
/* the cellZone names of a case's mesh (constant/polyMesh/cellZones), up to max; returns the count */
int    sim_mesh_cell_zones(const char *casePath, char (*out)[64], int max);

/* ---- free surface (plan step 8, simsetup.c) */
extern const char *const PHASE_NAME[PH__N];       /* "Single fluid", "Free surface (water and air)" */
extern const char *const PHASEIN_NAME[PI__N];     /* "Water", "Air", "Either" */
extern const char *const FILL_NAME[FL__N];        /* "Box", "Below a height" */
extern const char *const LIQ_NAME[LIQ__N];
bool   sim_is_vof(const SimSetup *s);
void   sim_liquid(SimSetup *s, int liq);          /* the preset's density and viscosity (custom keeps them) */
void   sim_set_phase(SimSetup *s, int ph);        /* and what goes with it: transient, PIMPLE, no heat, gravity; returns via sim_settle */
const char *sim_phase_why_not(const SimSetup *s, int ph);
int    sim_up_axis(const SimSetup *s);            /* the axis most against gravity (0 x, 1 y, 2 z) */
/* the water's volume at the start, m³ (boxes clipped to the mesh's box lo..hi, overlaps counted twice) */
double sim_fill_volume(const SimSetup *s, const double lo[3], const double hi[3]);

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
/* starting values (plan step 9) */
const char *sim_start_why_not(const SimSetup *s, int sv);   /* NULL: it can start that way */
bool   sim_map_source(const char *src, char *time, size_t tcap, char *why, size_t wcap);   /* a case with results to start from: its latest time */
/* the Run page's "Starting values" step for the case's simulation.ini: the shell command (potentialFoam ..., mapFields ...),
   false when it starts uniform or has no setup. The fields of 0/ as the setup wrote them are kept first (in .qc-setup/0.before),
   so writing the setup again merges with them, not with the tool's output */
bool   sim_start_command(const char *casePath, char *label, size_t lcap, char *cmd, size_t ccap);
const char *sim_keep_fields_cmd(void);             /* the shell line that keeps them (the setFields step's too) */
bool   sim_regions_set(const SimSetup *s);         /* a single fluid's regions set a temperature or velocity (setFields) */
/* the numerics preset's files edited by hand since the setup wrote them (the preset then shows as Custom), and back to the preset */
bool   sim_numerics_edited(const char *casePath);
int    sim_numerics_of(const char *casePath);       /* NUM_* the setup wrote system/fvSchemes with, -1: not the setup's */
void   sim_numerics_reset(const char *casePath);   /* the hand edits to system/fvSchemes and fvSolution dropped: the next write is the preset's */
int    sim_check(const SimSetup *s, char *out, int cap);
/* the Setup step line i of the last sim_check is about */
enum { SA_MODELS = 0, SA_MATERIALS, SA_BOUNDARIES, SA_ZONES, SA_START, SA_REPORTS, SA_SOLVER };
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
