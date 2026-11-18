/* festudy.h -- a part study for CalculiX (docs/FEA_PLAN.md, 2026-11-15): the part and how it is meshed, the material,
 * the fixtures and the loads, each on faces of the part (FeMesh groups, by name), kept in <project>/study.ini; and the
 * CalculiX input written from them: study_mesh.inp (nodes, elements, the fixtures' node sets) and study.inp (material,
 * step, supports, loads as consistent nodal forces). Millimetres, newtons, MPa, tonnes per mm³. */
#ifndef FESTUDY_H
#define FESTUDY_H
#include <stdbool.h>
#include "femesh.h"

#define FE_MAXITEM 16
#define FE_MAXFACES 64
enum { FX_FIXED = 0, FX_X, FX_Y, FX_Z, FX__N };              /* held in x, y and z; in x only ...           */
enum { LD_FORCE = 0, LD_PRESSURE, LD__N };
enum { DIR_NORMAL = 0, DIR_PX, DIR_PY, DIR_PZ, DIR_MX, DIR_MY, DIR_MZ, DIR__N };   /* normal: pushing into the face */
typedef struct { int kind; int nf; char face[FE_MAXFACES][64]; } FeFixture;
typedef struct { int kind; int nf; char face[FE_MAXFACES][64]; double value; int dir; } FeLoad;   /* value: N, or MPa */
typedef struct {
    char   part[256];          /* the part's file, in the project folder                         */
    double scale;              /* mm per unit of a file without one (STL, BREP, MED, UNV)        */
    double size;               /* element size, mm (0: a twentieth of the part)                  */
    int    order;              /* 2 quadratic, 1 linear                                          */
    char   material[64];       /* a CCX_LIB name                                                 */
    int    nFix, nLoad;
    FeFixture fix[FE_MAXITEM];
    FeLoad    load[FE_MAXITEM];
} FeStudy;

extern const char *const FE_FX_NAME[FX__N];
extern const char *const FE_LD_NAME[LD__N];
extern const char *const FE_DIR_NAME[DIR__N];

void fe_study_default(FeStudy *s);
bool fe_study_load(const char *proj, FeStudy *s);          /* false: no study.ini */
bool fe_study_save(const char *proj, const FeStudy *s);
int  fe_group(const FeMesh *m, const char *name);           /* -1 when the mesh has no face of that name */
/* the totals the loads come to: the force (N), the fixed nodes; and what's missing ("" when it can run) */
typedef struct { double force[3]; int fixedNodes, loadedNodes, unknownFaces; char problem[300]; } FeSummary;
void fe_study_summary(const FeStudy *s, const FeMesh *m, FeSummary *out);
bool fe_study_write(const FeStudy *s, const FeMesh *m, const char *proj, char *err, int cap);
#endif
