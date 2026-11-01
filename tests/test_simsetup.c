/* test_simsetup.c -- the simulation setup: test_simsetup <scratchDir> saves and reads back simulation.ini, writes the case
   for every flow and turbulence model and reads each file back with the dictionary parser, and checks the conditions
   each boundary role gets */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "simsetup.h"
#include "foamdict.h"
#include "stlmesh.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static FDFile *load(const char *dir, const char *rel)
{
    char p[PL_PATH]; pl_join(p, sizeof p, dir, rel);
    FDFile *f = fd_load(p);
    CHECK(f && f->ok, "%s parses (%s)", rel, f ? f->err : "missing");
    return f;
}

static bool is(FDFile *f, const char *key, const char *want)
{
    char v[200] = "";
    if (!f || !fd_get(f, key, v, sizeof v)) return false;
    return !strcmp(v, want);
}

static void setup(SimSetup *s)
{
    sim_defaults(s, 0.5);
    SimPatch p[] = { { "inlet", ROLE_INLET, "patch", true, 0.01 }, { "outlet", ROLE_OUTLET, "patch", true, 0.01 },
                     { "walls", ROLE_WALL, "wall", true, 0.3 }, { "top", ROLE_SYMMETRY, "patch", false, 0 },
                     { "front", ROLE_WALL, "empty", false, 0 } };
    sim_set_boundaries(s, p, 5);
    s->b[0].speed = 10;
}

/* test_simsetup matrix <outDir> <polyMeshDir> <deltaT> name:role:type ...: one case per time x model x algorithm in
   outDir/<name>, its mesh linked, set to run five iterations or time steps (tests/simsetup_solvers.sh runs them) */
static int matrix(int argc, char **argv)
{
    const char *out = argv[2], *mesh = argv[3];
    double dt = atof(argv[4]);
    SimPatch p[32]; char nm[32][64], ty[32][24]; int np = 0;
    static const char *const roles[] = { "wall", "opening", "inlet", "outlet", "symmetry" };
    for (int i = 5; i < argc && np < 32; i++) {
        char role[32] = "";
        if (sscanf(argv[i], "%63[^:]:%31[^:]:%23s", nm[np], role, ty[np]) != 3) continue;
        int r = ROLE_WALL;
        for (int k = 0; k < 5; k++) if (!strcmp(role, roles[k])) r = k;
        p[np] = (SimPatch){ nm[np], r, ty[np], false, 0 };
        np++;
    }
    int n = 0;
    /* heat (plan step 3): the same again with temperature, and with buoyancy (gravity down y); walls 10 K warmer. A
       compressible gas (plan step 4, en == EN__N): air at 10 m/s, its transient outlets non-reflecting */
    for (int en = 0; en <= EN__N; en++)
    for (int flow = 0; flow < FLOW__N; flow++)
        for (int t = 0; t < TURB__N; t++)
            for (int alg = 0; alg < (flow ? ALG__N : 1); alg++) {
                SimSetup s; sim_defaults(&s, 0.1);
                sim_set_boundaries(&s, p, np);
                for (int i = 0; i < s.nB; i++) if (s.b[i].role == ROLE_INLET) s.b[i].speed = 10;
                s.U0[0] = 1;
                s.flow = flow; s.turb = t;
                if (flow) sim_set_algo(&s, alg);
                if (en == EN__N) {
                    sim_set_comp(&s, COMP_GAS);
                    for (int i = 0; i < s.nB; i++) if (s.b[i].role == ROLE_OUTLET) s.b[i].nonRefl = flow != 0;
                } else sim_set_energy(&s, en);
                s.g[0] = 0; s.g[1] = -9.81; s.g[2] = 0;
                for (int i = 0; i < s.nB; i++) if (s.b[i].role == ROLE_WALL) { s.b[i].heat = HT_FIXED; s.b[i].T = s.T0 + 10; }
                sim_settle(&s);
                if (s.turb != t || (flow && s.algo != alg)) continue;
                s.iterations = 5; s.writeEvery = 1000; s.resStop = false;
                s.endTime = 5 * dt; s.deltaT = dt; s.adjustDt = false; s.writeTime = 1000 * dt;
                char name[96]; snprintf(name, sizeof name, "%s%s_%s%s%s", en == EN__N ? "gas_" : en == EN_FORCED ? "heat_" : en == EN_BUOYANT ? "buoyant_" : "", flow ? "transient" : "steady", sim_model_word(t), flow ? "_" : "",
                                        flow ? (alg == ALG_PISO ? "PISO" : alg == ALG_PIMPLE ? "PIMPLE" : "largePIMPLE") : "");
                char dir[PL_PATH], pm[PL_PATH], err[300];
                pl_join(dir, sizeof dir, out, name);
                pl_join(pm, sizeof pm, dir, "constant");
                pl_mkdirs(pm);
                pl_join(pm, sizeof pm, dir, "constant/polyMesh");
                if (symlink(mesh, pm) && !pl_exists(pm)) { printf("cannot link the mesh into %s\n", dir); return 1; }
                if (!sim_write(dir, &s, err, sizeof err)) { printf("%s: %s\n", name, err); return 1; }
                printf("%s\n", name);
                n++;
            }
    fprintf(stderr, "%d cases\n", n);
    return 0;
}

/* test_simsetup cavity <caseDir> <L> <Ra>: the case (its mesh already there: a square L x L, patches hot, cold, adiabatic,
   frontAndBack) set up as de Vahl Davis's differentially heated cavity at that Rayleigh number: air, laminar, steady,
   buoyant, gravity down y, the hot and cold walls dT apart about 293.15 K (tests/simsetup_heat.sh runs it). Prints dT. */
static int cavity(char **argv)
{
    const char *dir = argv[2];
    double L = atof(argv[3]), Ra = atof(argv[4]);
    SimPatch p[] = { { "hot", ROLE_WALL, "wall", false, 0 }, { "cold", ROLE_WALL, "wall", false, 0 },
                     { "adiabatic", ROLE_WALL, "wall", false, 0 }, { "frontAndBack", ROLE_WALL, "empty", false, 0 } };
    SimSetup s; sim_defaults(&s, L);
    sim_set_boundaries(&s, p, 4);
    sim_template_apply(&s, TMPL_NATCONV, L);
    s.g[0] = 0; s.g[1] = -9.81; s.g[2] = 0; s.gAuto = false;
    double alpha = s.kappa / (s.rho * s.cp), dT = Ra * s.nu * alpha / (9.81 * sim_beta(&s) * L * L * L);
    s.b[0].heat = s.b[1].heat = HT_FIXED;
    s.b[0].T = s.T0 + dT / 2; s.b[1].T = s.T0 - dT / 2;
    s.iterations = 4000; s.writeEvery = 4000; s.resStop = true; s.resP = 1e-5; s.resRest = 1e-6;
    s.nuAuto = false; s.nuLength = L;                                    /* Nu = q L / (kappa (Th - Tc)) */
    char err[300];
    if (!sim_save(dir, &s) || !sim_write(dir, &s, err, sizeof err)) { printf("cavity: %s\n", err); return 1; }
    printf("%.10g %.10g\n", dT, s.kappa);
    return 0;
}

/* test_simsetup tallcavity <caseDir> [iterations]: the v2606 buoyantCavity tutorial's mesh (patches hot, cold, topAndBottom,
   frontAndBack, all walls) set up as Betts and Bokhari's tall cavity: air, RANS (k-omega SST), steady, buoyant, gravity
   down y, hot wall 34.6 °C, cold 15 °C, the rest adiabatic, 1000 iterations by default (tests/simsetup_cavity.sh runs it) */
static int tallcavity(char **argv)
{
    const char *dir = argv[2];
    SimPatch p[] = { { "frontAndBack", ROLE_WALL, "wall", false, 0 }, { "topAndBottom", ROLE_WALL, "wall", false, 0 },
                     { "hot", ROLE_WALL, "wall", false, 0 }, { "cold", ROLE_WALL, "wall", false, 0 } };
    SimSetup s; sim_defaults(&s, 2.18);
    sim_set_boundaries(&s, p, 4);
    sim_template_apply(&s, TMPL_NATCONV, 2.18);
    s.turb = TURB_KOMEGASST;
    sim_settle(&s);
    s.g[0] = 0; s.g[1] = -9.81; s.g[2] = 0; s.gAuto = false;
    s.T0 = s.Tref = 293;
    s.b[2].heat = s.b[3].heat = HT_FIXED;
    s.b[2].T = 307.75; s.b[3].T = 288.15;
    s.iterations = argv[3] ? atof(argv[3]) : 1000; s.writeEvery = s.iterations; s.resStop = true;
    s.nuAuto = false; s.nuLength = 0.076;                                /* over the cavity's width */
    char err[300];
    if (!sim_save(dir, &s) || !sim_write(dir, &s, err, sizeof err)) { printf("tallcavity: %s\n", err); return 1; }
    printf("%s %s Ra %.3g\n", sim_app(&s), sim_model_word(s.turb), sim_rayleigh(&s, 2.18));
    return 0;
}

/* test_simsetup nozzle <caseDir> <pe/p0> [transient]: a 2-D half nozzle (patches inlet, outlet, wall, axis
   (symmetryPlane), frontAndBack (empty)) set up from the High-speed gas template as an inviscid nozzle: laminar, slip
   wall, air with a total pressure and temperature (300 K) at the inlet and the static pressure at the outlet pe/p0 of
   the total (tests/simsetup_gas.sh runs it). Prints p0, T0 (absolute), gamma and R. */
