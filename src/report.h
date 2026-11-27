/* report.h -- a case's results written up (improvement plan A3, 2026-11-26): <case>/report/report.md with the solver, mesh,
 * last run, final residuals, every monitor's last values, what the starter promised, the Visualization screenshots and the
 * run history; residuals and monitors as SVG charts beside it (plain text, shown by GitHub, VS Code and browsers). With
 * pandoc on the PATH a report.html is written too. Also used for a comparison of cases (Studies). */
#ifndef REPORT_H
#define REPORT_H
#include <stdbool.h>

/* one chart: up to 12 series of (x, y); logY for residuals. False when nothing could be written */
#define RPT_MAX_SERIES 12
typedef struct { const char *name; const float *x, *y; int n; } RptSeries;
bool rpt_svg(const char *path, const char *title, const char *xLabel, const char *yLabel, const RptSeries *s, int ns, bool logY);
/* the case's report; out: the report.md written. False with the reason in err */
bool rpt_case(const char *casePath, char *out, int ocap, char *err, int ecap);
/* a comparison: one table row per case (name, then each value), a chart of the series given (may be 0); written to
   <dir>/<name>-report.md */
typedef struct { const char *caseName, *casePath; double val[8]; } RptRow;
bool rpt_compare(const char *dir, const char *name, const char *const *colNames, int nCols, const RptRow *rows, int nRows,
                 const char *chartTitle, const RptSeries *s, int ns, char *out, int ocap, char *err, int ecap);
#endif
