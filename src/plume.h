/* plume.h -- ROADMAP 4.4: the impinging-plume starter. An axisymmetric (wedge) under-expanded jet from a nozzle exit
 * onto a flat plate at a stand-off height, solved with rhoCentralFoam (Kurganov flux, vanLeer reconstruction, as the
 * LadenburgJet60psi tutorial), set up from the nozzle exit state. Pure: computes the numbers and writes the case. */
#ifndef PLUME_H
#define PLUME_H

#include <stdbool.h>

typedef struct {
    int    gas;            /* 0 N2, 1 air, 2 other (molWeight, gamma below) */
    double molWeight;      /* kg/kmol */
    double gamma;
    double Me;             /* nozzle exit Mach number */
    double pe, Te;         /* exit static pressure [Pa] and temperature [K] */
    double re;             /* exit radius [m] */
    double H;              /* stand-off: exit plane to plate [m] */
    double R;              /* domain radius [m] */
    double pa, Ta;         /* ambient pressure [Pa], temperature [K] */
    double Tw;             /* plate temperature [K] */
    int    cellsPerRadius; /* cells across the exit radius */
    double maxCo;          /* 0.2 */
} PlumeIn;

typedef struct {
    double Rs;             /* specific gas constant J/(kg K) */
    double ae, Ue;         /* exit sound speed and velocity m/s */
    double rhoe, mdot;     /* exit density kg/m3, mass flow kg/s (whole nozzle) */
    double p0;             /* exit stagnation pressure Pa */
    double npr;            /* p0 / pa */
    double per;            /* pe / pa: > 1 under-expanded */
    double xMach;          /* Mach-disk distance, Ashkenas-Sherman 0.67 De sqrt(p0/pa) (sonic-nozzle correlation) */
    double dx;             /* cell size at the axis m */
    int    nx, nr1, nr2;   /* cells along the axis, across the exit radius, beyond it */
    long   cells;
    double dt0, endTime, writeInterval;
    long   steps;          /* time steps to endTime, with the wedge's smaller step */
    double cpuSeconds;     /* core-seconds, from ~11 us per cell and step */
    double Cp, As, Ts;     /* hConst Cp; Sutherland coefficients */
    char   warn[400];      /* what looks off ("" = nothing) */
} PlumeOut;

void plume_defaults(PlumeIn *in);
bool plume_compute(const PlumeIn *in, PlumeOut *out, char *err, int cap);   /* false: the inputs can't make a case */
bool plume_write(const char *dir, const PlumeIn *in, char *err, int cap);   /* writes the whole case into dir (new) */

#endif
