/* bmesh.h -- blockMesh as a model (mesh overhaul plan step 5, gaps L3 / E2): vertices, hex blocks with simple, edge and
 * multi-grading, curved edges (arc, spline, polyLine, BSpline), patches; read from and written to system/blockMeshDict;
 * checked the way blockMesh would refuse it; drawn as a preview; and the templates (cavity, channel, backward step, flat
 * plate, pipe O-grid, cylinder in crossflow, airfoil C-mesh, axisymmetric wedge) that make one from sizes a learner knows.
 * Keywords checked against OpenFOAM v2606: $FOAM_ETC/caseDicts/annotated/blockMeshDict and src/mesh/blockMesh.
 * Plain C, no drawing: page_blocks.c is the page, test_bmesh the tests. */
#ifndef BMESH_H
#define BMESH_H

#include <stdbool.h>
#include "foamdict.h"
#include "pl_util.h"

#define BM_MAXV   512
#define BM_MAXB   128
#define BM_MAXE   256
#define BM_MAXPTS 160      /* points on one curved edge                */
#define BM_MAXP   48       /* patches                                  */
#define BM_MAXF   256      /* faces on one patch                       */
#define BM_MAXSEG 8        /* multi-grading segments in one direction  */

enum { BG_SIMPLE = 0, BG_EDGE };                       /* simpleGrading (3 directions) or edgeGrading (12 edges) */
enum { BE_ARC = 0, BE_SPLINE, BE_POLYLINE, BE_BSPLINE, BE__N };
extern const char *const BE_NAME[BE__N];               /* "arc", "spline", "polyLine", "BSpline" */

/* one direction's (or edge's) grading: a single expansion ratio, or segments of (share of the length, share of the
   cells, expansion ratio), blockMesh's multi-grading; shares need not add to 1 (blockMesh normalises them) */
typedef struct { int n; double len[BM_MAXSEG], cells[BM_MAXSEG], ratio[BM_MAXSEG]; } BmGrade;

typedef struct {
    int     v[8];
    int     n[3];
    int     kind;          /* BG_*                                                       */
    BmGrade g[12];         /* BG_SIMPLE: g[0..2] for x1 x2 x3; BG_EDGE: the 12 edges     */
    char    zone[64];      /* cellZone name, or ""                                       */
} BmBlock;

typedef struct {
    int    kind;           /* BE_*                                                       */
    int    a, b;
    int    np;
    double p[BM_MAXPTS][3];  /* arc: the one point the arc passes through; the others: the interior points */
} BmEdge;

typedef struct {
    char name[64];
    char type[32];
    int  nf;
    int  f[BM_MAXF][4];
    char extra[256];       /* the patch's other entries as written (inGroups, neighbourPatch, ...), kept on save */
} BmPatch;

typedef struct {
    double  scale;         /* blockMesh's scale (convertToMeters in old files)           */
    int     nv, nb, ne, np;
    double  v[BM_MAXV][3];
    BmBlock b[BM_MAXB];
    BmEdge  e[BM_MAXE];
    BmPatch p[BM_MAXP];
    char    defaultName[64], defaultType[32];   /* defaultPatch, when the file sets one   */
} BmModel;

void   bm_init(BmModel *m);
double bm_cells(const BmModel *m);

/* ---- reading and writing */
/* the model from a blockMeshDict; false (why says, for the page) when it can't be edited without losing something:
   $macros, #eval / #calc / #include, named vertices, projected geometry, blocks that aren't hex, curved faces */
bool bm_read(FDFile *f, BmModel *m, char *why, int cap);
bool bm_read_text(const char *text, BmModel *m, char *why, int cap);
/* a whole blockMeshDict, FoamFile header included (malloc'd) */
char *bm_write(const BmModel *m, const char *note);
/* the model into an existing file: vertices, blocks, edges, boundary and scale replaced, everything else kept */
bool bm_apply(FDFile *f, const BmModel *m);

/* ---- pieces as the builder's tables show them: text in, text out (false: it doesn't parse; the model is unchanged) */
void bm_grading_text(const BmBlock *b, char *out, int cap);          /* "1 4 1" or "1 ((0.5 0.5 4) (0.5 0.5 0.25)) 1"   */
bool bm_grading_parse(const char *text, BmBlock *b);                 /* 3 items: simpleGrading; 12: edgeGrading          */
void bm_ints_text(const int *v, int n, char *out, int cap);          /* "0 1 2 3"                                        */
bool bm_ints_parse(const char *text, int *v, int n);                 /* exactly n whole numbers, brackets ignored         */
void bm_faces_text(const BmPatch *p, char *out, int cap);            /* "(0 4 7 3) (2 6 5 1)"                            */
bool bm_faces_parse(const char *text, BmPatch *p);
void bm_points_text(const BmEdge *e, char *out, int cap);            /* "(0.5 1.1 0) ..."                                */
bool bm_points_parse(const char *text, BmEdge *e);

/* ---- checks: what blockMesh would refuse, and what it would accept but meant a mistake */
typedef struct { int level; char text[200]; } BmIssue;   /* level 1 warning, 2 error */
int  bm_check(const BmModel *m, BmIssue *out, int max);

/* ---- the preview: block edges (curved ones sampled) and the cell divisions along them */
typedef struct {
    int    nSeg;  float *seg;     /* line segments, 6 floats each, in metres (scaled)          */
    int   *segBlockEdge;          /* per segment: 1 a block edge, 0 a division tick            */
    float  lo[3], hi[3];          /* bounding box                                              */
} BmDraw;
void bm_draw_build(const BmModel *m, BmDraw *d);
void bm_draw_free(BmDraw *d);
/* a point along block b's edge k (0..11, blockMesh's numbering) at t in [0, 1], curved edges followed */
void bm_edge_point(const BmModel *m, int b, int k, double t, double out[3]);
/* the cell boundaries of a grading along a unit length with n cells: out[0] = 0 ... out[n] = 1 */
void bm_grade_points(const BmGrade *g, int n, double *out);

/* ---- templates */
enum { BT_CAVITY = 0, BT_CHANNEL, BT_STEP, BT_PLATE, BT_PIPE, BT_CYLINDER, BT_AIRFOIL, BT_WEDGE, BT__N };
#define BT_MAXPAR 12
typedef struct { const char *key, *label, *unit, *hint; double def, lo, hi; } BmParam;
typedef struct {
    const char *key, *name, *what;
    int nPar;
    BmParam par[BT_MAXPAR];
    const char *patches;          /* the patches it makes, in words                             */
} BmTemplate;
extern const BmTemplate BM_TEMPLATES[BT__N];
int  bm_template_par(int t, const char *key);       /* the parameter's index, -1 if none   */
/* the model from template t with parameter values val[] (as BM_TEMPLATES[t].par); false with err */
bool bm_template(int t, const double *val, BmModel *m, char *err, int cap);
/* the expansion ratio (last / first) that gives a first cell d1 over length L in n cells; 1 when d1 <= 0 */
double bm_ratio_for_first(double L, int n, double d1);

#endif
