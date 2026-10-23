/* xsection.h -- the cross section of a 2-D case (one cell thick, "empty" front and back): its cells as polygons in the
 * plane, the boundary patches as line segments, and optionally a field's value in every cell */
#ifndef XSECTION_H
#define XSECTION_H

#include <stdbool.h>
#include "pl_util.h"

#define XS_MAX_PATCHES 64

typedef struct {
    bool   ok;
    char   err[200];
    int    axU, axV;                 /* the in-plane axes (0 x, 1 y, 2 z) */
    float  lo[2], hi[2];             /* extent in the plane */
    /* cells: polygon k has vertices [start[k], start[k+1]) in u/v */
    int    nPoly;
    int   *start;
    float *u, *v;
    int   *cell;                     /* the cell each polygon is a side of */
    int    nCells;
    /* boundary: segment i from (s[4i], s[4i+1]) to (s[4i+2], s[4i+3]) on patch segPatch[i] */
    int    nSeg;
    float *seg;
    int   *segPatch;
    int    nPatch;
    char   patch[XS_MAX_PATCHES][64];
    char   ptype[XS_MAX_PATCHES][24];
    /* a field */
    char   field[64];
    char   time[32];
    float *val;                      /* per cell (vector fields: magnitude), NULL = none */
    float  vmin, vmax;
    time_t meshTime;                 /* to notice a new mesh */
    bool   slice;                    /* a cut through a 3-D mesh (xs_load_slice), not a 2-D case's own plane */
    int    sliceAxis;                /*   its normal: 0 x, 1 y, 2 z            */
    float  slicePos;                 /*   where along it (metres)               */
    float  sliceLo, sliceHi;         /*   the mesh's extent along it            */
} XSection;

/* the mesh's boundary surface for the 3-D view (Mesh > View): triangles (faces fanned) with a flat normal each, the
   face outlines as line segments, per patch; and the mesh's counts */
#define MS_MAX_PATCHES 256
typedef struct {
    bool   ok;
    char   err[200];
    float *tri;          /* 9 floats per triangle */
    float *nrm;          /* 3 floats per triangle: its unit normal */
    int   *triPatch;
    long   nTri;
    float *edge;         /* 6 floats per segment (each face's outline; shared edges appear twice) */
    int   *edgePatch;
    long   nEdge;
    int    nPatch;
    char   patch[MS_MAX_PATCHES][64];
    char   ptype[MS_MAX_PATCHES][24];
    long   patchFaces[MS_MAX_PATCHES];
    float  lo[3], hi[3];
    long   nPoints, nFaces, nCells, nInternal;
    time_t meshTime;
} MeshSurf;
bool ms_load(MeshSurf *m, const char *polyMeshDir, long maxEdges);   /* edges only when the boundary has at most maxEdges */
void ms_free(MeshSurf *m);

bool xs_is_2d(const char *casePath);                         /* the mesh has an "empty" patch */
bool xs_load(XSection *x, const char *casePath);             /* reads constant/polyMesh; x->err says why not */
/* a 3-D mesh cut by the plane normal to axis at fraction frac (0..1) of its extent: the cut cells as polygons, the cut
   boundary as patch segments, in the same form as a 2-D cross section. meshDir: the polyMesh folder. */
bool xs_load_slice(XSection *x, const char *meshDir, int axis, float frac);
bool xs_load_field(XSection *x, const char *casePath, const char *time, const char *field);
void xs_free(XSection *x);

#endif
