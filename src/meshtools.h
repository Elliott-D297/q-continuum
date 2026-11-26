/* meshtools.h -- Mesh > Tools (mesh overhaul plan step 10, gaps E6 E7): OpenFOAM's everyday mesh utilities as forms. Each
 * tool is a few numbers turned into the commands to run (and, where the utility reads one, its dictionary, written as
 * system/<name>Dict.qc-tool so the case's own is left alone); renaming a patch is done here directly, in the mesh and in
 * every field file. Every keyword checked against v2606's -help and tutorials. Plain C: page_meshtools.c is the panel,
 * test_meshtools checks the text. */
#ifndef MESHTOOLS_H
#define MESHTOOLS_H

#include <stdbool.h>
#include "pl_util.h"

enum { MT_TRANSFORM = 0,   /* transformPoints: scale, turn, move                                                 */
       MT_MIRROR,          /* mirrorMesh about a plane: half a symmetric geometry made whole                     */
       MT_REFINE,          /* refineMesh -all: every cell split in two each way                                  */
       MT_RENUMBER,        /* renumberMesh: cells reordered so the solver's matrix is narrow (faster runs)       */
       MT_ZONE,            /* topoSet: the cells in a box as a cellZone (a porous region, a fan, MRF)             */
       MT_MERGE,           /* createPatch: several patches as one                                                */
       MT_BAFFLES,         /* createBaffles: a faceZone made into two-sided walls (a thin plate)                 */
       MT_SURFACE,         /* surfaceCheck on an STL: closed, its quality                                        */
       MT__N };
extern const char *const MT_NAME[MT__N];
extern const char *const MT_WHAT[MT__N];

typedef struct {
    int    tool;
    double scale, rotAxis[3], rotDeg, move[3];          /* MT_TRANSFORM: done in that order          */
    double point[3], normal[3];                         /* MT_MIRROR                                 */
    double lo[3], hi[3]; char name[64];                 /* MT_ZONE: the box, the zone's name         */
    char   patches[512]; char type[32];                 /* MT_MERGE: names, space-separated; name; type */
    char   zone[64];                                    /* MT_BAFFLES: the faceZone (name: the walls) */
    char   file[PL_PATH];                               /* MT_SURFACE: the STL                       */
} MtIn;

/* the commands (run in the case folder) and, when the utility reads a dictionary, its text and where it goes; false with err */
bool mt_script(const MtIn *in, char *cmd, int ccap, char *dictRel, int dcap, char *dict, int dictCap, char *err, int ecap);

/* a patch renamed in constant/polyMesh/boundary and in the boundaryField of every field in 0/ and 0.orig/; false with err
   (no such patch, the new name taken or not a valid word). files: how many files were changed */
bool mt_rename_patch(const char *casePath, const char *from, const char *to, int *files, char *err, int ecap);
/* the patches of constant/polyMesh/boundary: names and types, how many */
int  mt_patches(const char *casePath, char names[][64], char types[][32], int max);
/* the faceZones of constant/polyMesh/faceZones (their names), how many */
int  mt_face_zones(const char *casePath, char names[][64], int max);
/* a valid OpenFOAM word for a patch or zone name: letters, digits, _ - . and not starting with a digit */
bool mt_word_ok(const char *w);

#endif
