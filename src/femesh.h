/* femesh.h -- a part's solid mesh for CalculiX (docs/FEA_PLAN.md, 2026-11-15): read from the .qcmesh fea_mesh.py writes,
 * its elements put in CalculiX's node order (corners turned for a positive volume, mid-side nodes matched to their edges
 * by position, whatever order the mesher used), its boundary faces found (faces of one element only, outward), and those
 * grouped into the faces a user picks: the file's groups (CAD faces, a SALOME mesh's face groups), else by the angle
 * between neighbouring triangles. Millimetres. */
#ifndef FEMESH_H
#define FEMESH_H
#include <stdbool.h>

#define FE_MAXK 20                 /* nodes in an element, at most (C3D20) */
typedef struct {
    int     n;  int *id; double *x;            /* nodes: the file's ids, coordinates (3 each)                         */
    int     ne; unsigned char *ek; int *en;    /* elements: node count, nodes (FE_MAXK each, indices into the nodes)   */
    int     nf; unsigned char *fk; int *fn; int *fg; int *fe;   /* boundary faces: 3 4 6 8 nodes (8 each), group, element */
    unsigned char *fl;                         /* ... and which face of its element: CalculiX's face number, 1-4 (tet), 1-6 (brick),
                                                  1-5 (wedge): the k in *DFLUX Sk, *FILM Fk, *RADIATE Rk (plan B1) */
    int     ng; char (*gname)[64];
    double *garea, *gnorm, *gcen;              /* per group: area (mm²), mean outward normal, centre (3 each)          */
    bool   *gplanar;                           /* every triangle's normal within 2 degrees of the mean                 */
    double  lo[3], hi[3];
    bool    quadratic, fileGroups;
    int     nFlipped;                          /* elements whose corners were turned to a positive volume              */
} FeMesh;

bool femesh_load(const char *path, FeMesh *m, char *err, int cap);
void femesh_free(FeMesh *m);
const char *femesh_ccx_type(int k);            /* "C3D10" for 10 nodes ... (NULL: none) */
/* the area and outward unit normal of boundary face f (a quadrangle's from its two triangles) */
double femesh_face_area(const FeMesh *m, int f, double nrm[3]);
#endif
