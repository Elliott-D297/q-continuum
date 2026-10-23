/* suggest.c -- what to offer in dropdowns for well-known OpenFOAM keywords, and short hints */
#include <string.h>
#include <stdio.h>
#include "app.h"

#define L(...) { __VA_ARGS__ }
#define N(a) ((int)(sizeof(a) / sizeof(a[0])))

static const char *const YESNO[] = { "yes", "no", "on", "off", "true", "false" };
static const char *const SOLVERS[] = { "PCG", "PBiCGStab", "PBiCG", "smoothSolver", "GAMG", "diagonal" };
static const char *const PRECOND[] = { "DIC", "DILU", "FDIC", "GAMG", "diagonal", "none" };
static const char *const SMOOTHERS[] = { "GaussSeidel", "symGaussSeidel", "DIC", "DILU", "DICGaussSeidel", "nonBlockingGaussSeidel" };
static const char *const DDT[] = { "Euler", "steadyState", "backward", "CrankNicolson 0.9", "localEuler", "bounded Euler" };
static const char *const GRAD[] = { "Gauss linear", "leastSquares", "cellLimited Gauss linear 1", "cellMDLimited Gauss linear 1",
                                    "faceLimited Gauss linear 1", "pointCellsLeastSquares" };
static const char *const DIV[] = { "none", "Gauss linear", "Gauss upwind", "bounded Gauss upwind", "Gauss linearUpwind grad(U)",
                                   "bounded Gauss linearUpwind grad(U)", "Gauss linearUpwind limited", "Gauss limitedLinear 1",
                                   "bounded Gauss limitedLinear 1", "Gauss limitedLinearV 1", "Gauss vanLeer", "Gauss vanLeerV",
                                   "Gauss MUSCL", "Gauss QUICK", "Gauss LUST grad(U)", "Gauss vanLeer01", "Gauss interfaceCompression vanLeer 1",
                                   "Gauss linear corrected", "Gauss linear limited 0.33", "bounded Gauss upwind phi" };
static const char *const LAPL[] = { "Gauss linear corrected", "Gauss linear limited corrected 0.33", "Gauss linear limited corrected 0.5",
                                    "Gauss linear limited 1", "Gauss linear uncorrected", "Gauss linear orthogonal" };
static const char *const INTERP[] = { "linear", "upwind", "linearUpwind", "midPoint", "reverseLinear" };
static const char *const SNGRAD[] = { "corrected", "limited corrected 0.33", "limited corrected 0.5", "limited 1", "uncorrected", "orthogonal" };
static const char *const WRITECTRL[] = { "timeStep", "runTime", "adjustable", "adjustableRunTime", "cpuTime", "clockTime" };
static const char *const STARTFROM[] = { "startTime", "latestTime", "firstTime" };
static const char *const STOPAT[] = { "endTime", "writeNow", "noWriteNow", "nextWrite" };
static const char *const WRITEFMT[] = { "ascii", "binary" };
static const char *const COMPRESS[] = { "off", "on" };
static const char *const TIMEFMT[] = { "general", "fixed", "scientific" };
static const char *const SIMTYPE[] = { "laminar", "RAS", "LES" };
static const char *const RAS[] = { "kEpsilon", "realizableKE", "RNGkEpsilon", "kOmega", "kOmegaSST", "kOmegaSSTLM", "GEKO",
                                   "SpalartAllmaras", "LaunderSharmaKE", "v2f", "LienLeschziner", "kkLOmega", "buoyantKEpsilon",
                                   "kEpsilonPhitF" };
static const char *const LES[] = { "Smagorinsky", "WALE", "kEqn", "dynamicKEqn", "dynamicLagrangian", "SpalartAllmarasDES",
                                   "SpalartAllmarasDDES", "SpalartAllmarasIDDES", "kOmegaSSTDES", "kOmegaSSTDDES", "kOmegaSSTIDDES" };
