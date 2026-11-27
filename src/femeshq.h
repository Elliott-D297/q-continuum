/* femeshq.h -- a part mesh's quality and how many elements cross its walls (improvement plan B3, 2026-11-26): the
 * tetrahedra's aspect ratio (longest edge over the inscribed sphere's diameter x sqrt(6): 1 for a regular one) and, for
 * quadratic ones, how far the mid-side nodes bend them (the scaled Jacobian at the corners: 1 straight-sided, below 0.2
 * badly curved, below 0 folded); and the wall thickness found by a ray from boundary faces inward to the opposite wall. */
#ifndef FEMESHQ_H
#define FEMESHQ_H
#include "femesh.h"

typedef struct {
    int    n;                       /* tetrahedra checked                                             */
    double arMax, arMean;           /* aspect ratio                                                  */
    int    arOver10, arOver30;      /* poor (worse than 10), bad (worse than 30)                      */
    double jacMin;                  /* the least scaled Jacobian (quadratic: the mid-side nodes' bend) */
    int    jacBad;                  /* elements with one below 0.2                                   */
    int    worst;                   /* the element with the largest aspect ratio (-1 none)          */
} FeQuality;
void fe_quality(const FeMesh *m, FeQuality *q);

/* the walls: the thinnest place found (mm) and the 5th percentile, from up to 3000 boundary faces' inward rays; the
   elements across it at element size h: thin / h. False when no ray found an opposite wall (an open mesh) */
typedef struct { double thin, p5, mean; int rays; double at[3]; } FeThickness;
bool fe_thickness(const FeMesh *m, FeThickness *t);
#endif
