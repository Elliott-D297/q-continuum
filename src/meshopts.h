/* meshopts.h -- every setting OpenFOAM's meshers take, for the STL mesher's Advanced panel (user request, 2026-11-13: "when
 * I check that I want advanced meshing, give me access to literally every thing I could change in a mesh that OpenFOAM
 * will allow").
 *
 * MESH_OPTS is the table of the keywords of each dictionary the STL mesher writes (snappyHexMeshDict, meshQualityDict,
 * blockMeshDict, surfaceFeatureExtractDict, extrudeMeshDict, cfMesh's meshDict, foamyHexMeshDict, collapseDict,
 * foamyQuadMeshDict, extrude2DMeshDict) and of the programs' command-line options, each checked against the v2606 source,
 * $FOAM_ETC/caseDicts/annotated, and for cfMesh the strings in libmeshLibrary.so (no cfMesh source is installed).
 *
 * The user's values ("overrides") live in <case>/mesher/advanced.ini, one a line:
 *     system/snappyHexMeshDict castellatedMeshControls/maxLocalCells = 5000000
 *     system/snappyHexMeshDict castellatedMeshControls/limitRegions = !remove
 *     cmd:polyDualMesh -splitAllFaces = true
 * and Generate applies them to each dictionary it writes (foamdict.c: the rest of the text stays as written). Any keyword
 * may be set, in the table or not: the table only gives the ones it knows a widget, words and a default. */
#ifndef MESHOPTS_H
#define MESHOPTS_H

#include <stdbool.h>
#include "pl_util.h"

/* when a keyword counts: which dictionaries the mesher in use writes */
enum {
    MW_SNAPPY   = 1 << 0,   /* snappyHexMesh (2-D too)                                  */
    MW_SNAPPY2D = 1 << 1,   /* snappyHexMesh in 2-D: extrudeMesh makes it one cell deep */
    MW_CF       = 1 << 2,   /* cfMesh (cartesianMesh, pMesh, tetMesh, cartesian2DMesh)  */
    MW_FOAMY    = 1 << 3,   /* foamyHexMesh                                             */
    MW_FOAMY2D  = 1 << 4,   /* foamyQuadMesh + extrude2DMesh                            */
    MW_QUALITY  = 1 << 5,   /* meshQualityDict: snappy, foamyHexMesh                    */
    MW_FEAT     = 1 << 6,   /* surfaceFeatureExtractDict: snappy, foamyQuadMesh         */
    MW_POLY     = 1 << 7,   /* polyDualMesh afterwards                                  */
    MW_ANY      = 1 << 8,   /* every mesher (checkMesh)                                 */
};
typedef enum { MO_BOOL = 0, MO_INT, MO_REAL, MO_WORD, MO_VEC, MO_TEXT } MOType;
enum {
    MO_PATCH  = 1 << 0,     /* path holds %s: each boundary (the part's, and a tunnel's)            */
    MO_REGION = 1 << 1,     /* path holds %s: each refinement region (zone0..., near)               */
    MO_PART   = 1 << 2,     /* path holds @: the first sub-dictionary there (foamy's part control)   */
};

typedef struct {
    unsigned    when;       /* MW_* */
    const char *file;       /* "system/snappyHexMeshDict", or "cmd:<program>"                          */
    const char *path;       /* '/' between keys; for cmd: the option ("-splitAllFaces", "featureAngle" the
                               positional argument, "extra" anything else typed)                         */
    const char *group;      /* the heading it is listed under                                          */
    const char *label;      /* plain words                                                             */
    const char *help;       /* what it does, in fluid-mechanics words where it can                     */
    MOType      type;
    const char *choices;    /* MO_BOOL: "true|false" (or "on|off", "yes|no", "1|0": the words written);
                               MO_WORD: the words, '|' between                                         */
    const char *dflt;       /* OpenFOAM's value when the keyword isn't there (NULL: none, or required)  */
    unsigned    flags;      /* MO_PATCH, MO_REGION, MO_PART */
} MeshOpt;

extern const MeshOpt MESH_OPTS[];
extern const int MESH_OPTS_N;

/* the user's values */
typedef struct { char file[64]; char path[256]; char value[512]; } MOSet;
typedef struct { MOSet *s; int n, cap; } MeshOverrides;

#define MO_REMOVE "!remove"
#define MO_INI "mesher/advanced.ini"

void        mo_init(MeshOverrides *o);
void        mo_free(MeshOverrides *o);
bool        mo_load(const char *casePath, MeshOverrides *o);      /* false: no file (o empty) */
bool        mo_save(const char *casePath, const MeshOverrides *o);
bool        mo_parse(const char *text, MeshOverrides *o);         /* the file's text */
void        mo_text(const MeshOverrides *o, Str *out);
const char *mo_get(const MeshOverrides *o, const char *file, const char *path);   /* NULL: not set */
void        mo_set(MeshOverrides *o, const char *file, const char *path, const char *value);   /* NULL / "": unset */
int         mo_count(const MeshOverrides *o, unsigned activeMask);   /* how many apply to the mesher in use (all
                                                                         free ones count) */
/* the user's values onto one dictionary's text (in place): false with err when a value can't be put (the text is
   then as far as it got) */
bool        mo_apply(const MeshOverrides *o, const char *file, Str *text, char *err, int cap);
/* a value fit for the keyword? (err: why not) */
bool        mo_check(const MeshOpt *op, const char *value, char *err, int cap);
/* a value for any keyword: balanced brackets and quotes, no ';' outside braces */
bool        mo_check_text(const char *value, char *err, int cap);
/* the mask for a mesher (stlmesh.h MESHER_*), 2-D or not, polyhedra afterwards or not */
unsigned    mo_active_mask(int mesher, bool twoD, bool polyConvert);
/* a program's options: the flag on (MO_BOOL) or its value; dflt when the user set none */
bool        mo_cmd_flag(const MeshOverrides *o, const char *prog, const char *flag, bool dflt);
const char *mo_cmd_value(const MeshOverrides *o, const char *prog, const char *key, const char *dflt);
/* the user's extra options for a program, checked (no ; & | < > ` $ or newlines); "" none */
const char *mo_cmd_extra(const MeshOverrides *o, const char *prog);
bool        mo_safe_args(const char *s);
/* the overrides that change the mesh (all but checkMesh's), for mesher/mesh.sig: "" when there are none */
void        mo_sig_text(const char *casePath, Str *out);

#endif