static const char *const DELTA[] = { "cubeRootVol", "vanDriest", "smooth", "Prandtl", "maxDeltaxyz" };
static const char *const TRANSPORT[] = { "Newtonian", "CrossPowerLaw", "BirdCarreau", "HerschelBulkley", "powerLaw", "Casson" };
static const char *const RADMODEL[] = { "none", "P1", "fvDOM", "viewFactor", "opaqueSolid", "solarLoad" };
static const char *const ABSEMI[] = { "none", "constantAbsorptionEmission", "greyMeanAbsorptionEmission", "wideBandAbsorptionEmission" };
static const char *const SCATTER[] = { "none", "constantScatter" };
static const char *const DECOMP[] = { "scotch", "simple", "hierarchical", "kahip", "metis", "manual", "random" };
static const char *const THERMO_TYPE[] = { "heRhoThermo", "hePsiThermo", "heSolidThermo" };
static const char *const MIXTURE[] = { "pureMixture", "multiComponentMixture", "reactingMixture" };
static const char *const TRANSPORT_T[] = { "const", "sutherland", "polynomial", "logPolynomial", "constIso", "exponential" };
static const char *const THERMO_T[] = { "hConst", "eConst", "janaf", "hPolynomial", "hRefConst" };
static const char *const EOS[] = { "perfectGas", "Boussinesq", "incompressiblePerfectGas", "rhoConst", "perfectFluid", "icoPolynomial", "PengRobinsonGas" };
static const char *const ENERGY[] = { "sensibleEnthalpy", "sensibleInternalEnergy", "absoluteEnthalpy" };
static const char *const SWITCH01[] = { "on", "off" };
static const char *const MPI_AGG[] = { "faceAreaPair", "algebraicPair" };
static const char *const NEARWALL[] = { "meshWave", "Poisson", "advectionDiffusion" };

const char *const SOLVER_APPS[] = {
    "icoFoam", "simpleFoam", "pimpleFoam", "pisoFoam", "potentialFoam", "SRFSimpleFoam", "porousSimpleFoam",
    "buoyantSimpleFoam", "buoyantPimpleFoam", "chtMultiRegionFoam", "chtMultiRegionSimpleFoam", "laplacianFoam",
    "interFoam", "interIsoFoam", "multiphaseInterFoam", "multiphaseEulerFoam", "twoPhaseEulerFoam", "driftFluxFoam",
    "compressibleInterFoam", "rhoSimpleFoam", "rhoPimpleFoam", "rhoCentralFoam", "sonicFoam", "reactingFoam",
    "solidDisplacementFoam", "scalarTransportFoam" };
const int N_SOLVER_APPS = N(SOLVER_APPS);

typedef struct { const char *key; const char *ctx; const char *const *items; int n; } Sugg;

#define S(k, c, a) { k, c, a, N(a) }
static const Sugg TABLE[] = {
    S("solver", "solvers", SOLVERS), S("preconditioner", NULL, PRECOND), S("smoother", NULL, SMOOTHERS),
    S("momentumPredictor", NULL, YESNO), S("consistent", NULL, YESNO), S("turbulence", NULL, SWITCH01),
    S("printCoeffs", NULL, SWITCH01), S("runTimeModifiable", NULL, YESNO), S("adjustTimeStep", NULL, YESNO),
    S("transonic", NULL, YESNO), S("cacheAgglomeration", NULL, YESNO), S("agglomerator", NULL, MPI_AGG),
    S("writeControl", NULL, WRITECTRL), S("startFrom", NULL, STARTFROM), S("stopAt", NULL, STOPAT),
    S("writeFormat", NULL, WRITEFMT), S("writeCompression", NULL, COMPRESS), S("timeFormat", NULL, TIMEFMT),
    S("simulationType", NULL, SIMTYPE), S("RASModel", NULL, RAS), S("LESModel", NULL, LES), S("delta", "LES", DELTA),
    S("transportModel", NULL, TRANSPORT), S("viscosityModel", NULL, TRANSPORT), S("radiationModel", NULL, RADMODEL),
    S("radiation", NULL, SWITCH01), S("absorptionEmissionModel", NULL, ABSEMI), S("scatterModel", NULL, SCATTER),
    S("method", NULL, DECOMP), S("mixture", "thermoType", MIXTURE), S("transport", "thermoType", TRANSPORT_T),
    S("thermo", "thermoType", THERMO_T), S("equationOfState", "thermoType", EOS), S("energy", "thermoType", ENERGY),
    S("method", "wallDist", NEARWALL), S("application", NULL, SOLVER_APPS),
};

