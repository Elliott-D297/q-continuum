/* teachcases.h -- starters written from scratch whose answer is known exactly (improvement plan A2, 2026-11-26): the
 * New case page's "Heat conduction in a bar" (laplacianFoam) and "Carried by a flow" (scalarTransportFoam). Each writes a
 * small blockMesh case with a probe where the answer is checked, and .qc-starter (what it teaches, the answer). */
#ifndef TEACHCASES_H
#define TEACHCASES_H
#include <stdbool.h>

/* a bar 0.1 m long at 300 K whose ends go to 400 K at t = 0; diffusivity 1e-4 m²/s; probe at the middle; 10 s */
bool   tc_bar_write(const char *dst, char *err, int cap);
/* the middle's temperature at time t (Fourier series, 50 terms): 400 - 100 sum 4/(n pi) sin(n pi/2) exp(-(n pi)^2 a t / L^2) */
double tc_bar_mid(double t);
/* 0.1 m of fluid moving at 0.01 m/s, diffusivity 1e-4 m²/s (Peclet 10), 1 at the inlet, 0 at the outlet, run to steady
   (300 s); probes at x = 0.05 and 0.09 m */
bool   tc_flow_write(const char *dst, char *err, int cap);
/* the steady profile: (e^Pe - e^(Pe x / L)) / (e^Pe - 1) */
double tc_flow_value(double x);
extern const char *const TC_BAR_TITLE, *const TC_BAR_DESC, *const TC_FLOW_TITLE, *const TC_FLOW_DESC;
#endif
