/* teachcases.c -- starters with exact answers (see teachcases.h; improvement plan A2). Keywords from the v2606 tutorials
 * basic/laplacianFoam/flange and basic/scalarTransportFoam/pitzDaily. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "teachcases.h"
#include "pl_util.h"

#define BAR_L 0.1
#define BAR_A 1e-4
#define FLOW_U 0.01

const char *const TC_BAR_TITLE = "Heat conduction in a bar";
const char *const TC_BAR_DESC = "A bar at 300 K whose ends are held at 400 K: the heat equation, solved in time and checked against its exact answer.";
const char *const TC_FLOW_TITLE = "Carried by a flow";
const char *const TC_FLOW_DESC = "A dye (or heat) carried along by a steady flow while it spreads: advection against diffusion, the Peclet number.";

double tc_bar_mid(double t)
{
    double s = 0;
    for (int n = 1; n < 100; n += 2) s += 4 / (n * M_PI) * sin(n * M_PI / 2) * exp(-(n * M_PI) * (n * M_PI) * BAR_A * t / (BAR_L * BAR_L));
    return 400 - 100 * s;
}
double tc_flow_value(double x)
{
    double Pe = FLOW_U * BAR_L / BAR_A;
    return (exp(Pe) - exp(Pe * x / BAR_L)) / (exp(Pe) - 1);
}

static bool put(const char *dst, const char *rel, const char *text, char *err, int cap)
{
    char p[PL_PATH], d[PL_PATH]; pl_join(p, sizeof p, dst, rel);
    pl_strlcpy(d, p, sizeof d); char *sl = strrchr(d, '/'); if (sl) *sl = 0;
    if (!pl_mkdirs(d) || !pl_write_file(p, text, strlen(text))) { snprintf(err, (size_t)cap, "Could not write %s", rel); return false; }
    return true;
}

#define HDR(cls, obj) "FoamFile\n{\n    version     2.0;\n    format      ascii;\n    class       " cls ";\n    object      " obj ";\n}\n\n"

/* a 1-D line of 100 cells along x, 0 to 0.1 m; left and right patches, the other sides empty */
static const char *LINE_MESH = HDR("dictionary", "blockMeshDict")
    "scale 1;\n\nvertices\n(\n    (0 0 0) (0.1 0 0) (0.1 0.01 0) (0 0.01 0)\n    (0 0 0.01) (0.1 0 0.01) (0.1 0.01 0.01) (0 0.01 0.01)\n);\n\n"
    "blocks\n(\n    hex (0 1 2 3 4 5 6 7) (100 1 1) simpleGrading (1 1 1)\n);\n\n"
    "boundary\n(\n    left\n    {\n        type patch;\n        faces ((0 4 7 3));\n    }\n    right\n    {\n        type patch;\n        faces ((1 2 6 5));\n    }\n"
    "    sides\n    {\n        type empty;\n        faces ((0 1 5 4) (3 7 6 2) (0 3 2 1) (4 5 6 7));\n    }\n);\n";

static bool common2(const char *dst, const char *app, double endT, double dt, double writeT, const char *probes, const char *divT,
                    const char *sol, char *err, int cap);

static bool common(const char *dst, const char *app, double endT, double dt, double writeT, const char *probes, const char *divT,
                   char *err, int cap)
{
    /* laplacianFoam's matrix is symmetric (PCG, DIC); scalarTransportFoam's is not (PBiCGStab, DILU) */
    bool sym = !strcmp(app, "laplacianFoam");
    char sol[700];
    snprintf(sol, sizeof sol, HDR("dictionary", "fvSolution")
             "solvers\n{\n    T\n    {\n        solver          %s;\n        preconditioner  %s;\n        tolerance       1e-9;\n        relTol          0;\n    }\n}\n\n"
             "SIMPLE\n{\n    nNonOrthogonalCorrectors 0;\n}\n", sym ? "PCG" : "PBiCGStab", sym ? "DIC" : "DILU");
    return common2(dst, app, endT, dt, writeT, probes, divT, sol, err, cap);
}

