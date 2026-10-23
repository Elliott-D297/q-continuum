/* plume.c -- the impinging-plume starter (ROADMAP 4.4, see plume.h)
 *
 * Geometry: the nozzle exit plane at x = 0 (radius re), the plate at x = H, the domain out to radius R, as a 5-degree
 * wedge about the x axis. Patches: inlet (the exit), nozzleBase (the rest of the x = 0 plane: the vehicle's base, a
 * wall), plate (wall), ambient (r = R, open), wedge1/wedge2. Fields start at the ambient state.
 */
#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plume.h"
#include "pl_util.h"

void plume_defaults(PlumeIn *in)
{
    memset(in, 0, sizeof *in);
    in->gas = 0; in->molWeight = 28.0134; in->gamma = 1.4;
    in->Me = 3.0; in->pe = 5000; in->Te = 300;       /* a cold-gas N2 thruster */
    in->re = 0.01; in->H = 0.05; in->R = 0.1;        /* stand-off 2.5 exit diameters */
    in->pa = 100; in->Ta = 300; in->Tw = 300;        /* a low ambient: the Knudsen check says where the continuum ends */
    in->cellsPerRadius = 10;                         /* a first look in minutes; 20-40 for shock positions that hold */
    in->maxCo = 0.2;
}

bool plume_compute(const PlumeIn *in, PlumeOut *o, char *err, int cap)
{
    memset(o, 0, sizeof *o);
    err[0] = 0;
    double W = in->gas == 0 ? 28.0134 : in->gas == 1 ? 28.96 : in->molWeight, g = in->gas == 2 ? in->gamma : 1.4;
    if (!(W > 0) || !(g > 1)) { snprintf(err, (size_t)cap, "The gas needs a molar mass above 0 and gamma above 1."); return false; }
    if (!(in->Me >= 1)) { snprintf(err, (size_t)cap, "The exit Mach number must be 1 or more (a converging nozzle exits at 1)."); return false; }
    if (!(in->pe > 0) || !(in->Te > 0) || !(in->pa > 0) || !(in->Ta > 0) || !(in->Tw > 0)) {
        snprintf(err, (size_t)cap, "Pressures and temperatures must be above 0 (rhoCentralFoam can't hold a true vacuum: use a low ambient)."); return false;
    }
    if (!(in->re > 0) || !(in->H > 0) || !(in->R > in->re)) { snprintf(err, (size_t)cap, "Exit radius and stand-off above 0, and the domain wider than the exit."); return false; }
    if (in->cellsPerRadius < 4 || in->cellsPerRadius > 400) { snprintf(err, (size_t)cap, "Use 4 to 400 cells across the exit radius."); return false; }
    o->Rs = 8314.46 / W;
    o->ae = sqrt(g * o->Rs * in->Te);
    o->Ue = in->Me * o->ae;
    o->rhoe = in->pe / (o->Rs * in->Te);
    o->mdot = o->rhoe * o->Ue * M_PI * in->re * in->re;
    o->p0 = in->pe * pow(1 + 0.5 * (g - 1) * in->Me * in->Me, g / (g - 1));
    o->npr = o->p0 / in->pa;
    o->per = in->pe / in->pa;
    o->xMach = 0.67 * 2 * in->re * sqrt(o->npr);
    o->dx = in->re / in->cellsPerRadius;
    o->nx = (int)lround(in->H / o->dx);
    if (o->nx < 4) o->nx = 4;
    o->nr1 = in->cellsPerRadius;
    /* beyond the exit the cells grow 4x toward the domain edge: about (R - re) / (2.5 dx) of them */
    o->nr2 = (int)lround((in->R - in->re) / (2.5 * o->dx));
    if (o->nr2 < 4) o->nr2 = 4;
    o->cells = (long)o->nx * (o->nr1 + o->nr2);
    o->dt0 = in->maxCo * o->dx / (o->Ue + o->ae);
    o->endTime = 2 * (in->H + in->R) / o->Ue;          /* the jet reaches the plate and the wall jet the domain edge, twice over */
    /* on a wedge the sound speed across the thin wedge faces near the axis sets rhoCentralFoam's Courant number: the step
       it takes is about 7x below maxCo dx / (U + a) (measured on this starter: 9.9e-9 s against 7.1e-8 s) */
    o->steps = (long)(o->endTime / (o->dt0 / 7));
    o->cpuSeconds = (double)o->steps * (double)o->cells * 12e-6;   /* ~12 us per cell and step on one core here (2300 cells, 11 600 steps: 300 core-s) */
    o->writeInterval = o->endTime / 20;
    o->Cp = g * o->Rs / (g - 1);
    if (in->gas == 0) { o->As = 1.407e-6; o->Ts = 111; } else { o->As = 1.458e-6; o->Ts = 110.4; }
    int k = 0;
    if (o->per < 1) k += snprintf(o->warn + k, sizeof o->warn - (size_t)k, "pe/pa = %.3g: over-expanded, the jet separates in the nozzle on a real "
                                  "vehicle; here it is fixed at the exit. ", o->per);
    if (o->cells > 2000000) k += snprintf(o->warn + k, sizeof o->warn - (size_t)k, "%ld cells is a lot for this machine (7 GB): use fewer cells "
                                          "across the radius. ", o->cells);
    if (in->pa < 1) snprintf(o->warn + k, sizeof o->warn - (size_t)k, "Ambient below 1 Pa: the outer flow is rarefied (SENSOR SWEEP's "
                                  "continuum check says where), and very large pressure ratios slow rhoCentralFoam down. ");
    return true;
}

