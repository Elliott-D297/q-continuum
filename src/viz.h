/* viz.h -- Results > Visualization's engine (docs/VISUALIZATION_PLAN.md): an OpenFOAM mesh and its fields in memory, the
 * fields derived from them, point location and interpolation, and the builders that turn them into what is drawn
 * (surfaces with a value per vertex, lines, images, voxel grids). Plain C, no drawing: page_viz.c draws, test_viz tests. */
#ifndef VIZ_H
#define VIZ_H

#include <stdbool.h>
#include "xsection.h"

#define VZ_MAX_PATCHES 256

typedef struct {
    bool    ok;
    char    err[200];
    long    nPoints, nFaces, nInternal, nCells;
    double *pt;                       /* 3 a point                                          */
    long   *fOff, *fIdx;              /* face f: points fIdx[fOff[f] .. fOff[f+1])         */
    long   *own, *nei;                /* nei: nInternal entries                             */
    int     nPatch;
    FrPatch patch[VZ_MAX_PATCHES];
    /* derived */
    double *fCtr, *fArea;             /* 3 a face: centre, area vector (owner to neighbour) */
    double *cCtr, *cVol;              /* 3 a cell, 1 a cell                                 */
    long   *cfOff, *cfIdx;            /* cell c's faces: cfIdx[cfOff[c] .. cfOff[c+1])     */
    long   *pcOff, *pcIdx;            /* point p's cells                                   */
    double  lo[3], hi[3];
    double  h;                        /* a typical cell size (cube root of the mean volume) */
    long   *grid;                     /* a uniform grid of cell centres for the first guess in point location */
    long   *gridOff;
    int     gn[3];
    /* a decomposed case (vz_mesh_procs): each processor's cells and faces in the whole mesh, from decomposePar's maps */
    int     nProc;
    struct VzProc { long nCells, nFaces, nInternal; long *cell, *face; int nPatch; FrPatch patch[VZ_MAX_PATCHES]; } *proc;
} VzMesh;

bool vz_mesh_load(VzMesh *m, const char *polyMeshDir);
void vz_mesh_free(VzMesh *m);
int  vz_patch_of_face(const VzMesh *m, long f);      /* -1 for an internal face */

/* a field on the mesh: per cell, per boundary face, and per point (averaged), 1 or 3 components */
typedef struct {
    char    name[64];
    char    dims[40];                 /* "[0 2 -2 0 0 0 0]"                                  */
    int     nc;                       /* 1 or 3                                              */
    double *c;                        /* nCells x nc                                         */
    double *b;                        /* (nFaces - nInternal) x nc: the patch value, or its cell's */
    double *p;                        /* nPoints x nc                                        */
} VzField;

bool vz_field_load(VzField *f, const VzMesh *m, const char *casePath, const char *time, const char *name);
/* a decomposed case: read each processorN/constant/polyMesh's cellProcAddressing and faceProcAddressing onto m (the whole mesh,
   constant/polyMesh); how many processors, 0 when there are none or the maps don't fit m (err says why). Afterwards
   vz_field_load_procs reads every processorN/<time>/<name> into one field on m: exactly what reconstructPar would write */
int  vz_mesh_procs(VzMesh *m, const char *casePath);
bool vz_field_load_procs(VzField *f, const VzMesh *m, const char *casePath, const char *time, const char *name);
void vz_field_free(VzField *f);
bool vz_field_points(VzField *f, const VzMesh *m);   /* (re)fills f->p from c and b; false: out of memory (f->p NULL) */

/* ---- derived fields: what to colour by. A "quantity" is a field and how to read it */
enum { VQ_VALUE = 0, VQ_MAG, VQ_X, VQ_Y, VQ_Z };     /* a scalar's value; a vector's magnitude or a component */
enum { VD_NONE = 0, VD_PPA, VD_PTOTAL, VD_GRADMAG, VD_VORTMAG, VD_Q, VD_HELICITY, VD_CELLVOL, VD__N };
extern const char *const VD_NAME[VD__N];
/* a derived scalar field built from loaded fields: VD_PPA p x rho; VD_PTOTAL p + |U|^2 / 2 (times rho for Pa when the p
   is kinematic and rho > 0 is given); VD_GRADMAG |grad U|; VD_VORTMAG |curl U|; VD_Q (|W|^2 - |S|^2) / 2; VD_HELICITY
   U . curl U; VD_CELLVOL. p, U may be NULL where not needed. false when what it needs is missing */
bool vz_derived(VzField *out, const VzMesh *m, int kind, const VzField *p, const VzField *U, double rho);
/* the Gauss gradient of component k of f at every cell: 3 a cell (malloc'd) */
double *vz_gradient(const VzMesh *m, const VzField *f, int k);

/* a field's scalar for quantity q at cell / boundary face / point i */
double vz_cell(const VzField *f, int q, long i);
double vz_bnd(const VzField *f, int q, long i);
double vz_point(const VzField *f, int q, long i);

/* ---- statistics */
typedef struct { double min, max, mean, sum; long n; } VzStats;   /* mean: volume-weighted over cells, area-weighted on faces */
void vz_stats_cells(const VzMesh *m, const VzField *f, int q, VzStats *s);
void vz_stats_patch(const VzMesh *m, const VzField *f, int q, int patch, VzStats *s);
/* a histogram of the cells' values in nb bins between lo and hi, volume-weighted (each bin a share of the volume) */
void vz_histogram(const VzMesh *m, const VzField *f, int q, double lo, double hi, int nb, double *bins);

