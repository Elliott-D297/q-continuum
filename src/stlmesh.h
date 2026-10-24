/* stlmesh.h -- an STL surface turned into a mesh (user request, 2026-10-23): read any STL, split it into boundaries by
 * angle, and mesh it in the way of STAR-CCM+'s automated mesher (base size, target and minimum surface size as a
 * percentage of it, curvature, growth, prism layers, volumetric controls), with snappyHexMesh here or SALOME's NETGEN. */
#ifndef STLMESH_H
#define STLMESH_H

#include <stdbool.h>
#include "pl_util.h"

#define STL_MAX_REGIONS 48
#define STL_MAX_ZONES 8

enum { BT_WALL = 0, BT_PATCH, BT_INLET, BT_OUTLET, BT_SYMMETRY, BT__N };
enum { MESHER_SNAPPY = 0, MESHER_TET, MESHER_POLY, MESHER_BODYFIT, MESHER__N };
enum { DOMAIN_INTERNAL = 0, DOMAIN_EXTERNAL };
enum { ZONE_BOX = 0, ZONE_SPHERE, ZONE_CYLINDER };

typedef struct {
    char   name[64];        /* the boundary (patch) name                 */
    int    type;            /* BT_*                                      */
    int    nTri;
    double area;
    float  n[3];            /* the area-weighted mean normal             */
} StlRegion;

typedef struct {
    int    nTri;
    float *tri;             /* 9 floats a triangle, scaled               */
    int   *region;          /* a region a triangle                       */
    char (*solid)[64];      /* the solid a triangle came from (ASCII)    */
    int    nRegions;
    StlRegion reg[STL_MAX_REGIONS];
    float  bmin[3], bmax[3];
    double area, volume;    /* volume: enclosed, from the divergence theorem (|.|) */
    int    openEdges, nonManifold;
    bool   binary;
    double scale;
    char   path[PL_PATH];
    char   err[256];
} Stl;

typedef struct {
    int    kind;            /* ZONE_*                                    */
    double a[3], b[3];      /* box: min, max; sphere: centre; cylinder: the two end centres */
    double r;               /* sphere and cylinder radius                */
    double sizePct;         /* target size there, % of base              */
} MeshZone;

typedef struct {
    char   stl[PL_PATH];    /* the file it came from                     */
    double scale;           /* to metres                                 */
    double splitAngle;      /* degrees; 0 keeps the file's own solids    */
    int    domain;          /* DOMAIN_*                                  */
    double up, down, side, top;   /* external: the wind tunnel, in body lengths */
    bool   ground;          /* external: the body sits on the floor      */
    int    mesher;          /* MESHER_*                                  */
    bool   remesh;          /* SALOME: rebuild the surface from CAD made of the STL (else use its triangles) */
    double base;            /* base size, m                              */
    double targetPct, minPct;     /* surface sizes, % of base            */
    double curvature;       /* points per circle                         */
    double surfGrowth, volGrowth;
    double featureAngle;    /* degrees: sharper edges are kept           */
    bool   prism;
    int    layers;
    double prismPct;        /* prism layer total thickness, % of base   */
    double stretch;         /* prism layer stretching                    */
    double maxNonOrtho, maxSkew;
    int    nZones;
    MeshZone zone[STL_MAX_ZONES];
    int    nNames;          /* the boundaries' names and types, as edited */
    char   names[STL_MAX_REGIONS][64];
    int    types[STL_MAX_REGIONS];
} MeshCfg;

extern const char *const BT_NAME[BT__N];
extern const char *const MESHER_NAME[MESHER__N];

bool stl_load(const char *path, Stl *s);           /* ASCII or binary; false with s->err */
void stl_free(Stl *s);
void stl_split(Stl *s, double angleDeg);           /* regions by angle (0: by solid), boundary checks, area, volume */
void stl_set_scale(Stl *s, double scale);          /* rescale the triangles (relative to the last scale) */
bool stl_inside(const Stl *s, const double p[3]);  /* ray parity: p is inside the closed surface */

void mesher_defaults(MeshCfg *m, const Stl *s);    /* sizes from the part's size */
bool mesher_load(const char *casePath, MeshCfg *m);
bool mesher_save(const char *casePath, const MeshCfg *m);
void mesher_apply_names(const MeshCfg *m, Stl *s); /* the edited names and types onto the regions */
void mesher_take_names(MeshCfg *m, const Stl *s);
double mesher_estimate_cells(const MeshCfg *m, const Stl *s);
void mesher_domain(const MeshCfg *m, const Stl *s, double lo[3], double hi[3]);   /* the meshed box */

/* writes constant/triSurface/geometry.stl, mesher/regions/<name>.stl, mesher/mesher.ini, salome/stl_mesh.py, the minimal
   system dictionaries a case needs and, for snappyHexMesh, its dictionaries (blockMesh, surfaceFeatureExtract, snappy,
   meshQuality: a file of the user's is kept as <name>.orig first); false with err */
bool mesher_prepare(const char *casePath, const MeshCfg *m, Stl *s, char *err, int cap);
/* the shell lines that give each boundary its type after a conversion (ideasUnvToFoam): foamDictionary on the boundary */
void mesher_type_script(const MeshCfg *m, const Stl *s, Str *out);

#endif
