/* redalert.h -- divergence watch over a solver's log (see redalert.c) */
#ifndef REDALERT_H
#define REDALERT_H

#include <stdbool.h>
#include "runner.h"

enum { RA_NONE = 0, RA_WATCH = 1, RA_ALERT = 2 };
enum { RA_RESIDUAL = 1, RA_COURANT = 2, RA_DT = 4, RA_BOUNDING = 8 };

typedef struct {
    int  level;                  /* RA_NONE, RA_WATCH, RA_ALERT */
    int  kind;                   /* RA_RESIDUAL | RA_COURANT | ...: which signs */
    int  reasons;
    char title[96];
    char text[1200];             /* one line per sign, in numbers */
} Alert;

/* adjustDt / maxCo from the case's controlDict; returns the level */
int red_alert_check(const LogMon *m, bool adjustDt, double maxCo, Alert *out);

#endif
