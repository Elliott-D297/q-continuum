/* test_xsection.c -- the polyMesh reader behind Mesh > View (src/xsection.c) reads every faces layout OpenFOAM writes:
   ASCII faceList, binary faceCompactList (blockMesh/snappy) and binary faceList with raw labels inside each "N(...)" (cfMesh) */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "xsection.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const double P[8][3] = { {0,0,0}, {1,0,0}, {1,1,0}, {0,1,0}, {0,0,1}, {1,0,1}, {1,1,1}, {0,1,1} };
static const int32_t F[6][4] = { {0,3,2,1}, {4,5,6,7}, {0,1,5,4}, {2,3,7,6}, {0,4,7,3}, {1,2,6,5} };

static FILE *header(const char *dir, const char *obj, const char *cls, bool binary)
{
    char p[1024]; snprintf(p, sizeof p, "%s/%s", dir, obj);
    FILE *f = fopen(p, "wb");
    fprintf(f, "FoamFile\n{\n    version 2.0;\n    format %s;\n    arch \"LSB;label=32;scalar=64\";\n    class %s;\n    object %s;\n}\n\n",
            binary ? "binary" : "ascii", cls, obj);
    return f;
}

static void write_labels(FILE *f, const int32_t *v, int n, bool binary)
{
    fprintf(f, "%d\n(", n);
    if (binary) fwrite(v, 4, (size_t)n, f);
    else for (int i = 0; i < n; i++) fprintf(f, "%s%d", i ? " " : "", v[i]);
    fprintf(f, ")\n");
}

/* layout 0 ASCII faceList, 1 binary faceCompactList, 2 binary faceList (cfMesh) */
static void write_cube(const char *dir, int layout)
{
    mkdir(dir, 0755);
    bool bin = layout != 0;
    FILE *f = header(dir, "points", "vectorField", bin);
    fprintf(f, "8\n(");
    if (bin) fwrite(P, sizeof P, 1, f);
    else for (int i = 0; i < 8; i++) fprintf(f, "(%g %g %g)\n", P[i][0], P[i][1], P[i][2]);
    fprintf(f, ")\n"); fclose(f);

    f = header(dir, "faces", layout == 1 ? "faceCompactList" : "faceList", bin);
    if (layout == 1) {
        int32_t off[7]; for (int i = 0; i < 7; i++) off[i] = 4 * i;
        write_labels(f, off, 7, true);
        write_labels(f, &F[0][0], 24, true);
    } else {
        fprintf(f, "\n6\n(\n");
        for (int i = 0; i < 6; i++) {
            fprintf(f, "4\n(");
            if (bin) fwrite(F[i], 4, 4, f);
            else fprintf(f, "%d %d %d %d", F[i][0], F[i][1], F[i][2], F[i][3]);
            fprintf(f, ")\n");
        }
        fprintf(f, ")\n");
    }
    fclose(f);

    int32_t own[6] = {0, 0, 0, 0, 0, 0};
    f = header(dir, "owner", "labelList", bin); write_labels(f, own, 6, bin); fclose(f);
    f = header(dir, "neighbour", "labelList", bin); fprintf(f, "0\n()\n"); fclose(f);
    f = header(dir, "boundary", "polyBoundaryMesh", false);
    fprintf(f, "1\n(\n    walls\n    {\n        type wall;\n        nFaces 6;\n        startFace 0;\n    }\n)\n");
    fclose(f);
}

int main(int argc, char **argv)
{
    const char *root = argc > 1 ? argv[1] : "/tmp";
    static const char *names[3] = { "ascii_faceList", "binary_faceCompactList", "binary_faceList" };
    for (int l = 0; l < 3; l++) {
        char dir[1024]; snprintf(dir, sizeof dir, "%s/xsection_%s", root, names[l]);
        write_cube(dir, l);
        MeshSurf m = {0};
        bool ok = ms_load(&m, dir, 1000);
        CHECK(ok, "%s: ms_load: %s", names[l], m.err);
        CHECK(m.nCells == 1 && m.nFaces == 6 && m.nPoints == 8, "%s: cells %ld faces %ld points %ld", names[l], m.nCells, m.nFaces, m.nPoints);
        ms_free(&m);
        for (int a = 0; a < 3; a++) {
            XSection x = {0};
            ok = xs_load_slice(&x, dir, a, 0.5f);
            CHECK(ok, "%s: slice %d: %s", names[l], a, x.err);
            xs_free(&x);
        }
    }
    if (!fails) printf("xsection: all faces layouts read\n");
    return fails ? 1 : 0;
}