int suggest_for_key(const char *key, const char *ctx, const char *const **out)
{
    char last[128] = "";
    if (ctx) { const char *sl = strrchr(ctx, '/'); snprintf(last, sizeof last, "%s", sl ? sl + 1 : ctx); }
    /* schemes: decided by the enclosing section */
    if (!strcmp(last, "ddtSchemes")) { *out = DDT; return N(DDT); }
    if (!strcmp(last, "gradSchemes")) { *out = GRAD; return N(GRAD); }
    if (!strcmp(last, "divSchemes")) { *out = DIV; return N(DIV); }
    if (!strcmp(last, "laplacianSchemes")) { *out = LAPL; return N(LAPL); }
    if (!strcmp(last, "interpolationSchemes")) { *out = INTERP; return N(INTERP); }
    if (!strcmp(last, "snGradSchemes")) { *out = SNGRAD; return N(SNGRAD); }
    if (!strcmp(key, "model") && strstr(ctx ? ctx : "", "RAS")) { *out = RAS; return N(RAS); }
    if (!strcmp(key, "model") && strstr(ctx ? ctx : "", "LES")) { *out = LES; return N(LES); }
    if (!strcmp(key, "type") && !strcmp(last, "thermoType")) { *out = THERMO_TYPE; return N(THERMO_TYPE); }
    if (!strcmp(key, "specie") && !strcmp(last, "thermoType")) { static const char *const sp[] = { "specie" }; *out = sp; return 1; }
    for (size_t i = 0; i < sizeof TABLE / sizeof TABLE[0]; i++) {
        if (strcmp(TABLE[i].key, key)) continue;
        if (TABLE[i].ctx && (!ctx || !strstr(ctx, TABLE[i].ctx))) continue;
        *out = TABLE[i].items;
        return TABLE[i].n;
    }
    return 0;
}

/* ---------------------------------------------------------------- boundary conditions */
static const char *const BC_U[] = { "fixedValue", "noSlip", "slip", "zeroGradient", "inletOutlet", "pressureInletOutletVelocity",
    "flowRateInletVelocity", "movingWallVelocity", "rotatingWallVelocity", "uniformFixedValue", "fixedMean", "outletInlet",
    "turbulentInlet", "variableHeightFlowRateInletVelocity", "symmetry", "symmetryPlane", "empty", "wedge", "cyclic", "calculated" };
static const char *const BC_P[] = { "zeroGradient", "fixedValue", "fixedFluxPressure", "totalPressure", "prghPressure",
    "prghTotalPressure", "fixedMean", "uniformFixedValue", "outletInlet", "calculated", "symmetry", "symmetryPlane", "empty",
    "wedge", "cyclic" };
static const char *const BC_T[] = { "fixedValue", "zeroGradient", "inletOutlet", "fixedGradient", "externalWallHeatFluxTemperature",
    "compressible::turbulentTemperatureCoupledBaffleMixed", "compressible::turbulentTemperatureRadCoupledMixed", "totalTemperature",
    "uniformFixedValue", "calculated", "symmetry", "symmetryPlane", "empty", "wedge", "cyclic" };
static const char *const BC_K[] = { "kqRWallFunction", "fixedValue", "zeroGradient", "inletOutlet", "turbulentIntensityKineticEnergyInlet",
    "calculated", "symmetry", "symmetryPlane", "empty", "wedge", "cyclic" };
static const char *const BC_EPS[] = { "epsilonWallFunction", "fixedValue", "zeroGradient", "inletOutlet",
    "turbulentMixingLengthDissipationRateInlet", "calculated", "symmetry", "symmetryPlane", "empty", "wedge", "cyclic" };
static const char *const BC_OMEGA[] = { "omegaWallFunction", "fixedValue", "zeroGradient", "inletOutlet",
    "turbulentMixingLengthFrequencyInlet", "calculated", "symmetry", "symmetryPlane", "empty", "wedge", "cyclic" };
static const char *const BC_NUT[] = { "nutkWallFunction", "nutUSpaldingWallFunction", "nutLowReWallFunction", "nutUWallFunction",
    "nutkRoughWallFunction", "calculated", "zeroGradient", "fixedValue", "symmetry", "symmetryPlane", "empty", "wedge", "cyclic" };
static const char *const BC_ALPHAT[] = { "compressible::alphatWallFunction", "compressible::alphatJayatillekeWallFunction",
    "calculated", "zeroGradient", "fixedValue", "symmetry", "empty", "wedge", "cyclic" };
static const char *const BC_ALPHA[] = { "zeroGradient", "inletOutlet", "fixedValue", "constantAlphaContactAngle",
    "variableHeightFlowRate", "calculated", "symmetry", "symmetryPlane", "empty", "wedge", "cyclic" };
static const char *const BC_G[] = { "MarshakRadiation", "MarshakRadiationFixedTemperature", "zeroGradient", "calculated", "empty", "symmetry" };
static const char *const BC_I[] = { "greyDiffusiveRadiation", "wideBandDiffusiveRadiation", "zeroGradient", "calculated", "empty", "symmetry" };
static const char *const BC_GENERIC[] = { "fixedValue", "zeroGradient", "inletOutlet", "fixedGradient", "uniformFixedValue",
    "calculated", "symmetry", "symmetryPlane", "empty", "wedge", "cyclic", "codedFixedValue" };

