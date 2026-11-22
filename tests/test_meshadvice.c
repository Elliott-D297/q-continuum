/* test_meshadvice.c -- "What mesh do I need?" (mesh overhaul plan step 7, meshadvice.c): each kind of geometry leads to
   the path the comparison of meshers and the block builder's templates support, the first cell is the Calculators' own,
   the cell counts follow the goal, and every answer explains itself.
     test_meshadvice */
#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "meshadvice.h"
#include "calc.h"
#include "stlmesh.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); fails++; } } while (0)

/* bmesh.h's template numbers (bmesh.h can't come in beside stlmesh.h): BT_CAVITY 0 ... BT_WEDGE 7 */
enum { T_CAVITY = 0, T_CHANNEL, T_STEP, T_PLATE, T_PIPE, T_CYLINDER, T_AIRFOIL, T_WEDGE };

static MaIn base(int shape)
{
    MaIn in = { .shape = shape, .dims = MA_3D, .flow = MA_INTERNAL, .walls = MA_WALLS_FUNC, .goal = MA_ANSWER, .file = MA_FILE_STL,
                .from = MA_FROM_GMSH, .salome = true, .U = 10, .L = 1, .nu = 1.516e-5 };
    return in;
}

static bool says(const MaOut *o, const char *what)
{
    for (int i = 0; i < o->nSteps; i++) if (strstr(o->step[i].r, what) || strstr(o->step[i].q, what)) return true;
    return strstr(o->title, what) != NULL;
}

