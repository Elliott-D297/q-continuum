/* test_runctl.c -- run controls (runctl.c): the solver table, the step for a Courant number, writes by time, and
   rc_to_pimple on a copy of a case: test_runctl <case to copy (icoFoam or pisoFoam)> <scratch dir>. The converted case is
   left at <scratch>/rc_case for a real pimpleFoam run (ctest runctl_pimple). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "runctl.h"
#include "foamdict.h"
#include "foamcase.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static bool is(FDFile *f, const char *key, const char *want)
{
    char v[200] = "";
    return f && fd_get(f, key, v, sizeof v) && !strcmp(v, want);
}

int main(int argc, char **argv)
{
    /* the table */
    CHECK(!(rc_solver("icoFoam")->flags & RC_ADJUST) && rc_solver("icoFoam")->upgrade, "icoFoam: fixed, upgradable");
    CHECK(rc_solver("pimpleFoam")->flags & RC_ADJUST, "pimpleFoam adapts");
    CHECK((rc_solver("interFoam")->flags & (RC_ADJUST | RC_ALPHACO)) == (RC_ADJUST | RC_ALPHACO), "interFoam: maxAlphaCo");
    CHECK(rc_solver("chtMultiRegionFoam")->flags & RC_DI, "cht: maxDi");
    CHECK(rc_solver("simpleFoam")->flags & RC_STEADY, "simpleFoam steady");
    CHECK(!(rc_solver("sonicFoam")->flags & RC_ADJUST), "sonicFoam fixed (v2606)");
    CHECK(!(rc_solver("myPimpleFoam")->flags & RC_KNOWN) && (rc_solver("myPimpleFoam")->flags & RC_ADJUST), "a guess from the name");
    CHECK(!strcmp(rc_alg_dict(rc_solver("potentialFoam")->alg), "potentialFlow"), "potentialFoam loop");
    /* the step for a Courant number: rounded down to two figures */
    CHECK(fabs(rc_dt_for_courant(1e-4, 0.42, 1) - 2.3e-4) < 1e-12, "dt for Co 1: %g", rc_dt_for_courant(1e-4, 0.42, 1));
    CHECK(rc_dt_for_courant(1e-4, 0, 1) == 0, "no Courant seen");
    /* writes by time */
    FDFile *w = fd_from_text("/x/system/controlDict", "deltaT 0.005;\nwriteControl timeStep;\nwriteInterval 20;\n");
    CHECK(rc_write_by_time(w, NULL, 0) && is(w, "writeControl", "adjustable") && is(w, "writeInterval", "0.1"), "timeStep 20 x 0.005 -> 0.1 s");
    CHECK(!rc_write_by_time(w, NULL, 0), "already by time");
    fd_free(w);

    if (argc == 3) {
        char dst[PL_PATH], cmd[PL_PATH * 3];
        pl_join(dst, sizeof dst, argv[2], "rc_case");
        snprintf(cmd, sizeof cmd, "rm -rf '%s' && cp -r '%s' '%s'", dst, argv[1], dst);
        CHECK(system(cmd) == 0, "copy");
        Case *c = case_open(dst);
        CHECK(c != NULL, "open");
        if (c) {
            char msg[600];
            int n = rc_to_pimple(c, 1, msg, sizeof msg);
            printf("rc_to_pimple: %d changes: %s\n", n, msg);
            CHECK(n > 0, "converted");
            CHECK(case_save_all(c) >= 0, "saved");
            CHECK(rc_to_pimple(c, 1, msg, sizeof msg) < 0, "a pimpleFoam case isn't converted again");
            case_close(c);
            char p[PL_PATH];
            pl_join(p, sizeof p, dst, "system/controlDict"); FDFile *cd = fd_load(p);
            CHECK(is(cd, "application", "pimpleFoam") && is(cd, "adjustTimeStep", "yes"), "controlDict");
            fd_free(cd);
            pl_join(p, sizeof p, dst, "system/fvSolution"); FDFile *fs = fd_load(p);
            CHECK(fs && fs->ok && fd_find(fs, "PIMPLE") && !fd_find(fs, "PISO"), "PIMPLE");
            CHECK(is(fs, "PIMPLE/nOuterCorrectors", "1"), "one outer corrector");
            bool fin = fd_find(fs, "solvers/UFinal") || fd_find(fs, "solvers/\"(U|k|epsilon|omega|R|nuTilda)Final\"");
            FDEntry *sol = fd_find(fs, "solvers");
            for (int i = 0; sol && i < sol->nChild; i++) if (strstr(sol->child[i].key, "Final") && strchr(sol->child[i].key, 'U')) fin = true;
            CHECK(fin, "a U Final solver");
            fd_free(fs);
            pl_join(p, sizeof p, dst, "constant/turbulenceProperties");
            CHECK(pl_is_file(p), "turbulenceProperties");
        }
    }
    printf("%s (%d failures)\n", fails ? "FAILED" : "ok", fails);
    return fails ? 1 : 0;
}
