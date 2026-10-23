/* test_tutorials.c -- which tutorials must be copied with their whole group (B-05). A case that runs on its own is
 * copied alone; one that a sibling feeds (mapFields from it, a mesh merged from it, a parent Allrun.pre or a multi-world
 * mpirun) is copied with the group. Expectations checked against the v2606 tutorials' Allrun scripts.
 *
 *   test_tutorials <$FOAM_TUTORIALS>
 */
#include <stdio.h>
#include "foamcase.h"

static const struct { const char *rel; bool group; } CASES[] = {
    { "incompressible/icoFoam/cavity/cavity", false },               /* the first case; the others map from it */
    { "incompressible/icoFoam/cavity/cavityClipped", true },         /* mapFields cavity -> cavityClipped (parent Allrun) */
    { "incompressible/icoFoam/cavity/cavityGrade", true },
    { "incompressible/pimpleFoam/LES/periodicHill/steadyState", false },
    { "incompressible/pimpleFoam/LES/periodicHill/transient", true },  /* its Allrun maps from ../steadyState */
    { "incompressible/pimpleFoam/LES/surfaceMountedCube/initChannel", false },
    { "incompressible/pimpleFoam/LES/surfaceMountedCube/fullCase", true },
    { "compressible/acousticFoam/obliqueAirJet/precursor", true },    /* the parent runs main/Allrun.pre first */
    { "compressible/acousticFoam/obliqueAirJet/main", true },
    { "basic/overPotentialFoam/cylinder/cylinderAndBackground", true },  /* mergeMeshes ../cylinderMesh */
    { "multiphase/overInterDyMFoam/floatingBody/background", true },   /* the group's Allrun.pre builds it */
    { "multiphase/compressibleInterFoam/laminar/waterCooler/fluid", true },  /* multi-world mpirun */
    { "lagrangian/icoUncoupledKinematicParcelFoam/hopper/hopperInitialState", false },
    { "lagrangian/icoUncoupledKinematicParcelFoam/hopper/hopperEmptying", true },
    { "mesh/snappyHexMesh/flange", false },                            /* a collection: the first one runs alone */
    { "basic/laplacianFoam/multiWorld1/left", true },                   /* ../common */
    { "incompressible/simpleFoam/pitzDaily", false },                   /* not in a group at all */
};

int main(int argc, char **argv)
{
    if (argc < 2) { printf("usage: test_tutorials <tutorials dir>\n"); return 2; }
    int fails = 0, n = 0;
    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        char p[PL_PATH]; pl_join(p, sizeof p, argv[1], CASES[i].rel);
        if (!pl_is_dir(p)) { printf("skip  %s (not in this OpenFOAM)\n", CASES[i].rel); continue; }
        n++;
        char why[300] = "";
        bool g = tutorial_needs_group(p, why, sizeof why);
        bool ok = g == CASES[i].group;
        printf("%s  %-70s %s%s%s\n", ok ? "ok  " : "FAIL", CASES[i].rel, g ? "group" : "alone", why[0] ? ": " : "", why);
        if (!ok) fails++;
    }
    printf("%d tutorials, %d wrong\n", n, fails);
    return fails ? 1 : 0;
}
