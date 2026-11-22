/* meshadvice.c -- "What mesh do I need?" (see meshadvice.h) */
#define _GNU_SOURCE
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "meshadvice.h"
#include "bmesh.h"
#include "calc.h"

/* stlmesh.h can't come in beside bmesh.h (both number their own BT_ values): the meshers chosen here, as stlmesh.h numbers
   them (its order is fixed by mesher.ini's history; test_meshadvice checks these against it) */
enum { MESHER_SNAPPY = MA_MESHER_SNAPPY, MESHER_TET = MA_MESHER_TET };
const char *mesher_short_name(int m);

const char *const MA_SHAPE_NAME[MA__SHAPES] = { "A box (a cavity, a duct, a room)", "A straight round pipe", "A channel or rectangular duct",
                                                "A backward-facing step", "A flat plate", "A cylinder in crossflow", "An airfoil",
                                                "Any other shape (a CAD part: STL or STEP)", "A mesh already made elsewhere" };
const char *const MA_FROM_NAME[MA__FROM] = { "Gmsh (.msh)", "SALOME (UNV, MED)", "Fluent (.msh)", "STAR-CD / STAR-CCM+ (exported as STAR-CD)", "Something else" };

static void step(MaOut *o, const char *q, const char *fmt, ...)
{
    if (o->nSteps >= MA_MAX_STEPS) return;
    snprintf(o->step[o->nSteps].q, sizeof o->step[0].q, "%s", q);
    va_list ap; va_start(ap, fmt);
    vsnprintf(o->step[o->nSteps].r, sizeof o->step[0].r, fmt, ap);
    va_end(ap);
    o->nSteps++;
}

static const char *cells_words(double n, char *b, size_t cap)
{
    if (n >= 1e6) snprintf(b, cap, "%.3g million", n / 1e6);
    else if (n >= 1e3) snprintf(b, cap, "%.0f thousand", round(n / 1e3));
    else snprintf(b, cap, "%.0f", n);
    return b;
}

