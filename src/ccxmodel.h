/* ccxmodel.h -- a CalculiX deck in engineering terms (workflow plan C1/C2, 2026-11-14): its materials, its supports
 * (*BOUNDARY), its loads (*CLOAD, *DLOAD), the node sets they act on and their sizes, the model's extent, element type and
 * analysis; the unit system the numbers imply, and a check that they agree. Reads the input and its *INCLUDE files. */
#ifndef CCXMODEL_H
#define CCXMODEL_H
#include <stdbool.h>

#define CCX_MAX_SETS 64
#define CCX_MAX_BC 32
#define CCX_MAX_MAT 8

typedef struct { char name[64]; int nNodes; } CcxSet;
typedef struct {
    char   set[64];              /* a node set's name, or a node number                     */
    int    d0, d1;               /* degrees of freedom: 1-3 x y z, 4-6 rotations, 11 temperature */
    double value; bool hasValue; /* a prescribed displacement (or temperature)              */
    int    line;                 /* 1-based, in the input file itself (0: in an included file) */
} CcxBc;
typedef struct {
    char   set[64];
    int    dof;                  /* *CLOAD: 1-3 a force along x y z; 11 a heat flow        */
    double mag;                  /* per node                                               */
    int    line;
} CcxLoad;
typedef struct { char set[64]; char kind[16]; double mag; double dir[3]; int line; } CcxDload;   /* P1..P6 pressure, GRAV */
typedef struct {
    char   name[64];
    double E, nu, rho, k, cp, alpha;
    bool   hasE, hasRho, hasK, hasCp, hasAlpha;
    int    lineE, lineRho;       /* the data lines (0: in an included file)                */
} CcxMat;
enum { CCX_STATIC = 0, CCX_HEAT, CCX_FREQUENCY, CCX_DYNAMIC, CCX_COUPLED, CCX_OTHER, CCX_NONE };
enum { CCX_UNITS_UNKNOWN = -1, CCX_UNITS_SI = 0, CCX_UNITS_MM = 1 };   /* m N Pa kg/m³  |  mm N MPa t/mm³ */
typedef struct {
    CcxSet  set[CCX_MAX_SETS]; int nSets;
    CcxBc   bc[CCX_MAX_BC]; int nBc;
    CcxLoad load[CCX_MAX_BC]; int nLoad;
    CcxDload dl[CCX_MAX_BC]; int nDl;
    CcxMat  mat[CCX_MAX_MAT]; int nMat;
    double  lo[3], hi[3]; int nNodes; long nElems;
    char    elType[16];
    int     analysis;            /* CCX_* of the first step                                */
    bool    nonlinear;           /* NLGEOM on a step                                       */
} CcxModel;

bool ccx_model_load(const char *dir, const char *input, CcxModel *m);   /* false: unreadable */
bool ccx_model_load_text(const char *dir, const char *input, const char *text, CcxModel *m);   /* the input's text given (unsaved edits) */
int  ccx_set_nodes(const CcxModel *m, const char *set);   /* -1 unknown; a node number counts 1 */
double ccx_extent(const CcxModel *m);                      /* the model's largest side, in its own length unit */
/* the unit system E (else the density) implies, and why not when the model's size or density disagree with it (msg "")  */
int  ccx_units(const CcxModel *m, char *msg, int cap);
const char *ccx_dof_name(int dof);                         /* "x", "y", "z", "rotation about x" ..., "temperature" */
const char *ccx_analysis_name(int a);

/* the materials Setup offers, in SI (Pa, kg/m³, W/(m K), J/(kg K), 1/K, Pa yield) */
typedef struct { const char *name; double E, nu, rho, k, cp, alpha, yield; } CcxLibMat;
extern const CcxLibMat CCX_LIB[];
extern const int CCX_LIB_N;
int  ccx_lib_match(const CcxMat *m, int units);            /* the library material whose E and density these are, -1 none */
/* ---- results (workflow plan C2): a .frd's last increment of each block. Nodes by index (id[i]); arrays NULL when absent */
typedef struct {
    int    n; int *id; float *x;            /* x: 3 per node                                         */
    float *u, *vm, *rf, *T;                  /* displacement (3), von Mises, reaction (3), temperature */
    int    nEdges; int *edge;                /* pairs of node indices (brick elements' edges)          */
    double uMax; int uMaxAt; double vmMax; int vmMaxAt; double rfSum[3];   /* rfSum: the reactions at the nodes that don't move */ double tMin, tMax;
    double time;                             /* the last increment's time                            */
} CcxFrd;
bool ccx_frd_load(const char *path, CcxFrd *r);   /* false: unreadable or no nodes */
void ccx_frd_free(CcxFrd *r);
/* a slender box fixed at one end and loaded across at the other (the beam starter and its kind): the Euler-Bernoulli tip
   deflection F L^3 / (3 E I) and root stress F L c / I, in the model's units; false when the model isn't one */
typedef struct { double L, b, h, F, E, I, delta, sigma; int axis, loadAxis; } CcxBeam;
bool ccx_beam_check(const CcxModel *m, CcxBeam *b);
/* ---- the run (workflow plan C3): what CalculiX did, from <job>.sta (one row per converged increment), <job>.dat's
   eigenvalue table and the log's "Total CalculiX Time" */
#define CCX_MAX_MODES 32
typedef struct {
    bool   hasSta;
    int    nInc, cutbacks, steps;            /* increments that converged; attempts beyond the first (the increment cut back) */
    int    itersTotal, itersMax;             /* equilibrium iterations                                 */
    double tot, stepTime, incFirst, incMin, incMax, incLast;   /* times reached; increment sizes       */
    int    nModes; double hz[CCX_MAX_MODES], rad[CCX_MAX_MODES];   /* *FREQUENCY: cycles and radians per time */
    int    nNeg;                             /* negative eigenvalues (CalculiX prints 0 Hz and an imaginary part) */
    double cpu;                              /* Total CalculiX Time, s (-1: not finished)              */
} CcxRun;
void ccx_run_load(const char *dir, const char *job, const char *logPath, CcxRun *r);   /* job: the input without .inp */
/* a cantilever's first bending frequency (Euler-Bernoulli, 1.875^2/(2 pi) sqrt(E I / (rho A L^4)), the softer side); 0 when not one */
double ccx_beam_f1(const CcxModel *m);
/* ---- the model's mesh (workflow plan C4): nodes, element edges (each once) and which nodes the supports and loads act on */
typedef struct {
    int    n; int *id; float *x;             /* x: 3 per node                                           */
    unsigned char *role;                     /* bits 1 2 4: held in x y z (*BOUNDARY); 8: temperature held; 16: a point load (*CLOAD) */
    int    nEdges; int *edge;                /* pairs of node indices                                   */
    int    nElems;
} CcxMesh;
bool ccx_mesh_load(const char *dir, const char *input, const CcxModel *m, CcxMesh *g);   /* false: no nodes */
void ccx_mesh_free(CcxMesh *g);
/* plain C3D8 bricks carrying bending lock (too stiff), the more the fewer there are through the thickness: the layers through
   the thinnest side when a slender part is meshed with them and has 3 or fewer (g gives the count; NULL: -1, unknown), else 0 */
int  ccx_locking_risk(const CcxModel *m, const CcxMesh *g);
/* every *ELEMENT, TYPE=from in the input and its includes changed to to; the number of lines changed */
int  ccx_set_element_type(const char *dir, const char *input, const char *from, const char *to);
#endif
