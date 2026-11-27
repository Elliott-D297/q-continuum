/* page_run_int.h -- what page_run.c and page_runchart.c share (improvement plan A5): not for other pages */
#ifndef PAGE_RUN_INT_H
#define PAGE_RUN_INT_H
#include "app.h"
#include "monitor.h"
void     start_run(void);                                      /* page_run.c: the Run button */
void     time_ruler(Rectangle r, double t0, double t1, double t, Color fill);
void     chart_area(Rectangle r, Case *c, LogMon *pm);         /* the switch, readouts and residual or monitor chart */
MonFile *mon_current(Case *c);                                 /* the monitor file the Monitors tab shows */
void     mon_select(Case *c, MonFile *f);
#endif