static int nozzle(int argc, char **argv)
{
    const char *dir = argv[2];
    double ratio = atof(argv[3]);
    bool tr = argc > 4 && !strcmp(argv[4], "transient");
    SimPatch p[] = { { "inlet", ROLE_INLET, "patch", false, 0 }, { "outlet", ROLE_OUTLET, "patch", false, 0 },
                     { "wall", ROLE_WALL, "wall", false, 0 }, { "axis", ROLE_SYMMETRY, "symmetryPlane", false, 0 },
                     { "frontAndBack", ROLE_WALL, "empty", false, 0 } };
    SimSetup s; sim_defaults(&s, 1);
    sim_set_boundaries(&s, p, 5);
    sim_template_apply(&s, TMPL_GAS, 1);
    s.turb = TURB_LAMINAR;
    s.T0 = 300;
    double p0 = s.pOp / ratio;
    s.b[0].inlet = IN_PRESSURE; s.b[0].p = p0 - s.pOp; s.b[0].T = 300;
    s.b[1].p = 0; s.b[1].T = 300;
    s.b[2].wall = WL_SLIP;
    s.p0 = 0;
    if (tr) {
        s.flow = FLOW_TRANSIENT; sim_set_algo(&s, ALG_PIMPLE);
        s.endTime = 0.03; s.deltaT = 1e-6; s.adjustDt = true; s.maxCo = 0.5; s.writeTime = 0.03;
    } else { s.iterations = 4000; s.writeEvery = 4000; s.resStop = true; s.resP = 1e-6; s.resRest = 1e-6; }
    sim_settle(&s);
    char err[300], chk[2000];
    sim_check(&s, chk, sizeof chk);
    if (!sim_save(dir, &s) || !sim_write(dir, &s, err, sizeof err)) { printf("nozzle: %s\n", err); return 1; }
    printf("%.10g %.10g %.10g %.10g %s %.4g\n", p0, 300.0, s.gamma, SIM_R / s.molW, sim_app(&s), sim_mach(&s));
    if (chk[0]) fprintf(stderr, "checks: %s\n", chk);
    return 0;
}

/* test_simsetup naca <caseDir> [intensity %]: the v2606 rhoSimpleFoam/aerofoilNACA0012 tutorial's mesh (patches freestream, wall, back,
   front) set up from the High-speed gas template as that tutorial is: air at 250 m/s and 298 K, 1 bar, k-omega SST with
   its k 0.01 and omega 10 at the free stream (tests/simsetup_naca.sh compares the two runs) */
static int naca(char **argv)
{
    const char *dir = argv[2];
    SimPatch p[] = { { "freestream", ROLE_OPENING, "patch", false, 0 }, { "wall", ROLE_WALL, "wall", false, 0 },
                     { "back", ROLE_WALL, "empty", false, 0 }, { "front", ROLE_WALL, "empty", false, 0 } };
    SimSetup s; sim_defaults(&s, 1);
    sim_set_boundaries(&s, p, 4);
    sim_template_apply(&s, TMPL_GAS, 1);
    s.pOp = 1e5; s.T0 = 298; s.p0 = 0;
    s.U0[0] = 250; s.U0[1] = s.U0[2] = 0;
    SimBoundary *f = &s.b[0];
    f->farField = true; f->vec[0] = 250; f->vec[1] = f->vec[2] = 0; f->p = 0; f->T = 298;
    /* k = 1.5 (U I)², omega = k^½ / (Cmu^¼ L): the tutorial's k 0.01, omega 10 */
    f->intensity = s.intensity = 100 * sqrt(0.01 / 1.5) / 250;
    f->length = s.length = sqrt(0.01) / (pow(0.09, 0.25) * 10);
    if (argv[3]) { f->intensity = s.intensity = atof(argv[3]); f->length = s.length = 0.01; }   /* a turbulent free stream instead */
    s.iterations = 1000; s.writeEvery = 1000; s.resStop = true; s.resP = 1e-4; s.resRest = 1e-4;
    sim_settle(&s);
    char err[300];
    if (!sim_save(dir, &s) || !sim_write(dir, &s, err, sizeof err)) { printf("naca: %s\n", err); return 1; }
    printf("%s Ma %.3g rho %.6g\n", sim_app(&s), sim_mach(&s), s.rho);
    return 0;
}

/* test_simsetup yplus <caseDir> <U> <L> <firstH>: the y+ helper's estimate for that case's setup (its simulation.ini) with a
   first cell firstH high, at speed U over length L (tests/simsetup_naca.sh holds it against the yPlus function object) */
static int yplus(char **argv)
{
    SimSetup s;
    if (!sim_load(argv[2], &s, 1)) { printf("no simulation.ini in %s\n", argv[2]); return 1; }
    double h1, d;
    sim_wall_spacing(&s, atof(argv[3]), atof(argv[4]), sim_yplus_target(&s), &h1, &d, NULL);
    printf("%.6g %.6g %.6g\n", sim_yplus_of(&s, atof(argv[3]), atof(argv[4]), atof(argv[5])), h1, d);
    return 0;
}

/* test_simsetup plate <caseDir>: a 2-D flat plate (patches inlet, outlet, top, front (symmetryPlane, upstream of the plate),
   plate (wall), frontAndBack (empty)) set up as the ERCOFTAC T3A case: air at 5.4 m/s, 3.3 % turbulence at a viscosity
   ratio of 12, k-omega SST with transition (resolved walls), steady (tests/simsetup_transition.sh runs it) */
