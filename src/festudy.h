/* festudy.h -- a part study for CalculiX (docs/FEA_PLAN.md 2026-11-15; v2 improvement plan B1, 2026-11-26): the part and
 * how it is meshed, the material, what is asked (the analysis), the supports, the loads and the thermal conditions, each
 * on faces of the part (FeMesh groups, by name), kept in <project>/study.ini; and the CalculiX input written from them:
 * study_mesh.inp (nodes, elements, node sets) and study.inp (material, steps, supports, loads).
 *
 * Units in study.ini are the user's: mm, N, MPa, N m (torque), m/s² (gravity), rpm, K, W, W/m², W/(m² K), Hz, s.
 * The deck is in mm, N, MPa, tonnes/mm³, s, K, mJ, mW (CalculiX takes any consistent set). Every keyword is from the
 * CalculiX 2.21 test decks (beamb buckling, beamdy* frequency / modal dynamic / steady state dynamics, beamheatplas
 * coupled temperature-displacement and plasticity, axrad radiation, the *FILM and *DFLUX decks). */
#ifndef FESTUDY_H
#define FESTUDY_H
#include <stdbool.h>
#include "femesh.h"

#define FE_MAXITEM 24
#define FE_MAXFACES 64
/* what is asked */
enum { AN_STATIC = 0,       /* linear static: stress and deflection                                     */
       AN_NONLINEAR,        /* large deflection (NLGEOM) and yielding (bilinear plasticity), in increments */
       AN_FREQUENCY,        /* natural frequencies and mode shapes                                      */
       AN_BUCKLE,           /* buckling load factors                                                    */
       AN_HEAT,             /* heat conduction (steady, or transient from a starting temperature)       */
       AN_THERMSTRESS,      /* temperature and the stress it causes (coupled)                           */
       AN_DYNAMIC,          /* a load in time (step, ramp, pulse): the response, by modes               */
       AN_HARMONIC,         /* a load shaking at a frequency: the response over a frequency range       */
       AN__N };
/* supports */
enum { FX_FIXED = 0, FX_X, FX_Y, FX_Z,     /* held in x, y and z; in one axis only                               */
       FX_ROLLER,                          /* held normal to a flat face only: slides along it (a symmetry plane) */
       FX_PIN,                             /* a cylindrical face held round and along its axis, free to turn     */
       FX_DISP,                            /* moved a given distance (mm) along dir                              */
       FX__N };
/* loads */
enum { LD_FORCE = 0, LD_PRESSURE,          /* N over the faces (normal or an axis); MPa                         */
       LD_GRAVITY,                         /* the part's weight: value m/s² along dir (no faces)                */
       LD_ROTATION,                        /* spinning: value rpm about the axis dir through point pt (no faces) */
       LD_BEARING,                         /* a pin or shaft pushing on a hole: total N along dir, cosine-spread */
       LD_TORQUE,                          /* a twist about a cylindrical face's axis: N m                      */
       LD__N };
enum { DIR_NORMAL = 0, DIR_PX, DIR_PY, DIR_PZ, DIR_MX, DIR_MY, DIR_MZ, DIR__N };   /* normal: pushing into the face */
/* thermal conditions */
enum { TH_TEMP = 0,                        /* faces held at T (K)                                               */
       TH_FLUX,                            /* heat in through faces, W/m²                                       */
       TH_CONVECT,                         /* convection to surroundings: h W/(m² K), Ta K                      */
       TH_RADIATE,                         /* radiation to surroundings: emissivity, Ta K                       */
       TH_HEATGEN,                         /* heat made inside the part, W in all (no faces)                    */
       TH__N };
/* how a dynamic load goes in time */
enum { AMP_STEP = 0, AMP_RAMP, AMP_PULSE, AMP__N };

