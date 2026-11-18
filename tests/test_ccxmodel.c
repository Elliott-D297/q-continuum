/* test_ccxmodel.c -- workflow plan C1/C2: a CalculiX deck read in engineering terms (ccxmodel.c): the beam and slab starters'
 * materials, supports, loads and set sizes; the unit system and its check; the library match; results from a .dat/.frd */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ccxmodel.h"
#include "program.h"
#include "pl_util.h"

static int fails;
#define CHECK(c, msg) do { if (c) printf("ok    %s\n", msg); else { printf("FAIL  %s\n", msg); fails++; } } while (0)

static const ProgTemplate *tmpl(const char *id)
{
    for (int i = 0; i < PROG_N_TEMPLATES; i++) if (PROG_TEMPLATES[i].prog == PROG_CALCULIX && !strcmp(PROG_TEMPLATES[i].id, id)) return &PROG_TEMPLATES[i];
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc < 2) { printf("usage: test_ccxmodel <scratch>\n"); return 2; }
    char d[PL_PATH], err[300]; pl_join(d, sizeof d, argv[1], "ccxmodel");
    pl_remove_tree(d); pl_mkdirs(d);
    char beam[PL_PATH]; pl_join(beam, sizeof beam, d, "beam");
    CHECK(tmpl("beam") && prog_template_write(tmpl("beam"), beam, err, sizeof err), "beam starter written");
    CcxModel m;
    CHECK(ccx_model_load(beam, "beam.inp", &m), "beam read");
    CHECK(m.analysis == CCX_STATIC, "linear static");
    CHECK(m.nMat == 1 && !strcmp(m.mat[0].name, "STEEL") && fabs(m.mat[0].E - 210e9) < 1 && fabs(m.mat[0].nu - 0.3) < 1e-9 && m.mat[0].rho == 7850, "steel: E, nu, density");
    CHECK(m.mat[0].lineE > 0 && m.mat[0].lineRho > 0, "the material's lines are in the input itself");
    CHECK(m.nBc == 1 && !strcmp(m.bc[0].set, "FIXED") && m.bc[0].d0 == 1 && m.bc[0].d1 == 3 && !m.bc[0].hasValue, "support FIXED, x to z");
    CHECK(m.nLoad == 1 && !strcmp(m.load[0].set, "TIP") && m.load[0].dof == 3 && m.load[0].mag == -40, "load TIP, -40 along z");
    CHECK(ccx_set_nodes(&m, "TIP") == 25 && ccx_set_nodes(&m, "FIXED") == 25, "TIP and FIXED: 25 nodes each (the included mesh)");
    CHECK(fabs(m.load[0].mag * ccx_set_nodes(&m, "TIP") + 1000) < 1e-9, "1 kN in total");
    CHECK(m.nNodes == 41 * 5 * 5 && m.nElems == 40 * 4 * 4 && !strcmp(m.elType, "C3D8I"), "mesh: nodes, elements, type");
    CHECK(fabs(ccx_extent(&m) - 1.0) < 1e-9, "1 m long");
    char msg[400];
    CHECK(ccx_units(&m, msg, sizeof msg) == CCX_UNITS_SI && !msg[0], "SI units, consistent");
    CHECK(ccx_lib_match(&m.mat[0], CCX_UNITS_SI) == 0, "matches structural steel");
    /* the same deck with E in MPa and metre coordinates: the check says so */
    const char *mmDeck = "*INCLUDE, INPUT=beam_mesh.inp\n*MATERIAL, NAME=STEEL\n*ELASTIC\n210000., 0.3\n*DENSITY\n7.85e-9\n*STEP\n*STATIC\n*END STEP\n";
    ccx_model_load_text(beam, "beam.inp", mmDeck, &m);
    CHECK(ccx_units(&m, msg, sizeof msg) == CCX_UNITS_MM && strstr(msg, "only 1 units across"), "MPa with a 1-unit model: flagged");
    CHECK(ccx_lib_match(&m.mat[0], CCX_UNITS_MM) == 0, "steel in MPa still matches");
    const char *rhoDeck = "*MATERIAL, NAME=STEEL\n*ELASTIC\n210e9, 0.3\n*DENSITY\n7.85e-9\n";
    ccx_model_load_text(beam, "beam.inp", rhoDeck, &m);
    CHECK(ccx_units(&m, msg, sizeof msg) == CCX_UNITS_SI && strstr(msg, "t/mm³"), "Pa with a t/mm³ density: flagged");
    /* the slab: heat transfer, a temperature held */
    char slab[PL_PATH]; pl_join(slab, sizeof slab, d, "slab");
    CHECK(tmpl("slab") && prog_template_write(tmpl("slab"), slab, err, sizeof err), "slab starter written");
    ccx_model_load(slab, "slab.inp", &m);
    CHECK(m.analysis == CCX_HEAT && m.nBc == 1 && m.bc[0].d0 == 11 && m.bc[0].value == 400, "slab: heat transfer, HOT held at 400 K");
    CHECK(m.nMat == 1 && m.mat[0].hasK && m.mat[0].k == 16.2, "slab: conductivity 16.2");
    /* C2: the beam run in ccx with its reactions asked for; the .frd read back against the hand calculation */
    CcxBeam bm;
    ccx_model_load(beam, "beam.inp", &m);
    CHECK(ccx_beam_check(&m, &bm) && fabs(bm.F - 1000) < 1e-9 && fabs(bm.L - 1) < 1e-9, "the beam is a slender cantilever: 1 kN over 1 m");
    CHECK(fabs(bm.delta - 1.905e-4) < 2e-6 && fabs(bm.sigma - 6.0e6) < 1e3, "hand calculation: 0.1905 mm, 6.0 MPa");
    if (system("command -v ccx > /dev/null 2>&1") == 0) {
        char cmd[PL_PATH * 2];
        snprintf(cmd, sizeof cmd, "cd '%s' && sed -i 's/^U$/U, RF/' beam.inp && ccx beam > log.ccx 2>&1", beam);
        CHECK(system(cmd) == 0, "ccx ran the beam");
        char frd[PL_PATH]; pl_join(frd, sizeof frd, beam, "beam.frd");
        CcxFrd r;
        CHECK(ccx_frd_load(frd, &r) && r.n == 1025 && r.u && r.vm && r.rf, "beam.frd: 1025 nodes, displacements, stresses, reactions");
        printf("      tip %.4g mm, von Mises %.4g MPa, reactions (%.4g %.4g %.4g) N, %d edges\n", r.uMax * 1e3, r.vmMax / 1e6, r.rfSum[0], r.rfSum[1], r.rfSum[2], r.nEdges);
        CHECK(fabs(r.uMax - bm.delta) < 0.05 * bm.delta, "the largest displacement within 5 %% of F L^3 / (3 E I)");
        CHECK(fabs(r.rfSum[2] - 1000) < 1 && fabs(r.rfSum[0]) < 1 && fabs(r.rfSum[1]) < 1, "the reactions balance the 1 kN load");
        CHECK(r.vmMax > 0.8 * bm.sigma && r.vmMax < 2 * bm.sigma, "the greatest stress near M c / I (a root corner's is higher)");
        CHECK(r.nEdges == 640 * 12, "the bricks' edges for the picture");
        ccx_frd_free(&r);
        /* C3: what CalculiX did. Linear static: one increment, one iteration, the solver time */
        char lg[PL_PATH]; pl_join(lg, sizeof lg, beam, "log.ccx");
        CcxRun ru; ccx_run_load(beam, "beam", lg, &ru);
        CHECK(ru.hasSta && ru.nInc == 1 && ru.itersTotal == 1 && ru.cutbacks == 0 && ru.cpu > 0, "linear static: one increment, one iteration, timed");
        /* nonlinear (NLGEOM, first increment 0.2): several increments of two iterations each */
        snprintf(cmd, sizeof cmd, "cd '%s' && sed 's/^\\*STEP$/*STEP, NLGEOM, INC=100/; s/^\\*STATIC$/*STATIC\\n0.2, 1./' beam.inp > nl.inp && ccx nl > log.nl 2>&1", beam);
        CHECK(system(cmd) == 0, "ccx ran the beam with NLGEOM");
        pl_join(lg, sizeof lg, beam, "log.nl");
        ccx_run_load(beam, "nl", lg, &ru);
        printf("      NLGEOM: %d increments, %d iterations, increments %.3g to %.3g\n", ru.nInc, ru.itersTotal, ru.incMin, ru.incMax);
        CHECK(ru.nInc >= 3 && fabs(ru.incFirst - 0.2) < 1e-9 && fabs(ru.stepTime - 1) < 1e-9 && ru.itersTotal > ru.nInc, "NLGEOM: increments from 0.2 to the step's end, more than one iteration each");
        /* frequency: the modes against the cantilever's first bending frequency */
        snprintf(cmd, sizeof cmd, "cd '%s' && sed 's/^\\*STATIC$/*FREQUENCY\\n6/; /^\\*CLOAD/,+1d; /^\\*NODE PRINT/,+1d' beam.inp > freq.inp && ccx freq > log.freq 2>&1", beam);
        CHECK(system(cmd) == 0, "ccx ran the beam's modes");
        ccx_run_load(beam, "freq", NULL, &ru);
        CcxModel fm; ccx_model_load(beam, "freq.inp", &fm);
        double f1 = ccx_beam_f1(&fm);
        printf("      modes: %d, first %.4g Hz, Euler-Bernoulli %.4g Hz\n", ru.nModes, ru.nModes ? ru.hz[0] : 0, f1);
        CHECK(fm.analysis == CCX_FREQUENCY && ru.nModes == 6 && fabs(ru.hz[0] - ru.hz[1]) < 1e-6 * ru.hz[0], "six modes; the square section's first pair equal");
        CHECK(fabs(f1 - 83.5) < 0.2 && fabs(ru.hz[0] - f1) < 0.02 * f1, "first mode within 2 %% of 1.875^2/(2 pi) sqrt(EI/(rho A L^4)) = 83.5 Hz");
        /* C4: plain C3D8 bricks lock in bending, the more the fewer through the thickness. The starter's beam, 4 through: 3.5 %
           too stiff, no warning. One through (40 x 1 x 1): flagged, and well under the hand calculation */
        CcxMesh g; ccx_mesh_load(beam, "beam.inp", &m, &g);
        CHECK(ccx_locking_risk(&m, &g) == 0, "C3D8I: no locking warning");
        ccx_mesh_free(&g);
        CHECK(ccx_set_element_type(beam, "beam.inp", "C3D8I", "C3D8") == 1, "the included mesh's *ELEMENT changed to C3D8");
        ccx_model_load(beam, "beam.inp", &m); ccx_mesh_load(beam, "beam.inp", &m, &g);
        CHECK(!strcmp(m.elType, "C3D8") && ccx_locking_risk(&m, &g) == 0, "C3D8, 4 bricks through: no warning");
        ccx_mesh_free(&g);
        CHECK(ccx_set_element_type(beam, "beam.inp", "C3D8", "C3D8I") == 1, "and back to C3D8I");
        {
            char thin[PL_PATH]; pl_join(thin, sizeof thin, d, "thin"); pl_mkdirs(thin);
            char pth[PL_PATH]; pl_join(pth, sizeof pth, thin, "thin.inp");
            FILE *fp = fopen(pth, "w");
            fprintf(fp, "*NODE, NSET=NALL\n");
            for (int k = 0; k <= 1; k++) for (int j = 0; j <= 1; j++) for (int i = 0; i <= 10; i++) fprintf(fp, "%d, %g, %g, %g\n", 1 + i + 11 * (j + 2 * k), 0.1 * i, 0.1 * j, 0.1 * k);
            fprintf(fp, "*ELEMENT, TYPE=C3D8, ELSET=EALL\n");
            for (int i = 0; i < 10; i++) { int a0 = 1 + i; fprintf(fp, "%d, %d, %d, %d, %d, %d, %d, %d, %d\n", i + 1, a0, a0 + 1, a0 + 12, a0 + 11, a0 + 22, a0 + 23, a0 + 34, a0 + 33); }
            fprintf(fp, "*NSET, NSET=FIXED\n1, 12, 23, 34\n*NSET, NSET=TIP\n11, 22, 33, 44\n");
            fprintf(fp, "*MATERIAL, NAME=STEEL\n*ELASTIC\n210000e6, 0.3\n*SOLID SECTION, ELSET=EALL, MATERIAL=STEEL\n*STEP\n*STATIC\n*BOUNDARY\nFIXED, 1, 3\n*CLOAD\nTIP, 3, -250.\n*NODE FILE\nU\n*END STEP\n");
            fclose(fp);
            CcxModel tm; ccx_model_load(thin, "thin.inp", &tm); ccx_mesh_load(thin, "thin.inp", &tm, &g);
            CHECK(ccx_locking_risk(&tm, &g) == 1, "C3D8, one brick through a slender beam: flagged (1 layer)");
            ccx_mesh_free(&g);
            snprintf(cmd, sizeof cmd, "cd '%s' && ccx thin > log 2>&1", thin);
            CHECK(system(cmd) == 0, "ccx ran the one-brick beam");
            pl_join(pth, sizeof pth, thin, "thin.frd");
            CcxFrd r8;
            CHECK(ccx_frd_load(pth, &r8), "its .frd read");
            printf("      C3D8, one through: tip %.4g mm, hand %.4g mm\n", r8.uMax * 1e3, bm.delta * 1e3);
            CHECK(r8.uMax < 0.7 * bm.delta, "locked: about a third too stiff");
            ccx_frd_free(&r8);
        }
        ccx_model_load(beam, "beam.inp", &m);
    } else printf("skip  no ccx: the .frd checks\n");
    /* C4: the new starters against the answers they promise (the .qc-starter check) */
    if (system("command -v ccx > /dev/null 2>&1") == 0) {
        static const struct { const char *id; const char *kind; double want, tol; } ST[] = {
            { "modes", "f1", 83.54, 0.02 }, { "bar", "umax", 1.2e-3, 0.02 }, { "plate", "vmmax", 151.5e6, 0.06 }, { "vessel", "vmmax", 20.5e6, 0.03 },
        };
        for (int i = 0; i < 4; i++) {
            char sd[PL_PATH]; pl_join(sd, sizeof sd, d, ST[i].id);
            CHECK(tmpl(ST[i].id) && prog_template_write(tmpl(ST[i].id), sd, err, sizeof err), "starter written");
            char sp[PL_PATH]; pl_join(sp, sizeof sp, sd, ".qc-starter");
            char *note = pl_read_file(sp, NULL);
            char want[64]; snprintf(want, sizeof want, "check = %s ", ST[i].kind);
            CHECK(note && strstr(note, want) && strstr(note, "learn = "), ".qc-starter: what it teaches and the check");
            free(note);
            char cmd[PL_PATH * 2]; snprintf(cmd, sizeof cmd, "cd '%s' && ccx %s > log 2>&1", sd, ST[i].id);
            CHECK(system(cmd) == 0, "ccx ran it");
            double got = 0;
            if (!strcmp(ST[i].kind, "f1")) { CcxRun ru; ccx_run_load(sd, ST[i].id, NULL, &ru); got = ru.nModes ? ru.hz[0] : 0; }
            else {
                char f[PL_PATH], fn[64]; snprintf(fn, sizeof fn, "%s.frd", ST[i].id); pl_join(f, sizeof f, sd, fn);
                CcxFrd r; if (ccx_frd_load(f, &r)) { got = !strcmp(ST[i].kind, "umax") ? r.uMax : r.vmMax; ccx_frd_free(&r); }
            }
            printf("      %-6s %s: %.4g against %.4g (%+.1f %%)\n", ST[i].id, ST[i].kind, got, ST[i].want, (got - ST[i].want) / ST[i].want * 100);
            CHECK(fabs(got - ST[i].want) < ST[i].tol * ST[i].want, "within its tolerance of the hand answer");
        }
    }
    {   /* C4: the wireframe */
        CcxMesh g;
        CHECK(ccx_mesh_load(beam, "beam.inp", &m, &g) && g.n == 1025 && g.nElems == 640, "the beam's mesh: 1025 nodes, 640 bricks");
        int held = 0, loaded = 0, all3 = 0; for (int i = 0; i < g.n; i++) { held += (g.role[i] & 7) != 0; all3 += (g.role[i] & 7) == 7; loaded += (g.role[i] & 16) != 0; }
        CHECK(g.nEdges == 1000 + 820 + 820, "each edge once: 2640");
        CHECK(held == 25 && all3 == 25 && loaded == 25, "25 nodes held in x, y and z (FIXED), 25 loaded (TIP)");
        ccx_mesh_free(&g);
    }
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