static bool common2(const char *dst, const char *app, double endT, double dt, double writeT, const char *probes, const char *divT,
                    const char *sol, char *err, int cap)
{
    char cd[2400];
    snprintf(cd, sizeof cd, HDR("dictionary", "controlDict")
             "application     %s;\n\nstartFrom       startTime;\nstartTime       0;\nstopAt          endTime;\nendTime         %g;\ndeltaT          %g;\n"
             "writeControl    runTime;\nwriteInterval   %g;\npurgeWrite      0;\nwriteFormat     ascii;\nwritePrecision  8;\nwriteCompression off;\n"
             "timeFormat      general;\ntimePrecision   6;\nrunTimeModifiable true;\n\nfunctions\n{\n    probes\n    {\n        type            probes;\n"
             "        libs            (sampling);\n        writeControl    timeStep;\n        writeInterval   1;\n        fields          (T);\n        interpolationScheme cellPoint;   // at the point itself, not the centre of its cell\n"
             "        probeLocations  (%s);\n    }\n}\n", app, endT, dt, writeT, probes);
    char sch[1600];
    snprintf(sch, sizeof sch, HDR("dictionary", "fvSchemes")
             "ddtSchemes\n{\n    default         Euler;\n}\n\ngradSchemes\n{\n    default         Gauss linear;\n}\n\ndivSchemes\n{\n    default         none;\n%s}\n\n"
             "laplacianSchemes\n{\n    default         none;\n    laplacian(DT,T) Gauss linear corrected;\n}\n\ninterpolationSchemes\n{\n    default         linear;\n}\n\n"
             "snGradSchemes\n{\n    default         corrected;\n}\n", divT);
    return put(dst, "system/blockMeshDict", LINE_MESH, err, cap) && put(dst, "system/controlDict", cd, err, cap) &&
           put(dst, "system/fvSchemes", sch, err, cap) &&
           put(dst, "system/fvSolution", sol, err, cap) &&
           put(dst, "constant/transportProperties", HDR("dictionary", "transportProperties") "DT              1e-4;   // m2/s\n", err, cap);
}

bool tc_bar_write(const char *dst, char *err, int cap)
{
    if (!common(dst, "laplacianFoam", 10, 0.01, 2, "(0.05 0.005 0.005)", "", err, cap)) return false;
    char st[1600];
    snprintf(st, sizeof st, "title = %s\nlearn = the heat equation ∂T/∂t = α ∂²T/∂x², the Fourier number Fo = α t / L² and how a time step's size matters\n"
             "expect = After 10 s (Fo = α t / L² = 10⁻⁴ × 10 / 0.01 = 0.1) the middle of the bar is at %.2f K. Exactly: T = 400 − 100 Σ (odd n) "
             "4/(nπ) sin(nπ/2) exp(−(nπ)² Fo); the first term alone gives %.2f K. The probe at x = 0.05 m (Results, Monitors) should agree within 0.2 K.\n"
             "check = probe T at 0.05 = %.4f\n", TC_BAR_TITLE, tc_bar_mid(10), 400 - 100 * 4 / M_PI * exp(-M_PI * M_PI * 0.1), tc_bar_mid(10));
    return put(dst, "0/T", HDR("volScalarField", "T")
               "dimensions      [0 0 0 1 0 0 0];\n\ninternalField   uniform 300;\n\nboundaryField\n{\n    left\n    {\n        type            fixedValue;\n"
               "        value           uniform 400;\n    }\n    right\n    {\n        type            fixedValue;\n        value           uniform 400;\n    }\n"
               "    sides\n    {\n        type            empty;\n    }\n}\n", err, cap) &&
           put(dst, ".qc-starter", st, err, cap);
}

bool tc_flow_write(const char *dst, char *err, int cap)
{
    if (!common(dst, "scalarTransportFoam", 300, 0.05, 50, "(0.05 0.005 0.005) (0.09 0.005 0.005)", "    div(phi,T)      Gauss linear;\n", err, cap)) return false;
    char st[1800];
    snprintf(st, sizeof st, "title = %s\nlearn = advection against diffusion: the Péclet number Pe = U L / D, and the thin layer where diffusion matters when Pe is large\n"
             "expect = Pe = U L / D = 0.01 × 0.1 / 10⁻⁴ = 10. Steady, T(x) = (e^Pe − e^(Pe x/L)) / (e^Pe − 1): %.4f at x = 0.05 m and %.4f at x = 0.09 m. "
             "The flow carries the inlet's value almost to the outlet; diffusion only wins in the last tenth (about L / Pe). Run to 300 s: three "
             "diffusion times (L²/D = 100 s), enough to settle.\ncheck = probe T at 0.09 = %.4f\n", TC_FLOW_TITLE, tc_flow_value(0.05), tc_flow_value(0.09), tc_flow_value(0.09));
    return put(dst, "0/T", HDR("volScalarField", "T")
               "dimensions      [0 0 0 0 0 0 0];\n\ninternalField   uniform 0;\n\nboundaryField\n{\n    left\n    {\n        type            fixedValue;\n"
               "        value           uniform 1;\n    }\n    right\n    {\n        type            fixedValue;\n        value           uniform 0;\n    }\n"
               "    sides\n    {\n        type            empty;\n    }\n}\n", err, cap) &&
           put(dst, "0/U", HDR("volVectorField", "U")
               "dimensions      [0 1 -1 0 0 0 0];\n\ninternalField   uniform (0.01 0 0);\n\nboundaryField\n{\n    left\n    {\n        type            fixedValue;\n"
               "        value           uniform (0.01 0 0);\n    }\n    right\n    {\n        type            zeroGradient;\n    }\n    sides\n    {\n"
               "        type            empty;\n    }\n}\n", err, cap) &&
           put(dst, ".qc-starter", st, err, cap);
}