typedef struct { int kind; int nf; char face[FE_MAXFACES][64]; double value; int dir; } FeFixture;   /* value: FX_DISP mm */
typedef struct { int kind; int nf; char face[FE_MAXFACES][64]; double value; int dir; double pt[3]; } FeLoad;
typedef struct { int kind; int nf; char face[FE_MAXFACES][64]; double value, value2; } FeThermal;  /* value2: Ta */
/* plan B3: finer elements on picked faces (a fillet, a hole, a thin wall), size mm */
#define FE_MAXREF 8
typedef struct { int nf; char face[FE_MAXFACES][64]; double size; } FeRefine;
/* the material: a library name, or the library's values changed (custom) */
typedef struct { double E, nu, rho, yield, uts, elong, k, cp, alpha; } FeMatProps;   /* SI: Pa, kg/m³, W/(m K), J/(kg K), 1/K */

typedef struct {
    char   part[256];          /* the part's file, in the project folder                         */
    double scale;              /* mm per unit of a file without one (STL, BREP, MED, UNV)        */
    double size;               /* element size, mm (0: a twentieth of the part)                  */
    int    order;              /* 2 quadratic, 1 linear                                          */
    char   material[64];       /* a CCX_LIB name                                                 */
    bool   custom;             /* props below replace the library's                              */
    FeMatProps props;
    int    analysis;           /* AN_*                                                           */
    bool   transient;          /* heat, thermal stress: in time from T0 (else steady)            */
    double time, inc;          /* nonlinear: increments of a unit load path (inc the first, 0: auto); dynamic,
                                  transient heat: the time span and first step (s)              */
    int    modes;              /* frequency, dynamic, harmonic: modes computed                   */
    double fLo, fHi; int nPts; /* harmonic: the frequency range (Hz) and its points              */
    double damping;            /* dynamic, harmonic: modal damping ratio (0.02 = 2 %)            */
    int    amp; double ampT;   /* dynamic: the load's shape in time and its rise or pulse time (s) */
    double T0;                 /* starting temperature, and the stress-free one, K              */
    int    nFix, nLoad, nTh, nRef;
    FeRefine  ref[FE_MAXREF];
    FeFixture fix[FE_MAXITEM];
    FeLoad    load[FE_MAXITEM];
    FeThermal th[FE_MAXITEM];
} FeStudy;

extern const char *const FE_AN_NAME[AN__N], *const FE_AN_WORD[AN__N];
extern const char *const FE_FX_NAME[FX__N];
extern const char *const FE_LD_NAME[LD__N];
extern const char *const FE_TH_NAME[TH__N];
extern const char *const FE_DIR_NAME[DIR__N];
extern const char *const FE_AMP_NAME[AMP__N];

void fe_study_default(FeStudy *s);
bool fe_study_load(const char *proj, FeStudy *s);          /* false: no study.ini */
bool fe_study_save(const char *proj, const FeStudy *s);
int  fe_group(const FeMesh *m, const char *name);           /* -1 when the mesh has no face of that name */
/* the material's properties as used (the library's, or custom) */
void fe_study_props(const FeStudy *s, FeMatProps *p);
/* which conditions an analysis uses: mechanical supports and loads, thermal conditions */
bool fe_an_mechanical(int an);
bool fe_an_thermal(int an);
/* a cylindrical face group's axis (unit), a point on it and its radius (mm); false: not a cylinder */
bool fe_cylinder(const FeMesh *m, int g, double axis[3], double ctr[3], double *radius);
/* the totals the loads come to: the force (N), the fixed nodes; and what's missing ("" when it can run) */
typedef struct { double force[3]; int fixedNodes, loadedNodes, unknownFaces; char problem[400]; } FeSummary;
void fe_study_summary(const FeStudy *s, const FeMesh *m, FeSummary *out);
bool fe_study_write(const FeStudy *s, const FeMesh *m, const char *proj, char *err, int cap);
/* the refinement for the next mesh: refine.txt in proj, a line per point of the picked faces of the mesh now (x y z size,
   mm), and the CAD faces by name ("Face 3 = 1.5") for a CAD part; false when there is none (the file removed) */
bool fe_refine_write(const FeStudy *s, const FeMesh *m, const char *proj);
#endif
