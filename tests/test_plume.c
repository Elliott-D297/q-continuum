/* test_plume.c -- ROADMAP 4.4: the impinging-plume starter's numbers against hand-worked values, and (with a directory)
   writing a case there: test_plume [dir] */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "plume.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)
#define NEAR(a, b, tol) (fabs((a) - (b)) <= (tol))

int main(int argc, char **argv)
{
    PlumeIn in; plume_defaults(&in);
    PlumeOut o; char err[300];
    CHECK(plume_compute(&in, &o, err, sizeof err), "defaults: %s", err);
    /* N2 at 300 K: R = 8314.46 / 28.0134 = 296.80; a = sqrt(1.4 x 296.80 x 300) = 353.07 m/s; Ue = 3 a = 1059.2 m/s */
    CHECK(NEAR(o.Rs, 296.80, 0.01), "R %g", o.Rs);
    CHECK(NEAR(o.ae, 353.07, 0.05), "a %g", o.ae);
    CHECK(NEAR(o.Ue, 1059.2, 0.2), "Ue %g", o.Ue);
    /* p0 = 5000 (1 + 0.2*9)^3.5 = 5000 * 2.8^3.5 = 183 683 Pa; NPR 1836.8 */
    CHECK(NEAR(o.p0, 183683, 50), "p0 %g", o.p0);
    CHECK(NEAR(o.npr, 1836.8, 0.5), "NPR %g", o.npr);
    /* Mach disk 0.67 * 0.02 * sqrt(1836.8) = 0.5743 m: beyond the 0.05 m plate */
    CHECK(NEAR(o.xMach, 0.5743, 0.001), "x Mach %g", o.xMach);
    /* dx = 0.01 / 10 = 1 mm: 50 cells to the plate, 10 across the exit, 0.09 / 2.5 mm = 36 beyond: 50 x 46 = 2300 */
    CHECK(o.nx == 50 && o.nr1 == 10 && o.nr2 == 36 && o.cells == 2300, "mesh %d %d %d %ld", o.nx, o.nr1, o.nr2, o.cells);
    /* endTime 2 (0.05 + 0.1) / 1059.2 = 2.832e-4 s */
    CHECK(NEAR(o.endTime, 2.832e-4, 1e-7), "endTime %g", o.endTime);
    in.Me = 0.5; CHECK(!plume_compute(&in, &o, err, sizeof err), "subsonic exit refused");
    plume_defaults(&in); in.pa = 0; CHECK(!plume_compute(&in, &o, err, sizeof err), "vacuum refused");
    plume_defaults(&in); in.pe = 50; CHECK(plume_compute(&in, &o, err, sizeof err) && strstr(o.warn, "over-expanded"), "over-expanded warned");
    if (argc > 1) {
        plume_defaults(&in);
        CHECK(plume_write(argv[1], &in, err, sizeof err), "write: %s", err);
    }
    printf(fails ? "FAILED: %d failures\n" : "passed\n", fails);
    return fails != 0;
}
