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
enum { DIR_PX = 0, DIR_NX, DIR_PY, DIR_NY, DIR_PZ, DIR_NZ, DIR__N };   /* +x -x +y -y +z -z: axis = d / 2, sign = d % 2 ? -1 : 1 */

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
    char   stlOriginal[PL_PATH];   /* 2-D: the coarse STL that stl is the smooth profile of ("": none) */
    double scale;           /* to metres                                 */
    double splitAngle;      /* degrees; 0 keeps the file's own solids    */
    int    domain;          /* DOMAIN_*                                  */
    double up, down, side, top;   /* external: the wind tunnel, in body lengths */
    double below;           /* external: room under the body (not on the ground), in body lengths */
    bool   ground;          /* external: the body sits on the floor      */
    int    flowDir;         /* external: DIR_* the flow goes (the inlet is on the other side) */
    int    upDir;           /* external: DIR_* that is up (the ground below, the top above)  */
    double refLen;          /* external: the tunnel's distances are in these, m; 0: the body's length along the flow */
    bool   twoD;            /* a 2-D mesh, one cell thick across normal2D: a profile (an airfoil) extruded along it */
    int    normal2D;        /* the axis (0 x, 1 y, 2 z) the profile was extruded along                              */
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
    int    evenLayers;      /* snappy: the layered walls' surface cells all one size (the minimum), so the layers don't
                               break where the size steps: -1 as the kind of mesh (2-D yes, 3-D no), 0 no, 1 yes */
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
/* 2-D: the outline of a profile extruded along axis n (the side walls at its low end): how many points, and its largest
   corner below cornerDeg (a coarse export's facets); false when it isn't one plain extruded outline */
bool stl_profile_facets(const Stl *s, int n, double cornerDeg, int *nPts, double *maxKink);
/* the profile again, smooth: a spline through its points, resampled about every segLen (400 to 4000 points, closer where
   it bends), corners of cornerDeg or more kept, the regions kept in their order; written as a binary STL in the file's
   own units. Returns the points (0: why in err) */
int  stl_smooth_profile(const Stl *s, int n, double cornerDeg, double segLen, const char *outPath, char *err, int cap);

void mesher_defaults(MeshCfg *m, const Stl *s);    /* sizes from the part's size */
void mesher_size_defaults(MeshCfg *m, const Stl *s, double thick);   /* base, surface and prism sizes for m->domain */
double stl_thickness(const Stl *s);                /* how thick the part is where it is thin, m (0: unknown) */
bool mesher_fits(const MeshCfg *m, double thick);  /* its surface cells fit across that thickness */
void mesher_fit(MeshCfg *m, double thick);         /* finer surface sizes (whole levels) until they do */
bool mesher_load(const char *casePath, MeshCfg *m);
bool mesher_save(const char *casePath, const MeshCfg *m);
/* the mesh's settings (mesher/mesh.sig, from the last Generate) are these, as far as the mesh goes: 1 yes, 0 no, -1 unknown */
int  mesher_mesh_current(const char *casePath, const MeshCfg *m);
void mesher_signature(const MeshCfg *m, Str *out);   /* what mesher/mesh.sig holds for m */
void mesher_apply_names(const MeshCfg *m, Stl *s); /* the edited names and types onto the regions */
void mesher_take_names(MeshCfg *m, const Stl *s);
double mesher_estimate_cells(const MeshCfg *m, const Stl *s);
void mesher_domain(const MeshCfg *m, const Stl *s, double lo[3], double hi[3]);   /* the meshed box */
extern const char *const DIR_NAME[DIR__N];         /* "+X" ... */
int    mesher_up(const MeshCfg *m);                /* the up direction, made square to the flow when it isn't */
int    stl_thin_axis(const Stl *s);                /* the axis the part is thinnest along (a 2-D profile's extrusion) */
double mesher_slab(const MeshCfg *m, const Stl *s, double *lo, double *hi);   /* 2-D: the slab across normal2D; its thickness */
bool   mesher_cap_tri(const MeshCfg *m, const float *tri);         /* 2-D: a triangle of the extrusion's end caps (not meshed) */
bool   mesher_region_meshed(const MeshCfg *m, const Stl *s, int r); /* false for a 2-D end cap: it is no boundary of the mesh */
double mesher_ref_length(const MeshCfg *m, const Stl *s);   /* the length the tunnel is measured in, m */
/* snappyHexMesh, external 3-D: the far field's cells are 2^K base sizes (0 to 3; 0 inside, in 2-D, for SALOME), and a box
   round the body and its wake (mesher_near_box: half a length round it, two behind) is refined to the base size */
int  mesher_far_levels(const MeshCfg *m, const Stl *s);
void mesher_near_box(const MeshCfg *m, const Stl *s, double lo[3], double hi[3]);
/* the prism layers' total thickness as a fraction of a surface cell at the target size (snappyHexMesh's relativeSizes; SALOME's
   script, assets/salome/stl_mesher.py layer_total, applies the same rule to its layers),
   from prismPct, and their outer layer's (*outer, may be NULL). The outer layer is held between these fractions of the
   cell next to it: thinner, the cells there fail the quality checks (determinant, interpolation weights) and snappy
   removes the layers; thicker, the medial-axis ones. So the total can differ from prismPct: the page says so. */
#define LAYER_OUTER_MIN 0.3
#define LAYER_OUTER_MAX 1.0
double mesher_layer_rel(const MeshCfg *m, double *outer);
double mesher_surface_cell(const MeshCfg *m);      /* a surface cell at the target size, m (snappyHexMesh: base / 2^level) */
bool   mesher_even_layers(const MeshCfg *m);       /* snappy with layers: the layered walls one surface size (MeshCfg.evenLayers) */
double mesher_layer_cell(const MeshCfg *m);        /* the surface cell the layers sit on: the minimum size when even, else the target */

/* writes constant/triSurface/geometry.stl, mesher/regions/<name>.stl, mesher/mesher.ini, salome/stl_mesh.py, the minimal
   system dictionaries a case needs and, for snappyHexMesh, its dictionaries (blockMesh, surfaceFeatureExtract, snappy,
   meshQuality: a file of the user's is kept as <name>.orig first); false with err */
bool mesher_prepare(const char *casePath, const MeshCfg *m, Stl *s, char *err, int cap);
/* 2-D, after snappyHexMesh: the shell lines that make one cell of the slab's front face (extrudeMesh), as deep as the part
   and moved onto it, with front2D and back2D empty */
void mesher_2d_script(const MeshCfg *m, const Stl *s, Str *out);
/* the shell lines that give each boundary its type after a conversion (ideasUnvToFoam): foamDictionary on the boundary */
void mesher_type_script(const MeshCfg *m, const Stl *s, Str *out);

#endif