int main(void)
{
    CHECK((int)MA_MESHER_SNAPPY == (int)MESHER_SNAPPY && (int)MA_MESHER_TET == (int)MESHER_TET, "meshadvice's mesher numbers are stlmesh.h's");
    MaOut o;

    /* shapes made of blocks: the block builder, its template */
    static const struct { int shape, dims, tmpl; } B[] = {
        { MA_BOX, MA_2D, T_CAVITY }, { MA_BOX, MA_3D, T_CHANNEL }, { MA_PIPE, MA_3D, T_PIPE }, { MA_PIPE, MA_AXI, T_WEDGE }, { MA_PIPE, MA_2D, T_WEDGE },
        { MA_CHANNEL, MA_2D, T_CHANNEL }, { MA_STEP, MA_2D, T_STEP }, { MA_PLATE, MA_2D, T_PLATE }, { MA_CYLINDER, MA_2D, T_CYLINDER },
        { MA_AIRFOIL, MA_2D, T_AIRFOIL } };
    for (size_t i = 0; i < sizeof B / sizeof B[0]; i++) {
        MaIn in = base(B[i].shape); in.dims = B[i].dims;
        ma_advise(&in, &o);
        CHECK(o.go == MA_GO_BLOCKS && o.tmpl == B[i].tmpl, "%s (%d): block builder template %d, got go %d template %d", MA_SHAPE_NAME[B[i].shape], B[i].dims, B[i].tmpl, o.go, o.tmpl);
        CHECK(says(&o, "blockMesh"), "%s: explains blockMesh", MA_SHAPE_NAME[B[i].shape]);
    }
    /* axisymmetric asked for a shape that isn't round: the plane, and why */
    { MaIn in = base(MA_BOX); in.dims = MA_AXI; ma_advise(&in, &o); CHECK(o.tmpl == T_CAVITY && says(&o, "same all round an axis"), "an axisymmetric box: 2-D, explained"); }
    /* a 2-D template asked for in 3-D: extruded */
    { MaIn in = base(MA_AIRFOIL); ma_advise(&in, &o); CHECK(says(&o, "Extrude") && strstr(o.title, "extruded"), "a 3-D airfoil: extruded"); }

    /* a CAD part: snappyHexMesh; a 2-D profile with SALOME: its triangles; a STEP without SALOME: says to install it */
    { MaIn in = base(MA_PART); ma_advise(&in, &o); CHECK(o.go == MA_GO_STL && o.mesher == MESHER_SNAPPY, "an STL part: snappyHexMesh"); }
    { MaIn in = base(MA_PART); in.dims = MA_2D; ma_advise(&in, &o); CHECK(o.mesher == MESHER_TET, "a 2-D profile with SALOME: its triangles (got %d)", o.mesher); }
    { MaIn in = base(MA_PART); in.dims = MA_2D; in.salome = false; ma_advise(&in, &o); CHECK(o.mesher == MESHER_SNAPPY && says(&o, "2-D mode"), "a 2-D profile without SALOME: snappy's 2-D mode"); }
    { MaIn in = base(MA_PART); in.file = MA_FILE_STEP; in.salome = false; ma_advise(&in, &o); CHECK(says(&o, "install_salome"), "a STEP without SALOME: how to read it"); }
    { MaIn in = base(MA_PART); in.dims = MA_AXI; ma_advise(&in, &o); CHECK(says(&o, "wedge") && strstr(o.title, "wedge"), "an axisymmetric part: a wedge"); }
    { MaIn in = base(MA_PART); in.flow = MA_EXTERNAL; ma_advise(&in, &o); CHECK(says(&o, "wind tunnel"), "flow round a part: the tunnel"); }

    /* a mesh made elsewhere */
    { MaIn in = base(MA_HAVE); in.from = MA_FROM_GMSH; ma_advise(&in, &o); CHECK(o.go == MA_GO_IMPORT && says(&o, "gmshToFoam"), "Gmsh: import"); }
    { MaIn in = base(MA_HAVE); in.from = MA_FROM_SALOME; ma_advise(&in, &o); CHECK(o.go == MA_GO_IMPORT && says(&o, "ideasUnvToFoam"), "SALOME: import"); }
    { MaIn in = base(MA_HAVE); in.from = MA_FROM_FLUENT; ma_advise(&in, &o); CHECK(o.go == MA_GO_IMPORT && strstr(o.cmd, "fluent3DMeshToFoam"), "Fluent: imported (fluent3DMeshToFoam)"); }
    { MaIn in = base(MA_HAVE); in.from = MA_FROM_STAR; ma_advise(&in, &o); CHECK(o.go == MA_GO_IMPORT && strstr(o.cmd, "star4ToFoam"), "STAR: imported (star4ToFoam)"); }

    /* the walls: no layers, wall functions at y+ 50, resolved at y+ 1, and the first cell is the Calculators' */
    { MaIn in = base(MA_PART); in.walls = MA_WALLS_NO; ma_advise(&in, &o); CHECK(!o.layers && o.h1 == 0, "walls that don't matter: no layers"); }
    {
        MaIn in = base(MA_PART); in.flow = MA_EXTERNAL; in.walls = MA_WALLS_RESOLVE; ma_advise(&in, &o);
        CalcWallIn wi = { .flow = CW_PLATE, .rho = 1, .mu = in.nu, .U = in.U, .L = in.L, .yplus = 1, .r = 1.2 };
        CalcWallOut wo; Working wk; calc_wall(&wi, &wo, &wk);
        CHECK(o.layers && o.yplus == 1 && fabs(o.h1 - wo.h1) < 1e-15, "resolved walls: y+ 1, the Calculators' first cell (%g, %g)", o.h1, wo.h1);
        CHECK(fabs(o.Re - 10 / 1.516e-5) < 1, "Re");
    }
    { MaIn in = base(MA_PIPE); in.walls = MA_WALLS_FUNC; ma_advise(&in, &o); CHECK(o.yplus == 50 && o.h1 > 0, "wall functions: y+ 50"); }
    { MaIn in = base(MA_PIPE); in.U = 0.01; in.L = 0.01; ma_advise(&in, &o); CHECK(says(&o, "laminar"), "Re 7 in a pipe: says laminar"); }
    { MaIn in = base(MA_PIPE); in.U = 0; ma_advise(&in, &o); CHECK(o.h1 == 0 && o.Re == 0 && says(&o, "Give the speed"), "no speed: no first cell, says so"); }

    /* the size: more cells for a better answer, three meshes for a study, 2-D far fewer than 3-D */
    double c[3];
    for (int g = 0; g < 3; g++) { MaIn in = base(MA_PART); in.goal = g; ma_advise(&in, &o); c[g] = o.cells; if (g == MA_INDEP) CHECK(o.nMeshes == 3 && says(&o, "Grid Convergence Index"), "a study: three meshes, GCI"); }
    CHECK(c[0] < c[1] && c[1] < c[2], "cells grow with the goal: %g %g %g", c[0], c[1], c[2]);
    { MaIn in = base(MA_CHANNEL); in.dims = MA_2D; ma_advise(&in, &o); double c2 = o.cells; in.dims = MA_3D; ma_advise(&in, &o); CHECK(c2 * 10 < o.cells, "2-D far fewer cells than 3-D"); }

    /* every answer explains itself, out-of-range answers don't crash */
    for (int s = 0; s < MA__SHAPES; s++) for (int d = 0; d < 3; d++) for (int w = 0; w < MA__WALLS; w++) for (int g = 0; g < MA__GOALS; g++) for (int f = 0; f < 2; f++) {
        MaIn in = base(s); in.dims = d; in.walls = w; in.goal = g; in.flow = f; in.salome = (s + d) % 2;
        ma_advise(&in, &o);
        CHECK(o.nSteps >= 1 && o.title[0], "shape %d dims %d walls %d goal %d: no answer", s, d, w, g);
        CHECK(o.go != MA_GO_BLOCKS || (o.tmpl >= 0 && o.tmpl <= T_WEDGE), "a template in range");
        CHECK(o.go != MA_GO_STL || o.mesher >= 0, "a mesher");
    }
    { MaIn in = base(99); in.dims = -4; in.walls = 7; in.goal = -1; ma_advise(&in, &o); CHECK(o.title[0], "nonsense answers still advise"); }

    if (fails) { fprintf(stderr, "%d failures\n", fails); return 1; }
    printf("meshadvice: all checks passed\n");
    return 0;
}
