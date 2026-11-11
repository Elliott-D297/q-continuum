/* test_meshverdict.c -- case_mesh_verdict (foamcase.c): checkMesh's failed checks read for what they amount to.
   Fixtures: a boundary file with two patches, a skewFaces set, and log.checkMesh texts. */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "foamcase.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void put(const char *root, const char *rel, const char *text)
{
    char p[1024]; snprintf(p, sizeof p, "%s/%s", root, rel);
    FILE *f = fopen(p, "w"); fputs(text, f); fclose(f);
}

#define HDR(cls, obj) "FoamFile\n{\n    version 2.0;\n    format ascii;\n    class " cls ";\n    object " obj ";\n}\n\n"
static const char *BOUNDARY = HDR("polyBoundaryMesh", "boundary")
    "2\n(\n    walls\n    {\n        type wall;\n        nFaces 50;\n        startFace 100;\n    }\n"
    "    Inlet_1\n    {\n        type patch;\n        nFaces 20;\n        startFace 150;\n    }\n)\n";
#define LOG(skew, tail) "Checking geometry...\n    Mesh non-orthogonality Max: 61.2 average: 8.1\n" skew tail

int main(int argc, char **argv)
{
    char root[900], d[1024];
    snprintf(root, sizeof root, "%s/meshverdict", argc > 1 ? argv[1] : "/tmp");
    mkdir(root, 0755);
    snprintf(d, sizeof d, "%s/constant", root); mkdir(d, 0755);
    snprintf(d, sizeof d, "%s/constant/polyMesh", root); mkdir(d, 0755);
    snprintf(d, sizeof d, "%s/constant/polyMesh/sets", root); mkdir(d, 0755);
    put(root, "constant/polyMesh/boundary", BOUNDARY);
    MeshVerdict v;

    /* 1: only boundary faces of the inlet are skew, under snappy's 20 */
    put(root, "constant/polyMesh/sets/skewFaces", HDR("faceSet", "skewFaces") "// comment\n3\n(\n151\n160\n169\n)\n");
    put(root, "log.checkMesh", LOG(" ***Max skewness = 11.350806, 3 highly skew faces detected which may impair the quality of the results\n",
                                   "\nFailed 1 mesh checks.\n\nEnd\n"));
    case_mesh_verdict(root, &v);
    CHECK(v.have && !v.ok && v.nFailed == 1, "have %d ok %d failed %d", v.have, v.ok, v.nFailed);
    CHECK(v.nSkew == 3 && v.nSkewBoundary == 3, "skew %ld boundary %ld", v.nSkew, v.nSkewBoundary);
    CHECK(v.boundarySkewOnly && !strcmp(v.skewPatch, "Inlet_1"), "boundary only %d patch [%s]", v.boundarySkewOnly, v.skewPatch);
    CHECK(v.maxSkew > 11.3 && v.maxSkew < 11.4, "max %g", v.maxSkew);

    /* 2: one of them is an internal face: a real failure */
    put(root, "constant/polyMesh/sets/skewFaces", HDR("faceSet", "skewFaces") "3\n(\n42\n160\n169\n)\n");
    case_mesh_verdict(root, &v);
    CHECK(!v.boundarySkewOnly && v.nSkewBoundary == 2, "internal face: only %d boundary %ld", v.boundarySkewOnly, v.nSkewBoundary);

    /* 3: boundary faces, but past what snappy allows */
    put(root, "constant/polyMesh/sets/skewFaces", HDR("faceSet", "skewFaces") "1\n(\n151\n)\n");
    put(root, "log.checkMesh", LOG(" ***Max skewness = 31.5, 1 highly skew faces detected which may impair the quality of the results\n",
                                   "\nFailed 1 mesh checks.\n\nEnd\n"));
    case_mesh_verdict(root, &v);
    CHECK(!v.boundarySkewOnly, "skewness 31.5 is not waved through");

    /* 4: another check failed as well */
    put(root, "log.checkMesh", LOG(" ***Max skewness = 11.3, 1 highly skew faces detected which may impair the quality of the results\n",
                                   " ***Zero or negative cell volume detected.  Minimum negative volume: -1e-11, Number of negative volume cells: 4\n"
                                   "\nFailed 2 mesh checks.\n\nEnd\n"));
    case_mesh_verdict(root, &v);
    CHECK(!v.boundarySkewOnly && v.nFailed == 2, "two failed checks: only %d n %d", v.boundarySkewOnly, v.nFailed);

    /* 5: a good mesh; 6: no log */
    put(root, "log.checkMesh", LOG("    Max skewness = 2.05 OK.\n", "\nMesh OK.\n\nEnd\n"));
    case_mesh_verdict(root, &v);
    CHECK(v.ok && v.nFailed == 0 && !v.boundarySkewOnly, "good mesh");
    snprintf(d, sizeof d, "%s/log.checkMesh", root); remove(d);
    case_mesh_verdict(root, &v);
    CHECK(!v.have, "no log");

    if (!fails) printf("meshverdict: ok\n");
    return fails ? 1 : 0;
}