int suggest_bc_types(const char *field, const char *cls, const char *const **out)
{
    (void)cls;
    #define RET(a) do { *out = a; return N(a); } while (0)
    if (!strcmp(field, "U") || pl_starts_with(field, "U.")) RET(BC_U);
    if (!strcmp(field, "p") || !strcmp(field, "p_rgh") || pl_starts_with(field, "p.")) RET(BC_P);
    if (!strcmp(field, "T") || pl_starts_with(field, "T.")) RET(BC_T);
    if (!strcmp(field, "k")) RET(BC_K);
    if (!strcmp(field, "epsilon")) RET(BC_EPS);
    if (!strcmp(field, "omega")) RET(BC_OMEGA);
    if (!strcmp(field, "nut") || !strcmp(field, "nuTilda")) RET(BC_NUT);
    if (!strcmp(field, "alphat")) RET(BC_ALPHAT);
    if (pl_starts_with(field, "alpha")) RET(BC_ALPHA);
    if (!strcmp(field, "G")) RET(BC_G);
    if (pl_starts_with(field, "IDefault") || pl_starts_with(field, "ILambda")) RET(BC_I);
    RET(BC_GENERIC);
    #undef RET
}

/* the usual condition for a field on a patch of this mesh type: wall functions and noSlip on walls,
   calculated for the turbulent viscosities, zeroGradient otherwise; constraint patches take their own type */
const char *bc_default_type(const char *field, const char *patchType)
{
    static const char *const cons[] = { "empty", "symmetry", "symmetryPlane", "wedge", "cyclic", "cyclicAMI", "processor", NULL };
    for (int i = 0; cons[i]; i++) if (!strcmp(patchType, cons[i])) return patchType;
    bool wall = !strcmp(patchType, "wall") || !strcmp(patchType, "mappedWall");
    if (!strcmp(field, "nut") || !strcmp(field, "alphat")) {
        if (!wall) return "calculated";
        return !strcmp(field, "nut") ? "nutkWallFunction" : "compressible::alphatWallFunction";
    }
    if (!wall) return "zeroGradient";
    if (!strcmp(field, "U") || pl_starts_with(field, "U.")) return "noSlip";
    if (!strcmp(field, "k")) return "kqRWallFunction";
    if (!strcmp(field, "epsilon")) return "epsilonWallFunction";
    if (!strcmp(field, "omega")) return "omegaWallFunction";
    if (!strcmp(field, "nuTilda")) return "fixedValue";
    if (!strcmp(field, "p_rgh")) return "fixedFluxPressure";
    if (!strcmp(field, "G")) return "MarshakRadiation";
    if (pl_starts_with(field, "IDefault")) return "greyDiffusiveRadiation";
    return "zeroGradient";
}