void ma_advise(const MaIn *in, MaOut *o)
{
    memset(o, 0, sizeof *o);
    o->tmpl = -1; o->mesher = -1; o->nMeshes = 1;
    int shape = in->shape >= 0 && in->shape < MA__SHAPES ? in->shape : MA_PART;
    int dims = in->dims >= MA_3D && in->dims <= MA_AXI ? in->dims : MA_3D;
    bool ext = in->flow == MA_EXTERNAL;

    /* ---- the path, from the shape */
    static const int TMPL[MA__SHAPES] = { BT_CAVITY, BT_PIPE, BT_CHANNEL, BT_STEP, BT_PLATE, BT_CYLINDER, BT_AIRFOIL, -1, -1 };
    if (shape == MA_HAVE) {
        int f = in->from >= 0 && in->from < MA__FROM ? in->from : MA_FROM_OTHER;
        if (f == MA_FROM_GMSH || f == MA_FROM_SALOME) {
            o->go = MA_GO_IMPORT;
            snprintf(o->title, sizeof o->title, "Import it: Mesh > SALOME / import reads %s", f == MA_FROM_GMSH ? "Gmsh's .msh" : "SALOME's UNV and MED");
            step(o, MA_FROM_NAME[f], "Mesh > SALOME / import turns it into constant/polyMesh (%s) and names the walls. Check it afterwards on "
                 "Mesh > Quality: a mesh made elsewhere passes through [[checkmesh|checkMesh]] like any other.", f == MA_FROM_GMSH ? "gmshToFoam" : "ideasUnvToFoam");
        } else if (f == MA_FROM_FLUENT) {
            o->go = MA_GO_IMPORT;
            snprintf(o->cmd, sizeof o->cmd, "fluent3DMeshToFoam mesh.msh");
            snprintf(o->title, sizeof o->title, "Convert it with OpenFOAM's fluent3DMeshToFoam");
            step(o, MA_FROM_NAME[f], "OpenFOAM reads Fluent's ASCII .msh: fluent3DMeshToFoam for 3-D meshes (it keeps polyhedra), fluentMeshToFoam "
                 "for 2-D ones. Mesh > SALOME / import runs the right one on a Fluent .msh (or run it in the case folder yourself); then check the patch "
                 "types: Fluent's walls arrive as walls, its inlets and outlets as plain patches.");
        } else if (f == MA_FROM_STAR) {
            o->go = MA_GO_IMPORT;
            snprintf(o->cmd, sizeof o->cmd, "star4ToFoam mesh");
            snprintf(o->title, sizeof o->title, "Convert it with OpenFOAM's star4ToFoam");
            step(o, MA_FROM_NAME[f], "This OpenFOAM reads STAR-CD's .vrt / .cel / .bnd files (star4ToFoam); STAR-CCM+ exports them (File > Export, "
                 "STAR-CD format; mesh.vrt, mesh.cel and mesh.bnd make \"mesh\"). Its .ccm files need ccmToFoam, which this OpenFOAM was built without. "
                 "Mesh > SALOME / import reads the three files too; star4ToFoam takes them as millimetres.");
        } else {
            o->go = MA_GO_IMPORT;
            snprintf(o->title, sizeof o->title, "Export it as UNV or Gmsh .msh, then import it");
            step(o, MA_FROM_NAME[f], "Nearly every mesher writes UNV or Gmsh's .msh: Mesh > SALOME / import reads both. OpenFOAM also ships converters for "
                 "ANSYS (ansysToFoam), CFX-4, Gambit, Plot3D, TetGen, KIVA, Netgen and VTK, run in the case folder (Overview > Terminal).");
        }
        return;   /* the mesh is made: none of the rest applies */
    }
    if (shape == MA_PART) {
        o->go = MA_GO_STL;
        bool step_ = in->file == MA_FILE_STEP;
        if (dims == MA_2D && in->salome) {
            o->mesher = MESHER_TET;
            step(o, "A 2-D profile", "SALOME's triangles with [[prism-layers|layers]] all round the profile: tested on a NACA profile, it made the "
                 "fewest cells of all the meshers for the same wall resolution, with no [[refinement-transition|steps in size]] near the wall.");
        } else if (in->salome && step_ && !ext && in->walls != MA_WALLS_RESOLVE) {
            o->mesher = MESHER_SNAPPY;
            step(o, "A part from a STEP file", "snappyHexMesh still: it passed every check on curved parts in the comparison of meshers. SALOME reads "
                 "the STEP file's real faces (no faceting), which matters most with layers; here the walls don't need resolving, so the "
                 "robust choice wins.");
        } else if (in->salome && step_) {
            o->mesher = MESHER_SNAPPY;
            step(o, "A part from a STEP file", "snappyHexMesh, reading the [[cad|STEP]] through SALOME (exact faces, named regions): "
                 "[[hex|hexahedra]] in the body of the flow, [[prism-layers|prism layers]] on the walls. It was the only mesher that passed every "
                 "check on the curved flange in the comparison.");
        } else {
            o->mesher = MESHER_SNAPPY;
            step(o, step_ ? "A part from a STEP file" : "A part from an STL file",
                 "[[snappyhexmesh|snappyHexMesh]]: it cuts a block of [[hex|hexahedra]] round the [[stl|surface]], refines toward it, [[snapping|snaps]] "
                 "to it and adds [[prism-layers|layers]]. It copes with any closed shape and passed every check on the curved parts tested.%s",
                 step_ && !in->salome ? " A STEP file needs SALOME to be read (tools/install_salome.sh); until then export the part as STL." : "");
        }
        if (dims == MA_AXI)
            step(o, "Axisymmetric", "Mesh a thin slice: the STL mesher's 2-D mode makes the profile one cell deep, then Mesh > Block builder > Extrude "
                 "turns it into a [[wedge|wedge]] about the axis (5 degrees or less).");
        if (dims == MA_2D && !(in->salome))
            step(o, "2-D", "The STL mesher's 2-D mode: the profile's cross-section one cell deep, front and back [[empty|empty]].");
        if (!(dims == MA_2D && in->salome))
            step(o, ext ? "Flow round it" : "Flow through it", ext ? "A [[domain|wind tunnel]] round the part: 5 lengths ahead, 10 behind, 3 to the sides "
                 "(the STL mesher sizes it); the cells grow away from the part." : "The part's inside is the fluid: the mesher keeps the cells on the "
                 "side of the [[location-in-mesh|point in the fluid]] and the inlets and outlets become patches.");
    } else {
        o->go = MA_GO_BLOCKS;
        o->tmpl = TMPL[shape];
        if (shape == MA_PIPE && dims != MA_3D) { o->tmpl = BT_WEDGE; dims = MA_AXI; }
        bool axiOther = dims == MA_AXI && shape != MA_PIPE;
        if (axiOther) dims = MA_2D;                           /* only a body of revolution is axisymmetric: the plane instead */
        if (shape == MA_BOX && dims == MA_3D) o->tmpl = BT_CHANNEL;   /* the channel template with a width is a box with an inlet and an outlet */
        const char *tn = BM_TEMPLATES[o->tmpl].name;
        step(o, MA_SHAPE_NAME[shape], "[[blockmesh|blockMesh]]: the shape is made of blocks, so every cell can be a perfect [[hex|hexahedron]] lined up with "
             "the flow, in the fewest cells and with no [[refinement-transition|size jumps]]. Mesh > Block builder's \"%s\" template builds it from a few numbers.", tn);
        if (o->tmpl == BT_WEDGE)
            step(o, "A pipe, axisymmetric", "Nothing varies round the axis, so one thin slice (a [[wedge|wedge]] of 5 degrees) gives the whole flow: "
                 "the cells of one radial plane instead of a full 3-D pipe, often a hundredth of the cost.");
        else if (shape == MA_PIPE)
            step(o, "A pipe in 3-D", "An O-grid: a square core and four curved blocks, so no cell is squeezed to a point at the centre. Choose 3-D "
                 "when the flow isn't the same all round (a bend downstream, swirl, a side inlet).");
        if (axiOther)
            step(o, "Axisymmetric", "A wedge needs a shape that is the same all round an axis (a pipe, a nozzle, a jet); %s isn't, so it is "
                 "meshed as a 2-D plane. For a round body in a stream, mesh it as any other shape (a CAD part) with the axisymmetric choice.",
                 shape == MA_BOX ? "a box" : "this shape");
        if (dims == MA_2D && (shape == MA_BOX || shape == MA_CHANNEL || shape == MA_STEP || shape == MA_PLATE || shape == MA_CYLINDER || shape == MA_AIRFOIL))
            step(o, "2-D", "One cell deep, front and back [[empty|empty]]: OpenFOAM solves the plane only. A 2-D mesh more than one cell deep runs "
                 "silently at double the cost; the template keeps it at one.");
        if (dims == MA_3D && (shape == MA_STEP || shape == MA_PLATE || shape == MA_CYLINDER || shape == MA_AIRFOIL))
            step(o, "3-D", "The template is 2-D: build it, then Extrude (linearNormal, the span as its length and the cells across it) makes it 3-D. "
                 "A 3-D run of these is for turbulence that is 3-D in itself (LES, a cylinder's wake at higher Re).");
        if (shape == MA_BOX && dims == MA_3D)
            step(o, "A 3-D box", "The channel template with a width is a box with an inlet and an outlet; for a closed box, set the inlet and "
                 "outlet to walls on Physics > Boundaries.");
    }

    /* ---- the walls: layers and the first cell */
    double nu = in->nu > 0 ? in->nu : 0;
    bool haveFlow = in->U > 0 && in->L > 0 && nu > 0;
    if (haveFlow) o->Re = in->U * in->L / nu;
    if (in->walls == MA_WALLS_NO) {
        step(o, "The walls only hold the flow in", "No [[prism-layers|layers]]: even cells, or a little [[grading|grading]] toward the walls, are "
             "enough when nothing is asked of the wall itself (forces, heat, separation).");
    } else {
        o->layers = true;
        o->yplus = in->walls == MA_WALLS_RESOLVE ? 1 : 50;
        if (haveFlow) {
            CalcWallIn wi = { .flow = (!ext && (shape == MA_PIPE || shape == MA_CHANNEL || shape == MA_BOX)) ? CW_PIPE : CW_PLATE,
                              .rho = 1, .mu = nu, .U = in->U, .L = in->L, .yplus = o->yplus, .r = 1.2 };
            CalcWallOut wo; Working wk;
            if (calc_wall(&wi, &wo, &wk)) o->h1 = wo.h1;
        }
        char h[240] = "";
        if (o->h1 > 0) {
            double v = o->h1; const char *u = "m";
            if (v < 1e-3) { v *= 1e6; u = "µm"; } else if (v < 1) { v *= 1e3; u = "mm"; }
            snprintf(h, sizeof h, " For Re %.3g that is a [[first-layer|first cell]] %.3g %s high (the Calculators' wall spacing shows the working).", o->Re, v, u);
        } else snprintf(h, sizeof h, " Give the speed and size above and the first cell height follows.");
        if (in->walls == MA_WALLS_RESOLVE)
            step(o, "Heat transfer, separation or accurate drag", "Resolve the [[viscous-sublayer|viscous sublayer]]: the first cell's centre at "
                 "[[first-layer|y+]] about 1, 10 to 20 [[prism-layers|layers]] growing by 1.1 to 1.2, and a turbulence model that integrates to the wall "
                 "(k-omega SST).%s", h);
        else
            step(o, "Pressure drop, lift and drag to a few per cent", "[[wall-function|Wall functions]] bridge the sublayer: the first cell's centre at "
                 "y+ 30 to 300 (aim for 50), 3 to 5 [[prism-layers|layers]]. Ten times fewer cells at the wall than resolving it.%s", h);
        if (haveFlow && o->Re < 2300 && !ext && (shape == MA_PIPE || shape == MA_CHANNEL))
            step(o, "Laminar", "At Re %.0f the flow in a duct is laminar: there is no turbulent sublayer to resolve, y+ doesn't apply. A few cells "
                 "graded toward the walls capture the parabolic profile.", o->Re);
    }

    /* ---- the size: cells for the goal */
    bool flat = dims != MA_3D;
    double base = flat ? (in->goal == MA_QUICK ? 5e3 : in->goal == MA_ANSWER ? 4e4 : 1.5e5)
                       : (in->goal == MA_QUICK ? 1e5 : in->goal == MA_ANSWER ? 1e6 : 3e6);
    if (ext && !flat) base *= 2;                      /* the tunnel round the part */
    if (o->layers && in->walls == MA_WALLS_RESOLVE) base *= 1.5;
    if (o->go == MA_GO_BLOCKS && !flat) base *= 0.6;  /* hexahedra lined up with the flow: fewer for the same answer */
    o->cells = base;
    char cw[40]; cells_words(base, cw, sizeof cw);
    if (in->goal == MA_INDEP) {
        o->nMeshes = 3;
        char c1[40], c2[40];
        double r3 = flat ? 2.25 : 3.375;            /* 1.5 finer each way */
        step(o, "An answer shown not to depend on the mesh", "Three meshes, each 1.5 times finer in every direction (%s, %s, %s cells), the same "
             "quantity from each, then Studies' Grid Convergence Index says how far the finest is from the mesh-independent value "
             "([[mesh-independence|mesh independence]]).", cells_words(base / r3 / r3, c1, sizeof c1), cells_words(base / r3, c2, sizeof c2), cw);
    } else
        step(o, in->goal == MA_QUICK ? "A first look" : "An engineering answer", "About %s cells%s. %s", cw, flat ? " (2-D: the plane only)" : "",
             in->goal == MA_QUICK ? "Coarse enough to run in minutes: right for seeing the flow's shape and checking the setup, not for numbers."
                                  : "Then refine once (1.5 times each way) and compare the number you care about: if it moves less than a few per cent, keep it.");

    /* ---- the answer in a line */
    if (o->go == MA_GO_BLOCKS)
        snprintf(o->title, sizeof o->title, "Block builder, \"%s\" template%s%s", BM_TEMPLATES[o->tmpl].name, o->layers ? ", graded to the walls" : "",
                 dims == MA_3D && o->tmpl != BT_PIPE && o->tmpl != BT_CHANNEL ? ", extruded to 3-D" : "");
    else
        snprintf(o->title, sizeof o->title, "STL mesher with %s%s%s", mesher_short_name(o->mesher), o->layers ? " and prism layers" : "",
                 dims == MA_AXI ? ", then a wedge" : "");
}
