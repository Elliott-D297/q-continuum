/* test_teach.c -- the starters with exact answers (improvement plan A2): test_teach <bar|flow> <case> writes one and prints
 * the answer its probe must give; tests/teach_openfoam.sh runs them in OpenFOAM and compares. test_teach check: the series. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "teachcases.h"

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "check")) {
        /* hand values: Fo 0.1, first term 400 - 100 x 4/pi x e^(-pi^2/10) = 352.55; Pe 10 at x/L 0.9: (e^10 - e^9) / (e^10 - 1) = 0.63215 */
        int f = 0;
        if (fabs(tc_bar_mid(10) - 352.554) > 0.01) { printf("FAIL  bar %.4f\n", tc_bar_mid(10)); f++; }
        if (fabs(tc_flow_value(0.09) - 0.63215) > 1e-4) { printf("FAIL  flow %.5f\n", tc_flow_value(0.09)); f++; }
        if (fabs(tc_bar_mid(1e6) - 400) > 1e-6) { printf("FAIL  bar steady %.3f\n", tc_bar_mid(1e6)); f++; }
        printf(f ? "FAILED\n" : "all ok\n");
        return f;
    }
    if (argc < 3) { printf("usage: test_teach check | bar <case> | flow <case>\n"); return 2; }
    char err[300];
    bool bar = !strcmp(argv[1], "bar");
    if (!(bar ? tc_bar_write(argv[2], err, sizeof err) : tc_flow_write(argv[2], err, sizeof err))) { printf("FAIL  %s\n", err); return 1; }
    printf("%.6f\n", bar ? tc_bar_mid(10) : tc_flow_value(0.09));
    return 0;
}
