/* extrude.h -- extrudeMesh (mesh overhaul plan step 6): extruding a patch of the mesh (extra cells beyond an outlet, a
 * wall thickened into a region), and turning a 2-D mesh into an axisymmetric wedge. Writes the dictionaries; the page runs
 * the tools. Keywords checked against OpenFOAM v2606: $FOAM_ETC/caseDicts/annotated/extrudeMeshDict and the climbingRod
 * tutorial (wedge from a patch).
 *
 * Found with v2606 (2026-11-21), and handled here:
 *  - constructFrom patch builds a new mesh from the patch alone; exposedPatchName must name an existing patch;
 *  - the extrusion runs along the source patch's outward normal: for the wedge, the axis is r x n (r from the axis to
 *    the mesh, n the front patch's outward normal), else every cell comes out inside out;
 *  - cells on the axis collapse to prisms by themselves (the axis patch is left with no faces);
 *  - points written at the case's writePrecision (6 in most tutorials) leave the wedge faces "not planar" on every
 *    load: the job raises writePrecision to 15 while extrudeMesh writes, then puts it back. */
#ifndef EXTRUDE_H
#define EXTRUDE_H

#include <stdbool.h>

enum { EX_LINEAR_NORMAL = 0, EX_LINEAR_DIRECTION, EX_PLANE, EX_WEDGE, EX_SECTOR, EX__N };
extern const char *const EX_MODEL[EX__N];        /* OpenFOAM's names: linearNormal ...           */
extern const char *const EX_MODEL_NAME[EX__N];   /* in words                                     */

typedef struct {
    int    model;           /* EX_*                                                          */
    char   patch[64];       /* the patch extruded                                            */
    int    nLayers;
    double expansion;       /* each layer this times the one before                          */
    double thickness;       /* m, linear models                                              */
    double dir[3];          /* linearDirection                                               */
    double point[3], axis[3], angle;   /* wedge and sector: a point on the axis, its direction, degrees */
    bool   flip;            /* extrude the other way (flipNormals)                           */
} ExtrudeCfg;

/* extrudeMeshDict for extruding a patch onto the existing mesh (constructFrom mesh); NULL with why when it can't be */
char *ex_patch_dict(const ExtrudeCfg *c, char *why, int cap);

/* turning a 2-D mesh (one cell thick across direction thin, front and back on the empty patches `empties`, space-separated)
   into a wedge about the axis along direction axisDir through axisAt (the coordinate in the third direction), angle
   degrees in all. lo / hi: the mesh's bounds. The three dictionaries the job writes, malloc'd: topoSetDict.axi (the
   front and back faces as sets), createPatchDict.axi (them as the patches front, back) and extrudeMeshDict. */
typedef struct {
    int    thin, axisDir;
    double axisAt, angle;
    double lo[3], hi[3];
    char   empties[256];
    char   front[64], back[64];   /* the wedge patches' names (default front, back)              */
} AxiCfg;
bool ex_axi_dicts(const AxiCfg *c, char **topoSet, char **createPatch, char **extrude, char *why, int cap);

#endif