/* ---- location and interpolation */
long vz_find_cell(const VzMesh *m, const double x[3], long hint);    /* -1 outside the mesh */
/* the value of quantity q (or all nc components into out when q < 0) at x, from the containing cell's points; false
   outside the mesh. *cell: the cell found (a hint for the next call) */
bool vz_sample(const VzMesh *m, const VzField *f, int q, const double x[3], long *cell, double *out);

/* ---- what gets drawn */
typedef struct {
    long   n;                         /* triangles                                          */
    float *xyz;                       /* 9 a triangle                                       */
    float *val;                       /* 3 a triangle: the value at each vertex (NAN: none) */
    float *nrm;                       /* 3 a triangle                                       */
    int   *cell;                      /* the cell it is in or on (a boundary face's owner)  */
    int   *grp;                       /* its patch (boundary), else -1                      */
} VzTris;
typedef struct {
    long   nv, nl;                    /* vertices; polylines                                */
    float *xyz;                       /* 3 a vertex                                         */
    float *val;                       /* 1 a vertex                                         */
    long  *off;                       /* polyline k: vertices off[k] .. off[k+1]           */
} VzLines;
void vz_tris_free(VzTris *t);
void vz_lines_free(VzLines *l);

/* the boundary faces of the patches marked in show[] (NULL: all but empty and processor), each fanned into triangles;
   values from quantity q of f (NULL: none). smooth: point values at the vertices, else each face's own value */
void vz_boundary(const VzMesh *m, const VzField *f, int q, const bool *show, bool smooth, VzTris *out);
/* the zero set of a function given at the points (fn, one a point) through every cell, as triangles coloured by q of f;
   the plane and iso-surface builders call it */
void vz_cut(const VzMesh *m, const double *fn, const VzField *f, int q, bool smooth, VzTris *out);
void vz_section(const VzMesh *m, const double origin[3], const double normal[3], const VzField *f, int q, bool smooth, VzTris *out);
void vz_isosurface(const VzMesh *m, const VzField *iso, int isoQ, double value, const VzField *f, int q, VzTris *out);
/* the outer faces of the cells whose value of q of f is within [lo, hi] */
void vz_threshold(const VzMesh *m, const VzField *f, int q, double lo, double hi, const VzField *col, int colQ, VzTris *out);
/* the triangles of t on the side of the plane its normal points away from (clip): kept whole when any vertex is inside */
void vz_clip(VzTris *t, const double origin[3], const double normal[3]);
/* iso-lines of t's vertex values at n levels between lo and hi (log spacing when logScale) */
void vz_isolines(const VzTris *t, double lo, double hi, int n, bool logScale, VzLines *out);

/* ---- vectors and streamlines */
/* glyph positions over the triangles of t (their centres), thinned to at most one per cell of a grid of side spacing,
   with the vector f (3 components) there: pos 3 a glyph, vec 3 a glyph; returns the count (malloc'd arrays) */
long vz_glyph_points(const VzMesh *m, const VzField *U, const VzTris *t, double spacing, float **pos, float **vec);
enum { VS_POINT = 0, VS_LINE, VS_PLANE, VS_PATCH };
typedef struct {
    int    kind;                      /* VS_*                                               */
    double a[3], b[3];                /* point: centre (a), radius b[0]; line: a to b; plane: centre a, normal b */
    double size;                      /* plane: side of the seeding square                  */
    int    n;                         /* seeds (a plane: n x n)                             */
    int    patch;                     /* VS_PATCH                                           */
    int    dir;                       /* 0 forward, 1 backward, 2 both                      */
    double maxLen;                    /* m (0: 10 x the mesh's diagonal)                    */
    int    maxSteps;
} VzSeeds;
/* streamlines of U from the seeds, coloured by q of col (NULL: U's magnitude); RK4 in steps of a fifth of the local cell */
void vz_streamlines(const VzMesh *m, const VzField *U, const VzSeeds *s, const VzField *col, int colQ, VzLines *out);
/* the seed points (3 each) a VzSeeds makes: malloc'd, count returned */
long vz_seed_points(const VzMesh *m, const VzSeeds *s, double **pts);

/* ---- images and volumes */
/* line integral convolution on a section plane: a w x h image of the in-plane velocity's streaks over the square of side
   size centred on origin (u along the plane's first in-plane axis), lum 0..1 (NAN outside the mesh), and the colour
   quantity's value per pixel in val (NAN outside); axes out: the plane's in-plane unit vectors */
void vz_lic(const VzMesh *m, const VzField *U, const VzField *col, int colQ, const double origin[3], const double normal[3],
            double size, int w, int h, int len, float *lum, float *val, double eu[3], double ev[3]);
/* the field resampled onto an n[0] x n[1] x n[2] grid over the mesh's box (NAN outside): malloc'd */
float *vz_resample(const VzMesh *m, const VzField *f, int q, const int n[3]);

/* ---- reports on a surface (a section plane): its area, the area-weighted mean of its vertex values (NAN ones left out), and
   with U the volume flow through it, the integral of U . n dA with n the triangles' normals turned toward dir (NULL: as they
   are), U sampled at each triangle's centre */
typedef struct { double area, mean, flow, flowAbs; long n; } VzSurfReport;
void vz_surface_report(const VzMesh *m, const VzTris *t, const VzField *U, const double dir[3], VzSurfReport *r);

/* ---- probes */
/* n samples along a to b: positions (3 each) and values (NAN outside); returns how many are inside */
int vz_line_probe(const VzMesh *m, const VzField *f, int q, const double a[3], const double b[3], int n, double *pos, double *val);
/* the first triangle of t the ray from o along d hits: its index, the distance in *t_; -1 none */
long vz_pick(const VzTris *t, const double o[3], const double d[3], double *dist);

#endif
