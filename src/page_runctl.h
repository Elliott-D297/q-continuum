/* page_runctl.h -- the Run page's run controls (page_runctl.c, runctl.h) */
#ifndef PAGE_RUNCTL_H
#define PAGE_RUNCTL_H
#include "foamcase.h"
/* adaptive time step (and its limits) for a solver that has one, else why not and what instead; coPeak / dtSeen: the
   largest Courant number of the last run and the step it was seen at (0 when unknown). Returns the height. */
float runctl_time_rows(Case *c, FDFile *cd, float x, float y, float w, bool locked, double coPeak, double dtSeen);
/* folded groups: writing, start and stop, the solver's loop, convergence, linear solvers. Returns the height. */
float runctl_panel(Case *c, float x, float y, float w, bool locked);
#endif
