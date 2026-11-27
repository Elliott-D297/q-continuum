/* test_report.c -- reports (improvement plan A3): a cavity case made from the real icoFoam log in tests/logs and a probes
 * file; the report has the case, the convergence chart, the monitor's last values and the starter's promise; an SVG is
 * well formed; a comparison of two cases writes its table. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "report.h"
#include "pl_util.h"

static int fails;
#define CHECK(c, msg) do { if (c) printf("ok    %s\n", msg); else { printf("FAIL  %s\n", msg); fails++; } } while (0)

static void put(const char *dir, const char *rel, const char *t)
{
    char p[PL_PATH]; pl_join(p, sizeof p, dir, rel);
    char d[PL_PATH]; pl_strlcpy(d, p, sizeof d); *strrchr(d, '/') = 0; pl_mkdirs(d);
    pl_write_file(p, t, strlen(t));
}
static bool has(const char *path, const char *needle)
{
    char *t = pl_read_file(path, NULL); bool ok = t && strstr(t, needle); free(t); return ok;
}

int main(int argc, char **argv)
{
    if (argc < 3) { printf("usage: test_report <tests/logs> <scratch>\n"); return 2; }
    char c[PL_PATH], cmd[PL_PATH * 5]; pl_join(c, sizeof c, argv[2], "report_case");
    snprintf(cmd, sizeof cmd, "rm -rf '%s' && mkdir -p '%s/system' && cp '%s/icoFoam_cavity.log' '%s/log.icoFoam'", c, c, argv[1], c);
    if (system(cmd)) { printf("FAIL  setup\n"); return 1; }
    put(c, "system/controlDict", "FoamFile { class dictionary; object controlDict; }\napplication icoFoam;\nstartTime 0;\nendTime 0.5;\ndeltaT 0.005;\n");
    put(c, "postProcessing/probes/0/p", "# Probe 0 (0.05 0.05 0.005)\n# Time p\n0.1 1.5\n0.2 1.25\n0.3 1.125\n");
    put(c, ".qc-starter", "title = Lid-driven cavity\nexpect = one vortex, a little towards the moving lid\n");
    char md[PL_PATH], err[300];
    CHECK(rpt_case(c, md, sizeof md, err, sizeof err), "case report written");
    CHECK(has(md, "# report_case") && has(md, "| Solver | `icoFoam` |"), "the case");
    CHECK(has(md, "**Starter: Lid-driven cavity.**"), "what the starter promised");
    CHECK(has(md, "![Initial residuals](residuals.svg)"), "convergence chart linked");
    char svg[PL_PATH]; pl_join(svg, sizeof svg, c, "report/residuals.svg");
    CHECK(has(svg, "<svg ") && has(svg, "</svg>") && has(svg, "<polyline") && has(svg, ">Ux<"), "residuals.svg drawn, Ux in the legend");
    CHECK(has(md, "| 1.125 |") || has(md, "| 1.125"), "the probe's last value");
    RptRow rows[2] = { { "a", "/x/a", { 1.5, 2 } }, { "b", "/x/b", { 2.5, 0.0 / 0.0 } } };
    const char *cols[2] = { "drag", "lift" };
    float t[3] = { 0, 1, 2 }, y1[3] = { 3, 2, 1 }, y2[3] = { 1, 2, 3 };
    RptSeries s[2] = { { "a", t, y1, 3 }, { "b", t, y2, 3 } };
    CHECK(rpt_compare(argv[2], "Two designs", cols, 2, rows, 2, "drag", s, 2, md, sizeof md, err, sizeof err), "comparison written");
    CHECK(has(md, "| a | 1.5 | 2 |") && has(md, "| b | 2.5 | - |") && has(md, "Two_designs-report.svg"), "its table and chart");
    printf(fails ? "FAILED (%d)\n" : "all ok\n", fails);
    return fails ? 1 : 0;
}
