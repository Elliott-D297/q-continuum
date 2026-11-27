/* chtsetup.h -- conjugate heat transfer set up from choices (improvement plan A2, 2026-11-26): the mesh's cellZones become
 * regions, each a fluid or a solid of a material from a library, with a starting temperature and (a solid) the heat it
 * makes; the outer patches are walls (adiabatic, a temperature, a heat flux, convection to surroundings), inlets (a speed
 * and a temperature) or outlets. Kept in <case>/cht.ini; cht_write turns it into constant/regionProperties, each region's
 * constant/<r>/ and system/<r>/ dictionaries and 0.orig/<r>/ fields for chtMultiRegionSimpleFoam (steady) or
 * chtMultiRegionFoam (transient). The Run page then splits the mesh (topoSet for the cells outside every zone, then
 * splitMeshRegions -cellZones) and restores 0.orig, as it does for the tutorials.
 *
 * The fields are written with patch-name patterns, so no split is needed first: ".*" a region's default (walls: no slip,
 * adiabatic), "<region>_to_.*" the interfaces splitMeshRegions makes (temperature coupled both ways), and each outer patch
 * by name (a patch a region doesn't have is ignored by OpenFOAM). Every keyword is from the v2606 tutorials
 * (snappyMultiRegionHeater, multiRegionHeaterRadiation, heatExchanger, cpuCabinet) and sources. */
#ifndef CHTSETUP_H
#define CHTSETUP_H
#include <stdbool.h>

#define CHT_MAX_R 16
#define CHT_MAX_P 64
#define CHT_REST "fluid"                 /* the default name of the cells outside every zone */

enum { CR_FLUID = 0, CR_SOLID, CR__N };
enum { CP_WALL = 0, CP_INLET, CP_OUTLET, CP__N };              /* an outer patch's role (a solid's patches are walls) */
enum { CT_ADIABATIC = 0, CT_FIXED, CT_FLUX, CT_CONVECT, CT__N }; /* a wall's heat                                     */

typedef struct { const char *name; double rho, cp, k, mu; bool gas; double molW; } ChtMat;   /* SI; mu fluids only */
extern const ChtMat CHT_FLUIDS[], CHT_SOLIDS[];
extern const int CHT_NFLUIDS, CHT_NSOLIDS;
extern const char *const CHT_ROLE_NAME[CP__N], *const CHT_HEAT_NAME[CT__N], *const CHT_KIND_NAME[CR__N];

typedef struct {
    char   name[64];        /* the cellZone (or CHT_REST, the cells in no zone)                 */
    bool   rest;            /* the cells in no zone, made a zone by topoSet before the split    */
    int    kind;            /* CR_*                                                             */
    int    mat;             /* index into CHT_FLUIDS or CHT_SOLIDS                              */
    double T0;              /* starting temperature, K                                          */
    double U0[3];           /* a fluid's starting velocity, m/s                                 */
    double Q;               /* a solid's heat made, W (the whole region)                        */
} ChtRegion;

typedef struct {
    char   name[64];        /* a patch of the base mesh                                         */
    int    role;            /* CP_*                                                             */
    double U;               /* an inlet's speed into the domain, m/s                            */
    double Tin;             /* an inlet's temperature, K                                        */
    int    heat;            /* a wall's CT_*                                                    */
    double T, q, h, Ta;     /* fixed K; flux W/m² into the region; convection W/(m² K) and K    */
} ChtPatch;

typedef struct {
    bool   transient;       /* chtMultiRegionFoam, else chtMultiRegionSimpleFoam                */
    bool   turbulent;       /* fluids: k-omega SST with wall functions, else laminar            */
    double g[3];            /* gravity, m/s² (0 0 0: none)                                      */
    double endTime;         /* steady: iterations; transient: seconds                           */
    double writeEvery;      /* steady: iterations; transient: seconds                           */
    int    nR, nP;
    ChtRegion r[CHT_MAX_R];
    ChtPatch  p[CHT_MAX_P];
} ChtSetup;

/* the regions and patches the mesh offers: its cellZones (and the cells in none, when there are any), its outer patches
   (constraint types left out: empty, symmetry, wedge, cyclic, processor). False: no mesh or no cellZones */
bool cht_from_mesh(const char *casePath, ChtSetup *s, char *err, int cap);
/* cells outside every cellZone (-1: unknown) */
long cht_rest_cells(const char *casePath);
bool cht_load(const char *casePath, ChtSetup *s);       /* false: no cht.ini */
bool cht_save(const char *casePath, const ChtSetup *s);
bool cht_exists(const char *casePath);
int  cht_find_region(const ChtSetup *s, const char *name);
int  cht_find_patch(const ChtSetup *s, const char *name);
/* what is missing or wrong, one problem a line ("" when it can run); warnings (it runs, but ...) in warn */
void cht_check(const ChtSetup *s, char *problems, int pcap, char *warn, int wcap);
/* writes every file (the region dictionaries the user changed by hand since the last write are kept; their names go to
   kept). False with the reason in err */
bool cht_write(const char *casePath, const ChtSetup *s, char *kept, int kcap, char *err, int ecap);
/* a material's Prandtl number (fluids) and thermal diffusivity k / (rho cp), m²/s */
double cht_prandtl(const ChtMat *m);
double cht_diffusivity(const ChtMat *m);
#endif