/* entries a freshly chosen BC type needs so the case still runs */
void bc_default_entries(FDFile *f, const char *pp, const char *type, const char *field, const char *cls)
{
    bool vec = cls && strstr(cls, "Vector");
    const char *zero = vec ? "uniform (0 0 0)" : "uniform 0";
    char path[600];
    #define HAS(k) (snprintf(path, sizeof path, "%s/%s", pp, k), fd_find(f, path) != NULL)
    #define PUT(k, v) do { snprintf(path, sizeof path, "%s/%s", pp, k); fd_put(f, path, v); } while (0)
    static const char *const noValue[] = { "zeroGradient", "noSlip", "slip", "empty", "symmetry", "symmetryPlane",
                                           "cyclic", "wedge", "fixedFluxPressure", NULL };
    bool needsValue = true;
    for (int i = 0; noValue[i]; i++) if (!strcmp(type, noValue[i])) needsValue = false;
    if (!strcmp(type, "inletOutlet") && !HAS("inletValue")) PUT("inletValue", zero);
    if (!strcmp(type, "outletInlet") && !HAS("outletValue")) PUT("outletValue", zero);
    if (!strcmp(type, "fixedGradient") && !HAS("gradient")) PUT("gradient", "uniform 0");
    if (!strcmp(type, "uniformFixedValue") && !HAS("uniformValue")) PUT("uniformValue", vec ? "constant (0 0 0)" : "constant 0");
    if ((!strcmp(type, "totalPressure") || !strcmp(type, "prghTotalPressure")) && !HAS("p0")) PUT("p0", "uniform 0");
    if (!strcmp(type, "prghPressure") && !HAS("p")) PUT("p", "uniform 0");
    if (!strcmp(type, "flowRateInletVelocity") && !HAS("volumetricFlowRate") && !HAS("massFlowRate")) PUT("volumetricFlowRate", "constant 0.001");
    if (!strcmp(type, "turbulentIntensityKineticEnergyInlet") && !HAS("intensity")) PUT("intensity", "0.05");
    if (!strcmp(type, "turbulentMixingLengthDissipationRateInlet") && !HAS("mixingLength")) PUT("mixingLength", "0.01");
    if (!strcmp(type, "turbulentMixingLengthFrequencyInlet") && !HAS("mixingLength")) PUT("mixingLength", "0.01");
    if (!strcmp(type, "externalWallHeatFluxTemperature") && !HAS("mode")) { PUT("mode", "coefficient"); PUT("h", "uniform 10"); PUT("Ta", "constant 300"); PUT("kappaMethod", "fluidThermo"); }
    if (!strcmp(type, "constantAlphaContactAngle") && !HAS("theta0")) { PUT("theta0", "90"); PUT("limit", "gradient"); }
    if (!strcmp(type, "MarshakRadiation") && !HAS("emissivityMode")) { PUT("emissivityMode", "lookup"); PUT("emissivity", "uniform 1"); }
    if (!strcmp(type, "greyDiffusiveRadiation") && !HAS("emissivityMode")) { PUT("emissivityMode", "lookup"); PUT("emissivity", "uniform 1"); }
    if (needsValue && !HAS("value")) {
        char iv[256] = "";
        fd_get(f, "internalField", iv, sizeof iv);
        PUT("value", pl_starts_with(iv, "uniform") ? iv : zero);
    }
    (void)field;
    #undef HAS
    #undef PUT
}

/* ---------------------------------------------------------------- hints */
typedef struct { const char *k, *h; } Hint;
static const Hint HINTS[] = {
    { "application", "Solver that runs this case." },
    { "startFrom", "Where a run begins: startTime, the latest written time, or the first time directory." },
    { "endTime", "Simulated time (or iteration count for steady solvers) at which the run stops." },
    { "deltaT", "Time step. Steady SIMPLE solvers use 1 (one iteration per step)." },
    { "writeControl", "What writeInterval counts: time steps, simulated time, or adjustable run time." },
    { "writeInterval", "How often results are written to disk." },
    { "purgeWrite", "Keep only this many time directories (0 keeps all)." },
    { "maxCo", "Largest allowed Courant number when adjustTimeStep is on." },
    { "maxAlphaCo", "Largest interface Courant number (VOF solvers)." },
    { "nCorrectors", "Pressure correctors per time step (PISO/PIMPLE)." },
    { "nOuterCorrectors", "PIMPLE outer loops per time step. 1 = PISO mode; more allows larger time steps." },
    { "nNonOrthogonalCorrectors", "Extra pressure solves for non-orthogonal meshes. 0 for orthogonal hex meshes." },
    { "momentumPredictor", "Solve the momentum equation before the pressure correction." },
    { "consistent", "SIMPLEC: allows higher pressure relaxation, often converges faster." },
    { "tolerance", "Absolute residual at which the linear solver stops." },
    { "relTol", "Stop when the residual has dropped by this factor (0 = solve to tolerance)." },
    { "pRefCell", "Cell where pressure is fixed when no boundary sets it." },
    { "residualControl", "Steady runs stop once all listed initial residuals are below these values." },
    { "numberOfSubdomains", "Number of MPI processes for a parallel run." },
    { "nu", "Kinematic viscosity [m^2/s]. Water about 1e-6, air about 1.5e-5." },
    { "simulationType", "laminar, RAS (averaged turbulence) or LES (resolved large eddies)." },
    { "radiationModel", "P1: cheap, diffusive, good for optically thick media. fvDOM: ray-based, accurate, costlier." },
    { "solverFreq", "Solve radiation every N flow iterations." },
    { "nPhi", "fvDOM azimuthal divisions per octant (2-D: 4*nPhi rays)." },
    { "nTheta", "fvDOM polar divisions (3-D only)." },
    { "convertToMeters", "Scale applied to blockMesh vertex coordinates." },
    { "scale", "Scale applied to blockMesh vertex coordinates." },
    { NULL, NULL } };

const char *hint_for_key(const char *key)
{
    for (int i = 0; HINTS[i].k; i++) if (!strcmp(HINTS[i].k, key)) return HINTS[i].h;
    return NULL;
}