static bool put(const char *dir, const char *rel, const char *cls, const char *obj, const char *body)
{
    char p[PL_PATH];
    if (!pl_fmt(p, sizeof p, "%s/%s", dir, rel)) return false;
    char d[PL_PATH]; pl_dirname(d, sizeof d, p);
    if (!pl_mkdirs(d)) return false;
    Str s; str_init(&s);
    str_catf(&s, "/*--------------------------------*- C++ -*----------------------------------*\\\n"
                 "  Written by Q-Continuum's impinging-plume starter (ROADMAP 4.4)\n"
                 "\\*---------------------------------------------------------------------------*/\n"
                 "FoamFile\n{\n    version     2.0;\n    format      ascii;\n    class       %s;\n    object      %s;\n}\n"
                 "// * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * //\n\n%s\n"
                 "// ************************************************************************* //\n", cls, obj, body);
    bool ok = pl_write_file(p, s.s, s.len);
    str_free(&s);
    return ok;
}

bool plume_write(const char *dir, const PlumeIn *in, char *err, int cap)
{
    PlumeOut o;
    if (!plume_compute(in, &o, err, cap)) return false;
    if (pl_exists(dir)) { snprintf(err, (size_t)cap, "%s already exists", dir); return false; }
    double W = in->gas == 0 ? 28.0134 : in->gas == 1 ? 28.96 : in->molWeight, g = in->gas == 2 ? in->gamma : 1.4;
    double c = cos(2.5 * M_PI / 180), s = sin(2.5 * M_PI / 180);
    char b[8192];
    bool ok = true;

    snprintf(b, sizeof b,
        "// Axisymmetric 5-degree wedge about x: nozzle exit (r < %g m) at x = 0, plate at x = %g m, domain radius %g m.\n"
        "// %g m cells at the axis (%d across the exit radius), growing 4x beyond the exit.\n\n"
        "mergeType points;\n\nscale   1;\n\nvertices\n(\n"
        "    (0 0 0)\n    (%.9g 0 0)\n"
        "    (0 %.9g %.9g)\n    (%.9g %.9g %.9g)\n"
        "    (0 %.9g %.9g)\n    (%.9g %.9g %.9g)\n"
        "    (0 %.9g %.9g)\n    (%.9g %.9g %.9g)\n"
        "    (0 %.9g %.9g)\n    (%.9g %.9g %.9g)\n);\n\n"
        "blocks\n(\n"
        "    hex (0 1 3 2 0 1 7 6) (%d %d 1) simpleGrading (1 1 1)\n"
        "    hex (2 3 5 4 6 7 9 8) (%d %d 1) simpleGrading (1 4 1)\n);\n\n"
        "edges\n(\n);\n\n"
        "boundary\n(\n"
        "    inlet\n    {\n        type patch;\n        faces\n        (\n            (0 2 6 0)\n        );\n    }\n"
        "    nozzleBase\n    {\n        type wall;\n        faces\n        (\n            (2 4 8 6)\n        );\n    }\n"
        "    plate\n    {\n        type wall;\n        faces\n        (\n            (1 3 7 1)\n            (3 5 9 7)\n        );\n    }\n"
        "    ambient\n    {\n        type patch;\n        faces\n        (\n            (4 8 9 5)\n        );\n    }\n"
        "    wedge1\n    {\n        type wedge;\n        faces\n        (\n            (0 2 3 1)\n            (2 4 5 3)\n        );\n    }\n"
        "    wedge2\n    {\n        type wedge;\n        faces\n        (\n            (0 1 7 6)\n            (6 7 9 8)\n        );\n    }\n);\n\n"
        "mergePatchPairs\n(\n);\n",
        in->re, in->H, in->R, o.dx, in->cellsPerRadius,
        in->H,
        in->re * c, -in->re * s, in->H, in->re * c, -in->re * s,
        in->R * c, -in->R * s, in->H, in->R * c, -in->R * s,
        in->re * c, in->re * s, in->H, in->re * c, in->re * s,
        in->R * c, in->R * s, in->H, in->R * c, in->R * s,
        o.nx, o.nr1, o.nx, o.nr2);
    ok = ok && put(dir, "system/blockMeshDict", "dictionary", "blockMeshDict", b);

    snprintf(b, sizeof b,
        "application     rhoCentralFoam;\n\nstartFrom       startTime;\n\nstartTime       0;\n\nstopAt          endTime;\n\n"
        "// two transits of the jet to the plate and the wall jet to the domain edge: 2 (H + R) / Ue\n"
        "endTime         %.6g;\n\ndeltaT          %.6g;\n\nwriteControl    adjustable;\n\nwriteInterval   %.6g;\n\n"
        "purgeWrite      0;\n\nwriteFormat     binary;\n\nwritePrecision  8;\n\nwriteCompression off;\n\ntimeFormat      general;\n\n"
        "timePrecision   8;\n\nrunTimeModifiable true;\n\nadjustTimeStep  yes;\n\nmaxCo           %g;\n\nmaxDeltaT       1;\n\n"
        "functions\n{\n"
        "    // what the plume does to the surface: pressure on the plate (area average and peak) and the wall shear stress\n"
        "    platePressure\n    {\n        type            surfaceFieldValue;\n        libs            (fieldFunctionObjects);\n"
        "        writeControl    timeStep;\n        writeInterval   20;\n        log             false;\n        writeFields     false;\n"
        "        regionType      patch;\n        name            plate;\n        operation       areaAverage;\n        fields          (p);\n    }\n"
        "    platePeak\n    {\n        $platePressure;\n        operation       max;\n    }\n"
        "    wallShearStress\n    {\n        type            wallShearStress;\n        libs            (fieldFunctionObjects);\n"
        "        patches         (plate);\n        writeControl    writeTime;\n        log             false;\n    }\n"
        "}\n",
        o.endTime, o.dt0, o.writeInterval, in->maxCo);
    ok = ok && put(dir, "system/controlDict", "dictionary", "controlDict", b);

    ok = ok && put(dir, "system/fvSchemes", "dictionary", "fvSchemes",
        "fluxScheme          Kurganov;\n\nddtSchemes\n{\n    default         Euler;\n}\n\ngradSchemes\n{\n    default         Gauss linear;\n}\n\n"
        "divSchemes\n{\n    default         none;\n    div(tauMC)      Gauss linear;\n}\n\n"
        "laplacianSchemes\n{\n    default         Gauss linear corrected;\n}\n\n"
        "interpolationSchemes\n{\n    default         linear;\n    reconstruct(rho) vanLeer;\n    reconstruct(U)  vanLeerV;\n    reconstruct(T)  vanLeer;\n}\n\n"
        "snGradSchemes\n{\n    default         corrected;\n}\n");
    ok = ok && put(dir, "system/fvSolution", "dictionary", "fvSolution",
        "solvers\n{\n    \"(rho|rhoU|rhoE)\"\n    {\n        solver          diagonal;\n    }\n\n"
        "    U\n    {\n        solver          smoothSolver;\n        smoother        GaussSeidel;\n        nSweeps         2;\n"
        "        tolerance       1e-10;\n        relTol          0;\n    }\n\n"
        "    e\n    {\n        $U;\n        tolerance       1e-10;\n        relTol          0;\n    }\n}\n");
    ok = ok && put(dir, "system/decomposeParDict", "dictionary", "decomposeParDict", "numberOfSubdomains 4;\n\nmethod          scotch;\n");

    snprintf(b, sizeof b,
        "thermoType\n{\n    type            hePsiThermo;\n    mixture         pureMixture;\n    transport       sutherland;\n"
        "    thermo          hConst;\n    equationOfState perfectGas;\n    specie          specie;\n    energy          sensibleInternalEnergy;\n}\n\n"
        "mixture\n{\n    specie\n    {\n        molWeight       %.6g;\n    }\n"
        "    thermodynamics\n    {\n        Cp              %.6g;   // gamma R / (gamma - 1), gamma %g\n        Hf              0;\n    }\n"
        "    transport\n    {\n        As              %g;\n        Ts              %g;\n        Pr              0.72;\n    }\n}\n",
        W, o.Cp, g, o.As, o.Ts);
    ok = ok && put(dir, "constant/thermophysicalProperties", "dictionary", "thermophysicalProperties", b);
    ok = ok && put(dir, "constant/turbulenceProperties", "dictionary", "turbulenceProperties", "simulationType  laminar;\n");

    /* the fields: ambient everywhere at the start; the exit fixed at its state */
    snprintf(b, sizeof b,
        "dimensions      [1 -1 -2 0 0 0 0];\n\ninternalField   uniform %g;\n\nboundaryField\n{\n"
        "    inlet\n    {\n        type            fixedValue;\n        value           uniform %g;\n    }\n"
        "    nozzleBase\n    {\n        type            zeroGradient;\n    }\n"
        "    plate\n    {\n        type            zeroGradient;\n    }\n"
        "    // non-reflecting: the wall jet leaves through it supersonic (a fixed total pressure there blew the run up)\n"
        "    ambient\n    {\n        type            waveTransmissive;\n        field           p;\n        psi             thermo:psi;\n"
        "        gamma           %g;\n        fieldInf        %g;\n        lInf            %g;\n        value           uniform %g;\n    }\n"
        "    wedge1\n    {\n        type            wedge;\n    }\n    wedge2\n    {\n        type            wedge;\n    }\n}\n",
        in->pa, in->pe, g, in->pa, in->R, in->pa);
    ok = ok && put(dir, "0/p", "volScalarField", "p", b);
    snprintf(b, sizeof b,
        "dimensions      [0 0 0 1 0 0 0];\n\ninternalField   uniform %g;\n\nboundaryField\n{\n"
        "    inlet\n    {\n        type            fixedValue;\n        value           uniform %g;\n    }\n"
        "    nozzleBase\n    {\n        type            fixedValue;\n        value           uniform %g;\n    }\n"
        "    plate\n    {\n        type            fixedValue;\n        value           uniform %g;\n    }\n"
        "    ambient\n    {\n        type            inletOutlet;\n        inletValue      uniform %g;\n        value           uniform %g;\n    }\n"
        "    wedge1\n    {\n        type            wedge;\n    }\n    wedge2\n    {\n        type            wedge;\n    }\n}\n",
        in->Ta, in->Te, in->Ta, in->Tw, in->Ta, in->Ta);
    ok = ok && put(dir, "0/T", "volScalarField", "T", b);
    snprintf(b, sizeof b,
        "dimensions      [0 1 -1 0 0 0 0];\n\ninternalField   uniform (0 0 0);\n\nboundaryField\n{\n"
        "    inlet\n    {\n        type            fixedValue;\n        value           uniform (%.6g 0 0);   // Me %g x a %.4g m/s\n    }\n"
        "    nozzleBase\n    {\n        type            noSlip;\n    }\n"
        "    plate\n    {\n        type            noSlip;\n    }\n"
        "    ambient\n    {\n        type            inletOutlet;\n        inletValue      uniform (0 0 0);\n        value           uniform (0 0 0);\n    }\n"
        "    wedge1\n    {\n        type            wedge;\n    }\n    wedge2\n    {\n        type            wedge;\n    }\n}\n",
        o.Ue, in->Me, o.ae);
    ok = ok && put(dir, "0/U", "volVectorField", "U", b);

    snprintf(b, sizeof b,
        "Impinging plume (Q-Continuum starter, ROADMAP 4.4)\n\n"
        "An axisymmetric under-expanded jet from a nozzle exit onto a flat plate, rhoCentralFoam, laminar.\n\n"
        "Gas: molWeight %g kg/kmol, gamma %g, R %.4g J/(kg K)\n"
        "Exit: Me %g, pe %g Pa, Te %g K, re %g m -> a %.4g m/s, Ue %.5g m/s, rho %.4g kg/m3, mdot %.4g kg/s\n"
        "Stagnation p0 = pe (1 + (g-1)/2 Me^2)^(g/(g-1)) = %.5g Pa; NPR p0/pa = %.4g; pe/pa = %.4g\n"
        "Mach disk (Ashkenas-Sherman, free jet, sonic-nozzle correlation): x = 0.67 De sqrt(p0/pa) = %.4g m; plate at H = %g m\n"
        "Ambient %g Pa, %g K; plate %g K\n"
        "Mesh: %ld cells, dx %.4g m at the axis; dt0 %.3g s for Co %g; endTime %.4g s\n",
        W, g, o.Rs, in->Me, in->pe, in->Te, in->re, o.ae, o.Ue, o.rhoe, o.mdot, o.p0, o.npr, o.per, o.xMach, in->H,
        in->pa, in->Ta, in->Tw, o.cells, o.dx, o.dt0, in->maxCo, o.endTime);
    char rp[PL_PATH];
    ok = ok && pl_fmt(rp, sizeof rp, "%s/README.plume", dir) && pl_write_file(rp, b, strlen(b));
    if (!ok) snprintf(err, (size_t)cap, "Could not write the case files in %s", dir);
    return ok;
}