static int plate(char **argv)
{
    SimPatch p[] = { { "inlet", ROLE_INLET, "patch", false, 0 }, { "outlet", ROLE_OUTLET, "patch", false, 0 },
                     { "top", ROLE_SYMMETRY, "patch", false, 0 }, { "front", ROLE_SYMMETRY, "symmetryPlane", false, 0 },
                     { "plate", ROLE_WALL, "wall", false, 0 }, { "frontAndBack", ROLE_WALL, "empty", false, 0 } };
    SimSetup s; sim_defaults(&s, 2);
    sim_set_boundaries(&s, p, 6);
    sim_template_apply(&s, TMPL_INTERNAL, 2);
    s.turb = TURB_SSTLM; sim_settle(&s);
    s.nu = 1.5e-5; s.fluid = FLUID_CUSTOM; s.rho = 1.2;
    s.b[0].speed = 5.4; s.b[0].turbSpec = TI_RATIO; s.b[0].intensity = 3.3; s.b[0].ratio = 12;
    s.U0[0] = 5.4; s.intensity = 3.3;
    s.iterations = 6000; s.writeEvery = 6000; s.resStop = true; s.resP = 1e-5; s.resRest = 1e-5;
    char err[300];
    if (!sim_save(argv[2], &s) || !sim_write(argv[2], &s, err, sizeof err)) { printf("plate: %s\n", err); return 1; }
    printf("%s %s %s\n", sim_app(&s), sim_model_word(s.turb), WALLT_NAME[sim_wall_treatment(&s)]);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[1], "plate")) return plate(argv);
    if (argc >= 6 && !strcmp(argv[1], "yplus")) return yplus(argv);
    if (argc >= 3 && !strcmp(argv[1], "naca")) return naca(argv);
    if (argc >= 4 && !strcmp(argv[1], "nozzle")) return nozzle(argc, argv);
    if (argc >= 3 && !strcmp(argv[1], "tallcavity")) return tallcavity(argv);
    if (argc >= 5 && !strcmp(argv[1], "matrix")) return matrix(argc, argv);
    if (argc >= 5 && !strcmp(argv[1], "cavity")) return cavity(argv);
    if (argc < 2) { printf("usage: test_simsetup <scratchDir>\n"); return 2; }
    char dir[PL_PATH]; pl_join(dir, sizeof dir, argv[1], "simsetup");
    pl_remove_tree(dir); pl_mkdirs(dir);
    SimSetup s, r;
    setup(&s);
    CHECK(s.nB == 5 && s.b[0].role == ROLE_INLET && s.b[2].role == ROLE_WALL, "boundaries set");
    CHECK(s.length > 0.034 && s.length < 0.036, "mixing length 7 %% of the size: %g", s.length);

    /* simulation.ini round trip */
    s.turb = TURB_SPALART; s.flow = FLOW_TRANSIENT; s.b[1].p = 250; s.b[2].wall = WL_MOVING; s.b[2].wallU[0] = 3; s.U0[2] = -1.5;
    sim_fluid(&s, FLUID_WATER);
    CHECK(sim_save(dir, &s), "saved");
    CHECK(sim_load(dir, &r, 0.5), "loaded");
    CHECK(r.turb == TURB_SPALART && r.flow == FLOW_TRANSIENT && r.fluid == FLUID_WATER && r.rho > 998 && r.nB == 5, "globals read back");
    CHECK(!strcmp(r.b[1].name, "outlet") && r.b[1].p == 250 && r.b[2].wall == WL_MOVING && r.b[2].wallU[0] == 3 && r.U0[2] == -1.5, "boundaries read back");
    CHECK(!strcmp(r.b[4].meshType, "empty"), "mesh type read back");

    /* the boundaries again, one renamed and one new: values kept by name, the STL's roles win, the others' stay */
    sim_rename(&r, "walls", "body");
    r.b[3].role = ROLE_OPENING;                                     /* top: the user's choice, not from the STL */
    SimPatch p2[] = { { "inlet", ROLE_INLET, "patch", true, 0.01 }, { "outlet", ROLE_WALL, "wall", true, 0.01 },
                      { "body", ROLE_WALL, "wall", true, 0.3 }, { "top", ROLE_SYMMETRY, "patch", false, 0 }, { "extra", ROLE_WALL, "wall", false, 0 } };
    sim_set_boundaries(&r, p2, 5);
    CHECK(r.nB == 5 && r.b[2].wall == WL_MOVING, "rename keeps the values");
    CHECK(r.b[1].role == ROLE_WALL && r.b[3].role == ROLE_OPENING && r.b[4].role == ROLE_WALL, "STL roles win, the user's stay");

    /* the checks */
    char msg[2000];
    setup(&s);
    CHECK(sim_check(&s, msg, sizeof msg) == 0, "a good setup has no warnings: %s", msg);
    s.b[1].role = ROLE_WALL;
    CHECK(sim_check(&s, msg, sizeof msg) >= 1 && strstr(msg, "no way out") && strstr(msg, "no wall functions"), "no outlet: %s", msg);
    s.b[1].role = ROLE_OUTLET; s.b[0].speed = 0;
    CHECK(sim_check(&s, msg, sizeof msg) == 1 && strstr(msg, "inlet speed"), "zero speed: %s", msg);

    /* every time x turbulence model x algorithm (step 1 of the setup plan, 2026-10-27) writes a case the parser reads,
       with the right solver, model, coupling and conditions */
    char err[300];
    int combos = 0;
    for (int flow = 0; flow < FLOW__N; flow++)
        for (int t = 0; t < TURB__N; t++)
            for (int alg = 0; alg < (flow ? ALG__N : 1); alg++) {
                setup(&s);
                s.flow = flow; s.turb = t;
                if (flow) sim_set_algo(&s, alg);
                sim_settle(&s);
                if (s.turb != t) {                       /* a steady LES or DES: settled to its RANS model, not written */
                    int a = sim_approach(t);
                    CHECK(!flow && (a == APPR_LES || a == APPR_DES) && sim_approach(s.turb) == APPR_RANS, "settle %d/%d", flow, t);
                    continue;
                }
                combos++;
                CHECK(sim_write(dir, &s, err, sizeof err), "write %d/%d/%d: %s", flow, t, alg, err);
                const char *f[SIM_MAX_F];
                int nf = sim_fields(&s, f);
                for (int i = 0; i < nf; i++) {
                    char rel[32]; snprintf(rel, sizeof rel, "0/%s", f[i]);
                    FDFile *ff = load(dir, rel);
                    CHECK(is(ff, "boundaryField/front/type", "empty"), "%s: front is empty", rel);
                    char ty[80] = "";
                    CHECK(fd_get(ff, "boundaryField/walls/type", ty, sizeof ty) && fd_get(ff, "boundaryField/inlet/type", ty, sizeof ty)
                          && fd_get(ff, "boundaryField/outlet/type", ty, sizeof ty) && fd_get(ff, "boundaryField/top/type", ty, sizeof ty),
                          "%s: a condition on every patch (%d/%d/%d)", rel, flow, t, alg);
                    fd_free(ff);
                }
                static const char *const gone[] = { "0/k", "0/omega", "0/epsilon", "0/nuTilda", "0/nut", "0/ReThetat", "0/gammaInt" };
                for (int g = 0; g < 7; g++) {
                    bool used = false;
                    for (int i = 0; i < nf; i++) if (!strcmp(gone[g] + 2, f[i])) used = true;
                    char p[PL_PATH]; pl_join(p, sizeof p, dir, gone[g]);
                    CHECK(used == pl_exists(p), "%s there only when the model solves it (%d/%d)", gone[g], flow, t);
                }
                FDFile *U = load(dir, "0/U"), *P = load(dir, "0/p");
                CHECK(is(U, "boundaryField/inlet/type", "surfaceNormalFixedValue") && is(U, "boundaryField/inlet/refValue", "uniform -10"), "inlet U");
                CHECK(is(U, "boundaryField/outlet/type", "inletOutlet") && is(P, "boundaryField/outlet/type", "fixedValue"), "outlet");
                CHECK(is(U, "boundaryField/walls/type", "noSlip") && is(U, "boundaryField/top/type", "slip"), "wall and symmetry");
                fd_free(U); fd_free(P);
                int a = sim_approach(t);
                FDFile *c = load(dir, "system/controlDict"), *tp = load(dir, "constant/turbulenceProperties");
                const char *app = !flow ? "simpleFoam" : alg == ALG_PISO ? "pisoFoam" : "pimpleFoam";
                CHECK(is(c, "application", app) && !strcmp(sim_app(&s), app), "application %s", app);
                CHECK(is(tp, "simulationType", a == APPR_LAMINAR ? "laminar" : a == APPR_RANS ? "RAS" : "LES"), "simulationType (%d)", t);
                if (a == APPR_RANS) CHECK(is(tp, "RAS/RASModel", sim_model_word(t)), "RASModel %s", sim_model_word(t));
                if (a == APPR_LES || a == APPR_DES) {
                    CHECK(is(tp, "LES/LESModel", sim_model_word(t)), "LESModel %s", sim_model_word(t));
                    CHECK(is(tp, "LES/delta", t == TURB_SAIDDES ? "IDDESDelta" : a == APPR_DES ? "maxDeltaxyzCubeRoot" : "cubeRootVol"), "delta (%d)", t);
                }
                if (flow && alg == ALG_PISO) CHECK(is(c, "adjustTimeStep", "no"), "pisoFoam: a fixed step");
                fd_free(c); fd_free(tp);
                FDFile *fs = load(dir, "system/fvSolution"), *sc = load(dir, "system/fvSchemes");
                const char *coup = !flow ? "SIMPLE" : alg == ALG_PISO ? "PISO" : "PIMPLE";
                CHECK(fd_find(fs, coup) != NULL, "pressure-velocity coupling %s", coup);
                if (flow && alg == ALG_LARGE) CHECK(is(fs, "PIMPLE/nOuterCorrectors", "20") && fd_find(fs, "PIMPLE/residualControl") && fd_find(fs, "relaxationFactors"), "large steps");
                if (flow && alg == ALG_PIMPLE) CHECK(is(fs, "PIMPLE/nOuterCorrectors", "1") && is(fs, "PIMPLE/nCorrectors", "2"), "PIMPLE correctors");
                bool eddies = a == APPR_LES || a == APPR_DES;
                CHECK(is(sc, "ddtSchemes/default", !flow ? "steadyState" : eddies ? "backward" : "Euler"), "ddt (%d/%d)", flow, t);
                CHECK(is(sc, "divSchemes/div(phi,U)", !flow ? "bounded Gauss linearUpwind grad(U)" : eddies ? "Gauss LUST grad(U)" : "Gauss linearUpwind grad(U)"), "div(phi,U)");
                for (int i = 2; i < nf; i++) if (strcmp(f[i], "nut")) { char k[64]; snprintf(k, sizeof k, "divSchemes/div(phi,%s)", f[i]); CHECK(fd_find(sc, k) != NULL, "%s", k); }
                fd_free(fs); fd_free(sc);
                if (t == TURB_KOMEGASST) {
                    FDFile *k = load(dir, "0/k"), *nut = load(dir, "0/nut");
                    CHECK(is(k, "boundaryField/inlet/type", "turbulentIntensityKineticEnergyInlet") && is(k, "boundaryField/walls/type", "kqRWallFunction"), "k");
                    CHECK(is(nut, "boundaryField/walls/type", "nutkWallFunction"), "nut");
                    fd_free(k); fd_free(nut);
                }
                if (t == TURB_WALE || t == TURB_SADDES) {
                    FDFile *nut = load(dir, "0/nut");
                    CHECK(is(nut, "boundaryField/walls/type", "nutUSpaldingWallFunction"), "LES and SA walls: Spalding");
                    fd_free(nut);
                }
                if (t == TURB_SSTLM) {
                    FDFile *rt = load(dir, "0/ReThetat"), *gi = load(dir, "0/gammaInt"), *nut = load(dir, "0/nut");
                    char v[64] = ""; fd_get(rt, "boundaryField/inlet/value", v, sizeof v);
                    double re = atof(v + 8);                    /* "uniform 123" */
                    CHECK(re > 110 && re < 135, "ReThetat at 5 %% intensity (Langtry-Menter: 122): %s", v);
                    CHECK(is(gi, "boundaryField/inlet/value", "uniform 1") && is(gi, "boundaryField/walls/type", "zeroGradient"), "gammaInt");
                    CHECK(is(nut, "boundaryField/walls/type", "nutLowReWallFunction"), "transition: low-Re walls");
                    fd_free(rt); fd_free(gi); fd_free(nut);
                }
            }
    CHECK(combos == 7 + 13 * 3, "every combination written: %d", combos);

    /* the time scheme: automatic, or picked */
    setup(&s); s.flow = FLOW_TRANSIENT; s.turb = TURB_KOMEGASST; s.timeScheme = TS_CN;
    CHECK(sim_write(dir, &s, err, sizeof err), "write CN: %s", err);
    { FDFile *sc = load(dir, "system/fvSchemes"); CHECK(is(sc, "ddtSchemes/default", "CrankNicolson 0.9"), "Crank-Nicolson"); fd_free(sc); }

    /* the approach and the time keep each other consistent */
    setup(&s);
    CHECK(sim_set_approach(&s, APPR_LES) != NULL && s.flow == FLOW_TRANSIENT && s.turb == TURB_WALE, "LES makes the run transient, WALE");
    CHECK(sim_set_approach(&s, APPR_DES) == NULL && s.turb == TURB_SADDES, "LES to DES: SA-DDES");
    s.flow = FLOW_STEADY;
    CHECK(sim_settle(&s) != NULL && s.turb == TURB_SPALART, "a steady DES: its RANS model");
    s.turb = TURB_KOMEGASST; s.flow = FLOW_TRANSIENT;
    sim_set_approach(&s, APPR_DES);
    CHECK(s.turb == TURB_SSTDDES, "SST to DES: SST-DDES");
    sim_set_approach(&s, APPR_RANS);
    CHECK(s.turb == TURB_KOMEGASST, "back to RANS: SST");
    CHECK(!strcmp(sim_approach_label(&s, APPR_RANS), "URANS"), "transient RANS is URANS");
    sim_set_algo(&s, ALG_LARGE);
    CHECK(s.nOuter == 20 && s.maxCo >= 5, "large steps: 20 outer correctors, Courant 5");
    sim_set_algo(&s, ALG_PISO);
    CHECK(s.nOuter == 1 && s.maxCo <= 1, "PISO: one, Courant 1");
    /* the new settings in simulation.ini, and an old one without them */
    s.turb = TURB_SAIDDES; s.algo = ALG_LARGE; s.nOuter = 12; s.nCorr = 3; s.timeScheme = TS_BACKWARD;
    CHECK(sim_save(dir, &s) && sim_load(dir, &r, 0.5), "saved and loaded");
    CHECK(r.turb == TURB_SAIDDES && r.algo == ALG_LARGE && r.nOuter == 12 && r.nCorr == 3 && r.timeScheme == TS_BACKWARD, "model controls read back");
    {
        char ip[PL_PATH]; sim_ini_path(dir, ip, sizeof ip);
        const char *old = "flow = transient\nturbulence = realizableKE\nfluid = air\n";
        pl_write_file(ip, old, strlen(old));
        CHECK(sim_load(dir, &r, 0.5) && r.turb == TURB_REALIZABLE && r.algo == ALG_PIMPLE && r.nOuter == 1 && r.nCorr == 2 && r.timeScheme == TS_AUTO,
              "an old simulation.ini: as before, PIMPLE");
    }
    /* the checks know the approach: LES on a 2-D mesh, a loose Courant limit */
    setup(&s); s.flow = FLOW_TRANSIENT; s.turb = TURB_WALE; s.maxCo = 4;
    CHECK(sim_check(&s, msg, sizeof msg) == 2 && strstr(msg, "2-D") && strstr(msg, "Courant"), "LES checks: %s", msg);

    /* hand edits survive the next write unless the setup changes the same setting */
    setup(&s);
    CHECK(sim_write(dir, &s, err, sizeof err), "write: %s", err);
    {
        FDFile *sc = load(dir, "system/fvSchemes");
        fd_put(sc, "divSchemes/div(phi,U)", "bounded Gauss upwind");      /* changed */
        fd_put(sc, "fluxRequired/default", "no");                         /* added (a new dict) */
        CHECK(fd_save(sc), "fvSchemes saved"); fd_free(sc);
        FDFile *cdf = load(dir, "system/controlDict");
        fd_put(cdf, "functions/myProbe/type", "probes");                  /* the user's own function object */
        fd_remove(cdf, "writePrecision");                                 /* removed */
        CHECK(fd_save(cdf), "controlDict saved"); fd_free(cdf);
        char fsp[PL_PATH]; pl_join(fsp, sizeof fsp, dir, "system/fvSolution");
        char *ft = pl_read_file(fsp, NULL);
        Str nt; str_init(&nt); str_cat(&nt, ft); str_cat(&nt, "\n#include \"myExtras\"\n"); pl_write_file(fsp, nt.s, nt.len);
        str_free(&nt); free(ft);
        FDFile *fs = load(dir, "system/fvSolution");
        fd_put(fs, "SIMPLE/residualControl/p", "1e-3");                   /* a setup value, edited */
        CHECK(fd_save(fs), "fvSolution saved"); fd_free(fs);
    }
    CHECK(sim_write(dir, &s, err, sizeof err), "write again: %s", err);
    {
        FDFile *sc = load(dir, "system/fvSchemes"), *cdf = load(dir, "system/controlDict"), *fs = load(dir, "system/fvSolution");
        CHECK(is(sc, "divSchemes/div(phi,U)", "bounded Gauss upwind"), "a changed entry kept");
        CHECK(is(sc, "fluxRequired/default", "no"), "an added dictionary kept");
        CHECK(is(cdf, "functions/myProbe/type", "probes") && fd_find(cdf, "functions/residuals"), "the user's function object kept beside ours");
        CHECK(!fd_find(cdf, "writePrecision"), "a removed entry stays removed");
        CHECK(is(fs, "SIMPLE/residualControl/p", "1e-3"), "an edited setup value kept while the setup's is unchanged");
        char *ft = pl_read_file(fs->path, NULL);
        CHECK(ft && strstr(ft, "#include \"myExtras\""), "the user's #include kept");
        free(ft);
        CHECK(strstr(sim_write_kept(), "div(phi,U)") && strstr(sim_write_kept(), "myProbe") && !sim_write_replaced()[0], "kept listed: %s", sim_write_kept());
        fd_free(sc); fd_free(cdf); fd_free(fs);
    }
    s.resP = 5e-5;                                                         /* the setup changes the same setting: it wins */
    CHECK(sim_write(dir, &s, err, sizeof err), "write with a new target: %s", err);
    {
        FDFile *fs = load(dir, "system/fvSolution"), *sc = load(dir, "system/fvSchemes");
        CHECK(is(fs, "SIMPLE/residualControl/p", "5e-05"), "the setup's new value replaces the hand edit");
        CHECK(strstr(sim_write_replaced(), "residualControl p") != NULL, "replaced listed: %s", sim_write_replaced());
        CHECK(is(sc, "divSchemes/div(phi,U)", "bounded Gauss upwind"), "the other hand edit still kept");
        fd_free(fs); fd_free(sc);
    }
    {   /* edited by foamDictionary -set, which drops our comment line: still ours, merged, no .orig */
        char fsp[PL_PATH], orig[PL_PATH + 8]; pl_join(fsp, sizeof fsp, dir, "system/fvSchemes"); snprintf(orig, sizeof orig, "%s.orig", fsp);
        unlink(orig);
        char *ft = pl_read_file(fsp, NULL);
        char *m = ft ? strstr(ft, "// Written by Q-Continuum") : NULL;
        if (m) { char *e = strchr(strchr(m, '\n') + 1, '\n'); memmove(m, e + 1, strlen(e + 1) + 1); }
        if (ft) pl_write_file(fsp, ft, strlen(ft));
        free(ft);
        FDFile *sc = load(dir, "system/fvSchemes");
        fd_put(sc, "laplacianSchemes/default", "Gauss linear corrected");
        CHECK(fd_save(sc), "saved"); fd_free(sc);
        CHECK(sim_write(dir, &s, err, sizeof err), "write: %s", err);
        sc = load(dir, "system/fvSchemes");
        CHECK(is(sc, "laplacianSchemes/default", "Gauss linear corrected") && !pl_exists(orig), "no comment line: still ours, the edit kept, no .orig");
        fd_free(sc);
    }
    s.turb = TURB_KEPSILON;                                                /* a model change: our entries move, the user's stay */
    CHECK(sim_write(dir, &s, err, sizeof err), "write k-epsilon: %s", err);
    {
        FDFile *sc = load(dir, "system/fvSchemes");
        CHECK(fd_find(sc, "divSchemes/div(phi,epsilon)") && !fd_find(sc, "divSchemes/div(phi,omega)"), "the model's entries follow it");
        fd_free(sc);
    }

    /* other inlets and walls */
    setup(&s);
    s.b[0].inlet = IN_FLOW; s.b[0].flow = 0.02;
    s.b[1].role = ROLE_OPENING; s.b[1].p = 1204;                     /* 1204 Pa / 1.204 kg/m³ = 1000 m²/s² */
    s.b[2].wall = WL_MOVING; s.b[2].wallU[1] = 2;
    CHECK(sim_write(dir, &s, err, sizeof err), "write: %s", err);
    FDFile *U = load(dir, "0/U"), *P = load(dir, "0/p");
    CHECK(is(U, "boundaryField/inlet/type", "flowRateInletVelocity") && is(U, "boundaryField/inlet/volumetricFlowRate", "0.02"), "flow rate inlet");
    CHECK(is(P, "boundaryField/outlet/type", "totalPressure") && is(P, "boundaryField/outlet/p0", "uniform 1000"), "opening, kinematic pressure");
    CHECK(is(U, "boundaryField/walls/type", "movingWallVelocity") && is(U, "boundaryField/walls/value", "uniform (0 2 0)"), "moving wall");
    fd_free(U); fd_free(P);
    s.b[0].inlet = IN_PRESSURE; s.b[0].p = 500;
    CHECK(sim_write(dir, &s, err, sizeof err), "write: %s", err);
    U = load(dir, "0/U"); P = load(dir, "0/p");
    CHECK(is(U, "boundaryField/inlet/type", "pressureInletOutletVelocity") && is(P, "boundaryField/inlet/type", "totalPressure"), "pressure inlet");
    fd_free(U); fd_free(P);
    /* a user's own file is kept as .orig once; ours are not */
    char p[PL_PATH]; pl_join(p, sizeof p, dir, "system/fvSchemes.orig");
    CHECK(!pl_exists(p), "no .orig for our own file");
    char cd[PL_PATH]; pl_join(cd, sizeof cd, dir, "system/controlDict");
    pl_write_file(cd, "application icoFoam;\n", 21);
    CHECK(sim_write(dir, &s, err, sizeof err), "write over the user's: %s", err);
    pl_join(p, sizeof p, dir, "system/controlDict.orig");
    char *o = pl_read_file(p, NULL);
    CHECK(o && strstr(o, "icoFoam"), "the user's controlDict kept as .orig");
    free(o);

    /* bug shakedown 2026-10-26: the boundaries kept as they were, passed back by pointer into s->b (refresh while the
       mesh is being made) come through intact, whatever order they are in */
    setup(&s);
    {
        SimPatch keep[5];
        for (int i = 0; i < 5; i++) keep[i] = (SimPatch){ s.b[4 - i].name, s.b[4 - i].role, s.b[4 - i].meshType, false, 0 };
        sim_set_boundaries(&s, keep, 5);
        CHECK(s.nB == 5 && !strcmp(s.b[0].name, "front") && !strcmp(s.b[4].name, "inlet") && !strcmp(s.b[0].meshType, "empty")
              && !strcmp(s.b[4].meshType, "patch") && s.b[4].speed == 10, "reversed in place: %s %s %s %s", s.b[0].name, s.b[4].name, s.b[0].meshType, s.b[4].meshType);
    }
    /* the run controls of our controlDict, edited elsewhere, come back into the setup; a controlDict not ours doesn't */
    setup(&s);
    CHECK(sim_write(dir, &s, err, sizeof err), "write: %s", err);
    CHECK(sim_save(dir, &s), "setup saved");
    FDFile *cdf = load(dir, "system/controlDict");
    fd_put(cdf, "endTime", "2500"); fd_put(cdf, "writeInterval", "250");
    CHECK(fd_save(cdf), "controlDict saved");
    fd_free(cdf);
    CHECK(sim_take_controls(dir, &s) && s.iterations == 2500 && s.writeEvery == 250, "steady controls taken: %g %g", s.iterations, s.writeEvery);
    CHECK(!sim_take_controls(dir, &s), "nothing new the second time");
    s.flow = FLOW_TRANSIENT;
    CHECK(sim_write(dir, &s, err, sizeof err), "write: %s", err);
    cdf = load(dir, "system/controlDict");
    fd_put(cdf, "deltaT", "0.0005"); fd_put(cdf, "maxCo", "0.8"); fd_put(cdf, "adjustTimeStep", "no");
    CHECK(fd_save(cdf), "controlDict saved");
    fd_free(cdf);
    CHECK(sim_take_controls(dir, &s) && s.deltaT == 0.0005 && s.maxCo == 0.8 && !s.adjustDt, "transient controls taken");
    { const char *t = "application icoFoam;\nendTime 7;\n"; pl_write_file(cd, t, strlen(t)); }
    CHECK(!sim_take_controls(dir, &s), "a controlDict for another solver is left out");
    { const char *t = "FoamFile { version 2.0; format ascii; class dictionary; object controlDict; }\napplication pimpleFoam;\nendTime 9;\n"; pl_write_file(cd, t, strlen(t)); }
    CHECK(sim_take_controls(dir, &s) && s.endTime == 9, "ours without the comment (foamDictionary -set drops it)");
    char ini[PL_PATH]; sim_ini_path(dir, ini, sizeof ini); unlink(ini);
    { const char *t = "FoamFile { version 2.0; format ascii; class dictionary; object controlDict; }\napplication pimpleFoam;\nendTime 11;\n"; pl_write_file(cd, t, strlen(t)); }
    CHECK(!sim_take_controls(dir, &s), "no setup: nothing taken (a tutorial)");

    /* heat (plan step 3): buoyantSimpleFoam's files, absolute p_rgh, T per boundary, Boussinesq, g, the wall heat flux */
    setup(&s);
    CHECK(s.energy == EN_OFF && s.Tref > 293 && s.cp > 1000 && s.kappa > 0.02, "heat off by default, air's properties");
    sim_set_energy(&s, EN_BUOYANT);
    s.b[0].T = 300;                                                      /* inlet */
    s.b[1].T = 295; s.b[1].p = 50;                                       /* outlet: backflow, gauge pressure */
    s.b[2].heat = HT_FLUX; s.b[2].q = 500;                              /* walls */
    s.g[0] = 0; s.g[1] = -9.81; s.g[2] = 0;
    s.nuAuto = false; s.nuLength = 0.2; s.nuDT = 0;
    CHECK(!strcmp(sim_app(&s), "buoyantSimpleFoam") && s.heatReport, "steady heat: buoyantSimpleFoam, heat report on");
    CHECK(sim_save(dir, &s) && sim_load(dir, &r, 0.5), "heat saved and loaded");
    CHECK(r.energy == EN_BUOYANT && r.b[2].heat == HT_FLUX && r.b[2].q == 500 && r.b[0].T == 300 && r.g[1] == -9.81 && r.heatReport && !r.nuAuto && r.nuLength == 0.2 && r.nuDT == 0, "heat read back");
    CHECK(sim_write(dir, &s, err, sizeof err), "write heat: %s", err);
    {
        FDFile *pr = load(dir, "0/p_rgh"), *P = load(dir, "0/p"), *T = load(dir, "0/T"), *at = load(dir, "0/alphat"), *th = load(dir, "constant/thermophysicalProperties");
        FDFile *g = load(dir, "constant/g"), *c = load(dir, "system/controlDict"), *fs = load(dir, "system/fvSolution"), *sc = load(dir, "system/fvSchemes");
        CHECK(is(pr, "internalField", "uniform 100000") && is(pr, "boundaryField/outlet/type", "prghPressure") && is(pr, "boundaryField/outlet/p", "uniform 100050"),
              "p_rgh absolute, the outlet's fixed");
        CHECK(is(pr, "boundaryField/inlet/type", "fixedFluxPressure") && is(pr, "boundaryField/walls/type", "fixedFluxPressure"), "p_rgh fixedFluxPressure on inlets and walls");
        CHECK(is(P, "boundaryField/walls/type", "calculated") && is(P, "dimensions", "[1 -1 -2 0 0 0 0]"), "p calculated, in Pa");
        CHECK(is(T, "boundaryField/inlet/value", "uniform 300") && is(T, "boundaryField/outlet/type", "inletOutlet") && is(T, "boundaryField/outlet/inletValue", "uniform 295"), "T inlet and outlet");
        CHECK(is(T, "boundaryField/walls/type", "externalWallHeatFluxTemperature") && is(T, "boundaryField/walls/mode", "flux") && is(T, "boundaryField/walls/q", "uniform 500"), "T heat-flux wall");
        CHECK(is(T, "boundaryField/top/type", "zeroGradient") && is(T, "boundaryField/front/type", "empty"), "T symmetry and empty");
        CHECK(is(at, "boundaryField/walls/type", "compressible::alphatWallFunction"), "alphat wall function");
        { char mw[64] = ""; fd_get(th, "mixture/specie/molWeight", mw, sizeof mw);   /* 1.204 kg/m³ at 293.15 K and 1 bar: 29.35 */
          CHECK(is(th, "thermoType/equationOfState", "perfectGas") && fabs(atof(mw) - 29.35) < 0.01 && is(th, "mixture/thermodynamics/Cp", "1005"), "air an ideal gas: %s", mw); }
        CHECK(is(g, "value", "(0 -9.81 0)"), "g");
        CHECK(is(c, "application", "buoyantSimpleFoam"), "controlDict: solver");
        CHECK(is(c, "functions/wallHeatFlux/type", "wallHeatFlux"), "wall heat flux");
        { char v[200] = ""; fd_get(c, "functions/wallHeatFlux/patches", v, sizeof v); CHECK(!strcmp(v, "(walls)"), "wall heat flux patches: %s", v); }
        CHECK(is(c, "functions/wallHeat_walls/type", "surfaceFieldValue") && is(c, "functions/wallHeat_walls/name", "walls") &&
              is(c, "functions/wallHeat_walls/operation", "areaAverage") && is(c, "functions/wallHeat_walls/fields", "(wallHeatFlux T)"), "heated wall's mean flux and temperature");
        {   /* Nu = q L / (kappa dT): dT the span the boundaries set, the inlet's 300 K to the start's 293.15 K */
            char v[64] = ""; fd_get(c, "functions/Nu_walls/scaleFactor", v, sizeof v);
            double want = 0.2 / (s.kappa * (300 - 293.15));
            CHECK(fabs(atof(v) - want) < 1e-6 * want && is(c, "functions/Nu_walls/fields", "(wallHeatFlux)"), "Nusselt number's scale: %s (want %.6g)", v, want);
            CHECK(!fd_find(c, "functions/wallHeat_inlet") && !fd_find(c, "functions/Nu_top"), "Nusselt only on walls that pass heat");
        }
        CHECK(is(c, "functions/residuals/fields", "(U p_rgh h k omega)"), "residuals: p_rgh and h");
        CHECK(is(fs, "SIMPLE/momentumPredictor", "no") && is(fs, "SIMPLE/pRefValue", "100000") && is(fs, "relaxationFactors/fields/p_rgh", "0.7"), "buoyant SIMPLE");
        CHECK(is(fs, "SIMPLE/residualControl/p_rgh", "0.0001") && fd_find(fs, "solvers/p_rgh"), "p_rgh solved and its target");
        CHECK(fd_find(sc, "divSchemes/div(phi,h)") && fd_find(sc, "divSchemes/div(phi,K)") && fd_find(sc, "divSchemes/div(((rho*nuEff)*dev2(T(grad(U)))))"), "energy schemes");
        fd_free(pr); fd_free(P); fd_free(T); fd_free(at); fd_free(th); fd_free(g); fd_free(c); fd_free(fs); fd_free(sc);
    }
    {   /* no temperature difference known (a flux wall, everything else at the start's): the means, no Nusselt number; a typed dT gives it */
        SimSetup h = s;
        h.b[0].T = h.b[1].T = h.T0;
        CHECK(sim_delta_T(&h) == 0 && sim_write(dir, &h, err, sizeof err), "write heat, no dT: %s", err);
        FDFile *c = load(dir, "system/controlDict");
        CHECK(fd_find(c, "functions/wallHeat_walls") && !fd_find(c, "functions/Nu_walls"), "no dT: no Nusselt number");
        fd_free(c);
        h.nuDT = 4;
        CHECK(sim_write(dir, &h, err, sizeof err), "write heat, typed dT: %s", err);
        c = load(dir, "system/controlDict");
        char v[64] = ""; fd_get(c, "functions/Nu_walls/scaleFactor", v, sizeof v);
        CHECK(fabs(atof(v) - 0.2 / (h.kappa * 4)) < 1e-6 * atof(v), "typed dT: %s", v);
        fd_free(c);
    }
    s.b[2].heat = HT_CONVECT; s.b[2].h = 25; s.b[2].Ta = 280;
    s.b[3].heat = HT_POWER;                                              /* symmetry: no heat whatever is set */
    CHECK(sim_write(dir, &s, err, sizeof err), "write convection: %s", err);
    {
        FDFile *T = load(dir, "0/T");
        CHECK(is(T, "boundaryField/walls/mode", "coefficient") && is(T, "boundaryField/walls/h", "uniform 25") && is(T, "boundaryField/walls/Ta", "constant 280"), "convection wall");
        CHECK(is(T, "boundaryField/top/type", "zeroGradient"), "symmetry: no heat");
        fd_free(T);
    }
    s.flow = FLOW_TRANSIENT; sim_set_algo(&s, ALG_PISO);
    CHECK(s.algo == ALG_PIMPLE && !strcmp(sim_app(&s), "buoyantPimpleFoam") && sim_algo_why_not(&s, ALG_PISO), "heat, transient: buoyantPimpleFoam, no PISO");
    CHECK(sim_write(dir, &s, err, sizeof err), "write transient heat: %s", err);
    { FDFile *fs = load(dir, "system/fvSolution"); CHECK(fd_find(fs, "solvers/p_rghFinal") && fd_find(fs, "solvers/\"rho.*\""), "transient heat solvers"); fd_free(fs); }
    /* forced convection: gravity written as zero */
    s.flow = FLOW_STEADY; sim_set_energy(&s, EN_FORCED);
    CHECK(sim_write(dir, &s, err, sizeof err), "write forced: %s", err);
    { FDFile *g = load(dir, "constant/g"), *fs = load(dir, "system/fvSolution"); CHECK(is(g, "value", "(0 0 0)") && is(fs, "SIMPLE/momentumPredictor", "yes"), "forced: no gravity"); fd_free(g); fd_free(fs); }
    /* the numbers: Ra over 0.1 m of air with 10 K across, about 1e6; Ri at 10 m/s, tiny */
    sim_set_energy(&s, EN_BUOYANT);
    s.b[2].heat = HT_FIXED; s.b[2].T = s.T0 + 10; s.b[0].T = s.T0;
    double Ra = sim_rayleigh(&s, 0.1);
    CHECK(Ra > 8e5 && Ra < 1.3e6 && sim_delta_T(&s) == 10, "Rayleigh %g", Ra);
    CHECK(sim_richardson(&s, 0.1) > 0 && sim_richardson(&s, 0.1) < 0.01, "Richardson %g", sim_richardson(&s, 0.1));
    /* water: Boussinesq about its density */
    sim_fluid(&s, FLUID_WATER);
    CHECK(sim_write(dir, &s, err, sizeof err), "write water: %s", err);
    { FDFile *th = load(dir, "constant/thermophysicalProperties");
      CHECK(is(th, "thermoType/equationOfState", "Boussinesq") && is(th, "mixture/equationOfState/rho0", "998.2") && is(th, "mixture/equationOfState/beta", "0.000207"), "water Boussinesq");
      fd_free(th); }
    sim_fluid(&s, FLUID_AIR);
    /* heat off again: our heat files go */
    sim_set_energy(&s, EN_OFF);
    CHECK(sim_write(dir, &s, err, sizeof err), "write heat off: %s", err);
    {
        static const char *const gone[] = { "0/T", "0/p_rgh", "0/alphat", "constant/thermophysicalProperties", "constant/g" };
        for (int i = 0; i < 5; i++) { char gp[PL_PATH]; pl_join(gp, sizeof gp, dir, gone[i]); CHECK(!pl_exists(gp), "%s removed with heat off", gone[i]); }
        FDFile *c = load(dir, "system/controlDict"); CHECK(is(c, "application", "simpleFoam") && !fd_find(c, "functions/wallHeatFlux"), "back to simpleFoam"); fd_free(c);
    }
    /* checks: nothing heats, no gravity */
    setup(&s); sim_set_energy(&s, EN_BUOYANT); s.g[0] = s.g[1] = s.g[2] = 0;
    CHECK(sim_check(&s, msg, sizeof msg) >= 2 && strstr(msg, "Nothing heats") && strstr(msg, "no gravity"), "heat checks: %s", msg);
    /* a closed box, still fluid: nothing moves it until buoyancy and a heated wall do (natural convection) */
    {
        SimPatch bx[] = { { "hot", ROLE_WALL, "wall", false, 0 }, { "cold", ROLE_WALL, "wall", false, 0 } };
        sim_defaults(&s, 0.1); sim_set_boundaries(&s, bx, 2);
        sim_check(&s, msg, sizeof msg);
        CHECK(strstr(msg, "Nothing moves"), "closed box, no heat: nothing moves: %s", msg);
        sim_set_energy(&s, EN_BUOYANT); s.b[0].heat = HT_FIXED; s.b[0].T = s.T0 + 1;
        sim_check(&s, msg, sizeof msg);
        CHECK(!strstr(msg, "Nothing moves"), "heated wall with buoyancy moves the fluid: %s", msg);
    }
    /* a hand edit equal to what the setup now writes is no conflict (the Run page's end time, taken in) */
    setup(&s);
    CHECK(sim_write(dir, &s, err, sizeof err), "write: %s", err);
    cdf = load(dir, "system/controlDict"); fd_put(cdf, "endTime", "40"); CHECK(fd_save(cdf), "saved"); fd_free(cdf);
    s.iterations = 40; s.turb = TURB_KEPSILON;
    CHECK(sim_write(dir, &s, err, sizeof err) && !sim_write_replaced()[0], "agreeing edit not listed as replaced: %s", sim_write_replaced());

    /* the mesh signature: an inlet made an outlet is the same mesh; made a wall, or renamed, it isn't */
    {
        MeshCfg m; memset(&m, 0, sizeof m);
        m.scale = 1; m.base = 0.01; m.nNames = 2;
        strcpy(m.names[0], "in"); m.types[0] = BT_INLET; strcpy(m.names[1], "body"); m.types[1] = BT_WALL;
        Str a, b; str_init(&a); str_init(&b);
        mesher_signature(&m, &a);
        MeshCfg m2 = m; m2.types[0] = BT_OUTLET; mesher_signature(&m2, &b);
        CHECK(!strcmp(a.s, b.s), "inlet to outlet: same mesh");
        b.len = 0; b.s[0] = 0; m2.types[0] = BT_WALL; mesher_signature(&m2, &b);
        CHECK(strcmp(a.s, b.s) != 0, "inlet to wall: another mesh");
        b.len = 0; b.s[0] = 0; m2 = m; strcpy(m2.names[0], "front"); mesher_signature(&m2, &b);
        CHECK(strcmp(a.s, b.s) != 0, "renamed: another mesh");
        char sp[PL_PATH]; pl_join(sp, sizeof sp, dir, "mesher");
        pl_mkdirs(sp);
        CHECK(mesher_mesh_current(dir, &m) == -1, "no signature yet");
        pl_join(sp, sizeof sp, dir, "mesher/mesh.sig");
        pl_write_file(sp, a.s, a.len);
        CHECK(mesher_mesh_current(dir, &m2) == 0 && mesher_mesh_current(dir, &m) == 1, "current against mesh.sig");
        str_free(&a); str_free(&b);
    }

    /* the wind tunnel along any axis: upstream on the side the flow comes from, the top on the up side, the ground below */
    {
        Stl st; memset(&st, 0, sizeof st);
        st.nTri = 1; st.bmin[0] = 0; st.bmin[1] = 0; st.bmin[2] = 0; st.bmax[0] = 0.01f; st.bmax[1] = 0.1f; st.bmax[2] = 1;
        MeshCfg m; memset(&m, 0, sizeof m);
        m.domain = DOMAIN_EXTERNAL; m.base = 0.01; m.up = 3; m.down = 8; m.side = 2; m.top = 4; m.below = 2;
        m.flowDir = DIR_NY; m.upDir = DIR_PZ;
        double lo[3], hi[3];
        mesher_domain(&m, &st, lo, hi);
        CHECK(fabs(mesher_ref_length(&m, &st) - 0.1) < 1e-6, "L: the length along the flow (the chord)");
        CHECK(fabs(hi[1] - (0.1 + 0.3)) < 1e-6 && fabs(lo[1] - (0 - 0.8)) < 1e-6, "-y: upstream at +y, the wake toward -y: %g %g", lo[1], hi[1]);
        CHECK(fabs(lo[0] + 0.2) < 1e-6 && fabs(hi[0] - 0.21) < 1e-6, "sides across: %g %g", lo[0], hi[0]);
        CHECK(fabs(hi[2] - 1.4) < 1e-6 && fabs(lo[2] + 0.2) < 1e-6, "top above, the sides' room below: %g %g", lo[2], hi[2]);
        m.below = 1; mesher_domain(&m, &st, lo, hi);
        CHECK(fabs(lo[2] + 0.1) < 1e-6, "below has its own distance: %g", lo[2]);
        /* 2-D across x: the slab is one base cell thick (a cube, so the layers hold), centred on the extrusion */
        m.twoD = true; m.normal2D = 0; mesher_domain(&m, &st, lo, hi);
        CHECK(fabs(lo[0]) < 1e-6 && fabs(hi[0] - 0.01) < 1e-6, "2-D slab: %g %g", lo[0], hi[0]);
        CHECK(stl_thin_axis(&st) == 0, "thin axis x");
        m.twoD = false;
        m.ground = true; m.flowDir = DIR_PZ; m.upDir = DIR_PZ;            /* up can't be the flow: y instead */
        CHECK(mesher_up(&m) == DIR_PY, "up made square to a flow along z");
        m.refLen = 0.5;
        mesher_domain(&m, &st, lo, hi);
        CHECK(fabs(lo[2] + 1.5) < 1e-6 && fabs(hi[2] - 5) < 1e-6 && fabs(lo[1]) < 1e-6 && fabs(hi[1] - 2.1) < 1e-6,
              "+z with a reference length, on the ground (y): z %g %g, y %g %g", lo[2], hi[2], lo[1], hi[1]);
    }

    printf("%s (%d failure%s)\n", fails ? "FAILED" : "ok", fails, fails == 1 ? "" : "s");

    /* a compressible gas (plan step 4): rhoSimpleFoam's files, absolute p, the isentropic inlets, Mach, the round trip */
    {
        char gerr[300], gmsg[2000];
        setup(&s);
        sim_set_comp(&s, COMP_GAS);
        CHECK(s.energy == EN_FORCED && !strcmp(sim_app(&s), "rhoSimpleFoam"), "gas: rhoSimpleFoam, energy on: %s", sim_app(&s));
        CHECK(fabs(s.rho - 101325 / (8314.462618 / 28.96 * 293.15)) < 1e-3, "gas: air's density at 1 atm, 20 C: %g", s.rho);
        CHECK(fabs(sim_gas_mu(&s, 293.15) - 1.813e-5) < 2e-8, "gas: Sutherland's air viscosity: %g", sim_gas_mu(&s, 293.15));
        CHECK(fabs(sim_sound_speed(&s, 293.15) - 343.2) < 0.5, "gas: speed of sound: %g", sim_sound_speed(&s, 293.15));
        CHECK(sim_energy_why_not(&s, EN_OFF) && sim_energy_why_not(&s, EN_BUOYANT) && !sim_energy_why_not(&s, EN_FORCED), "gas: energy choices");
        s.flow = FLOW_TRANSIENT; sim_set_algo(&s, ALG_PISO);
        CHECK(s.algo == ALG_PIMPLE && !strcmp(sim_app(&s), "rhoPimpleFoam"), "gas: PISO becomes PIMPLE (%d)", s.algo);
        CHECK(sim_algo_why_not(&s, ALG_PISO) != NULL, "gas: PISO greyed");
        s.flow = FLOW_STEADY; sim_settle(&s);
        /* Mach: 10 m/s is 0.03; by Mach 0.9 it is transonic; a total pressure of 1.5 atm over 1 atm out is Mach 0.78 */
        CHECK(fabs(sim_mach(&s) - 10 / 343.2) < 1e-3 && !sim_transonic(&s), "gas: Mach at 10 m/s: %g", sim_mach(&s));
        s.b[0].inlet = IN_MACH; s.b[0].mach = 0.9; s.b[0].T = 293.15;
        CHECK(fabs(sim_mach(&s) - 0.9) < 1e-9 && sim_transonic(&s) && fabs(sim_ref_speed(&s) - 0.9 * 343.2) < 0.5, "gas: Mach inlet: %g", sim_ref_speed(&s));
        s.b[0].inlet = IN_PRESSURE; s.b[0].p = 0.5 * 101325;
        CHECK(fabs(sim_mach(&s) - 0.7837) < 1e-3, "gas: isentropic Mach of a 1.5 pressure ratio: %g", sim_mach(&s));
        CHECK(sim_write(dir, &s, gerr, sizeof gerr), "gas write: %s", gerr);
        FDFile *f = load(dir, "constant/thermophysicalProperties");
        CHECK(is(f, "thermoType/type", "hePsiThermo") && is(f, "thermoType/transport", "sutherland") && is(f, "thermoType/energy", "sensibleInternalEnergy")
              && is(f, "mixture/transport/Ts", "110.4"), "gas thermo");
        fd_free(f);
        f = load(dir, "0/p");
        CHECK(is(f, "internalField", "uniform 101325") && is(f, "boundaryField/inlet/type", "totalPressure") && is(f, "boundaryField/inlet/gamma", "1.4")
              && is(f, "boundaryField/outlet/value", "uniform 101325") && is(f, "dimensions", "[1 -1 -2 0 0 0 0]"), "gas p absolute, total-pressure inlet");
        fd_free(f);
        f = load(dir, "0/T"); CHECK(is(f, "boundaryField/inlet/type", "totalTemperature"), "gas: total temperature at a total-pressure inlet"); fd_free(f);
        f = load(dir, "system/fvSchemes"); CHECK(fd_find(f, "divSchemes/div(phi,e)") && fd_find(f, "divSchemes/div(phi,Ekp)") && fd_find(f, "gradSchemes/limited"), "gas schemes"); fd_free(f);
        f = load(dir, "system/fvSolution");
        CHECK(is(f, "SIMPLE/transonic", "no") && is(f, "SIMPLE/pMinFactor", "0.1") && is(f, "relaxationFactors/fields/rho", "0.01"), "gas SIMPLE");
        fd_free(f);
        f = load(dir, "system/controlDict"); CHECK(is(f, "application", "rhoSimpleFoam") && fd_find(f, "functions/MachNo"), "gas controlDict: MachNo"); fd_free(f);
        f = load(dir, "system/fvOptions"); CHECK(fd_find(f, "limitT") && fd_find(f, "limitU"), "gas start-up guards"); fd_free(f);
        char gp[PL_PATH];
        pl_join(gp, sizeof gp, dir, "constant/transportProperties"); CHECK(!pl_exists(gp), "gas: no transportProperties");
        pl_join(gp, sizeof gp, dir, "0/p_rgh"); CHECK(!pl_exists(gp), "gas: no p_rgh");
        /* transient: a non-reflecting outlet, the density bounded */
        s.flow = FLOW_TRANSIENT; sim_settle(&s); s.b[1].nonRefl = true;
        s.b[0].inlet = IN_MASS; s.b[0].mdot = 0.1;
        CHECK(sim_write(dir, &s, gerr, sizeof gerr), "gas transient write: %s", gerr);
        f = load(dir, "0/p"); CHECK(is(f, "boundaryField/outlet/type", "waveTransmissive") && is(f, "boundaryField/inlet/type", "zeroGradient"), "gas: waveTransmissive outlet"); fd_free(f);
        f = load(dir, "system/fvSolution"); CHECK(fd_find(f, "PIMPLE/rhoMin") && fd_find(f, "PIMPLE/rhoMax"), "gas PIMPLE density bounds"); fd_free(f);
        /* the round trip */
        s.gas = GAS_CO2; sim_gas_preset(&s, GAS_CO2); s.pOp = 2e5; s.b[0].mach = 0.4; sim_settle(&s);
        CHECK(sim_save(dir, &s), "gas save");
        sim_load(dir, &r, 0.5);
        CHECK(r.comp == COMP_GAS && r.gas == GAS_CO2 && fabs(r.gamma - 1.289) < 1e-9 && r.pOp == 2e5 && r.b[1].nonRefl && r.b[0].mach == 0.4 && r.energy == EN_FORCED,
              "gas round trip");
        /* the checks: an incompressible air setup at Mach 0.5 is told to be a gas; a supersonic steady gas to go transient */
        setup(&s); s.b[0].speed = 170;
        CHECK(sim_check(&s, gmsg, sizeof gmsg) && strstr(gmsg, "Ideal gas"), "Mach 0.5 incompressible: %s", gmsg);
        sim_set_comp(&s, COMP_GAS); s.b[0].inlet = IN_MACH; s.b[0].mach = 2;
        CHECK(sim_check(&s, gmsg, sizeof gmsg) && strstr(gmsg, "supersonic") && !strstr(gmsg, "Nothing heats"), "supersonic steady: %s", gmsg);
        /* back to incompressible: the gas's files go, and the Mach inlet becomes its speed (bug hunt 2026-11-01) */
        sim_set_comp(&s, COMP_INCOMP);
        CHECK(s.b[0].inlet == IN_SPEED && fabs(s.b[0].speed - 2 * sqrt(1.4 * SIM_R / 28.96 * s.b[0].T)) < 1e-6, "Mach inlet to speed: %d %g", s.b[0].inlet, s.b[0].speed);
        CHECK(s.energy == EN_OFF && s.rho > 1.1 && s.rho < 1.3 && !strcmp(sim_app(&s), "simpleFoam"), "back to incompressible");
        CHECK(sim_write(dir, &s, gerr, sizeof gerr), "write: %s", gerr);
        pl_join(gp, sizeof gp, dir, "system/fvOptions"); CHECK(!pl_exists(gp), "gas fvOptions removed");
        pl_join(gp, sizeof gp, dir, "constant/thermophysicalProperties"); CHECK(!pl_exists(gp), "gas thermo removed");
        /* templates: High-speed gas sets the gas; another one undoes it */
        sim_template_apply(&s, TMPL_GAS, 0.5);
        CHECK(sim_is_gas(&s) && !sim_template_why_not(&s, TMPL_GAS, false), "gas template");
        sim_template_apply(&s, TMPL_HEAT, 0.5);
        CHECK(!sim_is_gas(&s) && s.energy == EN_FORCED, "heat template after gas: %d %d", s.comp, s.energy);
        /* the free stream far field, incompressible: freestreamVelocity and freestreamPressure */
        setup(&s); s.b[3].role = ROLE_OPENING; s.b[3].farField = true; s.b[3].vec[0] = 30;
        CHECK(sim_write(dir, &s, gerr, sizeof gerr), "far field write: %s", gerr);
        f = load(dir, "0/U"); CHECK(is(f, "boundaryField/top/type", "freestreamVelocity"), "far field U"); fd_free(f);
        f = load(dir, "0/p"); CHECK(is(f, "boundaryField/top/type", "freestreamPressure"), "far field p"); fd_free(f);
        CHECK(sim_ref_speed(&s) == 30, "far field speed counts: %g", sim_ref_speed(&s));
    }


    /* turbulence and walls (plan step 5): the treatment, the y+ helper, inlet turbulence, yPlus and fieldAverage */
    {
        char werr[300], wmsg[2000];
        setup(&s);
        CHECK(sim_wall_treatment(&s) == WT_FUNCTIONS && sim_yplus_target(&s) == 50, "SST: wall functions by default");
        CHECK(sim_write(dir, &s, werr, sizeof werr), "write: %s", werr);
        FDFile *f = load(dir, "0/nut"); CHECK(is(f, "boundaryField/walls/type", "nutkWallFunction"), "wall functions: nutk"); fd_free(f);
        f = load(dir, "system/controlDict"); CHECK(fd_find(f, "functions/yPlus") && !fd_find(f, "functions/fieldAverage"), "yPlus on a RANS run, no averages"); fd_free(f);
        s.wallTreat = WT_RESOLVED;
        CHECK(sim_write(dir, &s, werr, sizeof werr), "write: %s", werr);
        f = load(dir, "0/nut"); CHECK(is(f, "boundaryField/walls/type", "nutUSpaldingWallFunction"), "resolved: Spalding"); fd_free(f);
        f = load(dir, "0/omega"); CHECK(is(f, "boundaryField/walls/blending", "binomial"), "resolved: omega blended"); fd_free(f);
        s.turb = TURB_KEPSILON; sim_settle(&s);
        CHECK(sim_wall_treatment(&s) == WT_FUNCTIONS && sim_wall_why_not(&s, WT_RESOLVED), "k-epsilon keeps wall functions");
        CHECK(sim_check(&s, wmsg, sizeof wmsg) && strstr(wmsg, "it runs with wall functions"), "k-epsilon resolved check: %s", wmsg);
        s.turb = TURB_WALE; s.flow = FLOW_TRANSIENT; s.wallTreat = WT_AUTO; sim_settle(&s);
        CHECK(sim_wall_treatment(&s) == WT_RESOLVED, "LES: resolved by default");
        s.endTime = 2; s.avgStart = -1;
        CHECK(fabs(sim_avg_start(&s) - 0.8) < 1e-12, "LES averaging: 40 %% of the end: %g", sim_avg_start(&s));
        CHECK(sim_write(dir, &s, werr, sizeof werr), "write: %s", werr);
        f = load(dir, "system/controlDict"); CHECK(fd_find(f, "functions/fieldAverage") && is(f, "functions/fieldAverage/timeStart", "0.8"), "LES fieldAverage from 0.8 s"); fd_free(f);
        s.avgStart = 3;
        CHECK(sim_check(&s, wmsg, sizeof wmsg) && strstr(wmsg, "no UMean"), "averaging after the end: %s", wmsg);
        s.maxCo = 0.8; s.adjustDt = true; s.algo = ALG_PIMPLE;
        CHECK(sim_check(&s, wmsg, sizeof wmsg) && strstr(wmsg, "0.5 or under"), "LES Courant 0.5: %s", wmsg);
        /* the helper: air at 10 m/s over 1 m, Re 6.6e5: Cf 0.026 Re^-1/7, the first cell for y+ 1 and its inverse */
        setup(&s);
        double h1, d, ut, Re = sim_wall_spacing(&s, 10, 1, 1, &h1, &d, &ut);
        double Cf = 0.026 * pow(Re, -1.0 / 7), ut0 = 10 * sqrt(0.5 * Cf);
        CHECK(fabs(Re - 10 / s.nu) < 1 && fabs(ut - ut0) < 1e-9 && fabs(h1 - 2 * s.nu / ut0) < 1e-12, "y+ helper: Re %g utau %g h %g", Re, ut, h1);
        CHECK(fabs(sim_yplus_of(&s, 10, 1, h1) - 1) < 1e-9 && fabs(d - 0.37 * pow(Re, -0.2)) < 1e-9, "y+ inverse and delta: %g", d);
        double tot; int nl = sim_layer_count(1e-5, 1e-3, 1.2, &tot);
        CHECK(nl == 1 + (int)ceil(log(100) / log(1.2)) && fabs(tot - 1e-5 * (pow(1.2, nl) - 1) / 0.2) < 1e-12, "layers: %d", nl);
        /* inlet turbulence by viscosity ratio and by diameter */
        SimBoundary *in = &s.b[0];
        in->turbSpec = TI_RATIO; in->ratio = 10; in->intensity = 5;
        double I, Lm, k, om;
        sim_inlet_turbulence(&s, in, 10, &I, &Lm);
        sim_turbulence(&s, 10, I, Lm, &k, &om, NULL, NULL);
        CHECK(fabs(k / om / s.nu - 10) < 1e-6, "viscosity ratio 10: nut/nu %g", k / om / s.nu);
        in->turbSpec = TI_DIAMETER; in->dh = 0.1;
        sim_inlet_turbulence(&s, in, 10, &I, &Lm);
        CHECK(fabs(I - 16 * pow(10 * 0.1 / s.nu, -0.125)) < 1e-9 && fabs(Lm - 0.007) < 1e-12, "hydraulic diameter: I %g L %g", I, Lm);
        in->dh = 0; in->area = M_PI * 0.05 * 0.05;
        sim_inlet_turbulence(&s, in, 10, &I, &Lm);
        CHECK(fabs(Lm - 0.007) < 1e-9, "diameter from the area: %g", Lm);
        CHECK(sim_write(dir, &s, werr, sizeof werr), "write: %s", werr);
        f = load(dir, "0/omega"); CHECK(is(f, "boundaryField/inlet/mixingLength", "0.007"), "diameter's mixing length written"); fd_free(f);
        /* the round trip */
        s.wallTreat = WT_RESOLVED; s.avgStart = 1.5; CHECK(sim_save(dir, &s), "save");
        sim_load(dir, &r, 0.5);
        CHECK(r.wallTreat == WT_RESOLVED && r.avgStart == 1.5 && r.b[0].turbSpec == TI_DIAMETER && r.b[0].ratio == 10, "step 5 round trip");
        s.avgStart = -1; sim_save(dir, &s); sim_load(dir, &r, 0.5);
        CHECK(r.avgStart < 0, "automatic averaging start kept");
    }

    return fails ? 1 : 0;
}
