/* meshadvice.h -- "What mesh do I need?" (mesh overhaul plan step 7, gap L6): a few questions about the flow and the
 * geometry, and the way to mesh it that follows, with each step of the reasoning written out (the answer, what it means
 * for the mesh, why). It ranges over every path the app has: the block builder's templates, extrusion to a wedge, the
 * STL mesher (snappyHexMesh, SALOME), and importing a mesh made elsewhere. Plain C, no drawing: page_meshadvice.c asks
 * and shows, test_meshadvice checks the choices. The reasons carry [[glossary]] markup. */
#ifndef MESHADVICE_H
#define MESHADVICE_H

#include <stdbool.h>

/* what is being meshed */
enum { MA_BOX = 0,      /* a box or a few boxes: a cavity, a duct, a room                         */
       MA_PIPE,         /* a straight round pipe                                                  */
       MA_CHANNEL,      /* flow between two walls, or a rectangular duct                          */
       MA_STEP,         /* a sudden expansion: a backward-facing step                             */
       MA_PLATE,        /* a flat plate's boundary layer                                          */
       MA_CYLINDER,     /* a cylinder in crossflow                                                */
       MA_AIRFOIL,      /* an airfoil profile                                                     */
       MA_PART,         /* any other shape: a part drawn in CAD (an STL or STEP file)             */
       MA_HAVE,         /* a mesh already made in another program                                 */
       MA__SHAPES };
extern const char *const MA_SHAPE_NAME[MA__SHAPES];

enum { MA_3D = 0, MA_2D, MA_AXI };                      /* 3-D; 2-D (one cell deep); axisymmetric (a wedge)   */
enum { MA_INTERNAL = 0, MA_EXTERNAL };                  /* through it; round it                               */
enum { MA_WALLS_NO = 0,   /* the walls only hold the flow in: mixing, a cavity's vortex, a room's air       */
       MA_WALLS_FUNC,     /* pressure drop, lift and drag to a few per cent: wall functions (y+ 30 to 300)  */
       MA_WALLS_RESOLVE,  /* heat transfer, separation, accurate drag: the wall resolved (y+ about 1)       */
       MA__WALLS };
enum { MA_QUICK = 0,      /* a first look, in minutes                                                       */
       MA_ANSWER,         /* an engineering answer                                                          */
       MA_INDEP,          /* an answer shown not to depend on the mesh (three meshes and a study)          */
       MA__GOALS };
enum { MA_FILE_STL = 0, MA_FILE_STEP };                  /* the part's file                                   */
enum { MA_FROM_GMSH = 0, MA_FROM_SALOME, MA_FROM_FLUENT, MA_FROM_STAR, MA_FROM_OTHER, MA__FROM };   /* a mesh made elsewhere */
extern const char *const MA_FROM_NAME[MA__FROM];

typedef struct {
    int    shape, dims, flow, walls, goal, file, from;
    bool   salome;                    /* SALOME is installed                                  */
    double U, L, nu;                  /* speed m/s, size m, kinematic viscosity m²/s (0: not given) */
} MaIn;

/* where it leads */
enum { MA_GO_BLOCKS = 0,  /* Mesh > Block builder, template tmpl                                            */
       MA_GO_STL,         /* Mesh > STL mesher, mesher                                                      */
       MA_GO_IMPORT,      /* Mesh > SALOME / import                                                         */
       MA_GO_CONVERT,     /* a converter run by hand (fluentMeshToFoam, ccmToFoam ...): the command is given */
       MA__GO };

enum { MA_MESHER_SNAPPY = 0, MA_MESHER_TET = 1 };       /* = stlmesh.h's MESHER_SNAPPY, MESHER_TET */

#define MA_MAX_STEPS 10
typedef struct {
    int    go;                        /* MA_GO_*                                              */
    int    tmpl;                      /* BT_* (MA_GO_BLOCKS), else -1                         */
    int    mesher;                    /* MESHER_* (MA_GO_STL), else -1                        */
    bool   layers;                    /* prism layers / wall grading                          */
    double yplus;                     /* the target y+ at the first cell (0: no wall target)  */
    double h1;                        /* m: the first cell height for it (0: not worked out)  */
    double Re;                        /* the Reynolds number (0: not given)                   */
    double cells;                     /* about how many cells (the finest, for a study)       */
    int    nMeshes;                   /* 1, or 3 for a grid study                             */
    char   title[160];                /* the answer in a line                                 */
    char   cmd[200];                  /* MA_GO_CONVERT: the command                           */
    int    nSteps;
    struct { char q[120]; char r[520]; } step[MA_MAX_STEPS];   /* the answer given, and what follows from it */
} MaOut;

void ma_advise(const MaIn *in, MaOut *out);

#endif
