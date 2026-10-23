/* test_sweep <dir> [--all]: SENSOR SWEEP over every case under dir (the shipped tutorials are correct, so what it
 * reports there as a problem is a false alarm). Prints each PC_BAD (and with --all PC_WARN) finding; exit 1 if any BAD. */
#define _GNU_SOURCE
#include <ftw.h>
#include <stdio.h>
#include <string.h>
#include "sweep.h"

static int nCases, nBad, nWarn, nAsk; static bool all;
static int visit(const char *p, const struct stat *st, int type, struct FTW *ftw)
{
    (void)st; (void)ftw;
    if (type != FTW_D || !case_is_case(p)) return 0;
    Case *c = case_open(p);
    if (!c) return 0;
    nCases++;
    Sweep w; sensor_sweep(c, &w);
    if (w.ask) { nAsk++; if (all) printf("ASK  %s\n", p); }
    for (int i = 0; i < w.n; i++) {
        if (w.f[i].level == PC_BAD) nBad++; else if (w.f[i].level == PC_WARN) nWarn++;
        if (w.f[i].level == PC_BAD || (all && w.f[i].level == PC_WARN))
            printf("%s %s: %s\n", w.f[i].level == PC_BAD ? "BAD " : "warn", p, w.f[i].x.title);
    }
    case_close(c);
    return 0;
}
int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    if (argc > 2 && !strcmp(argv[2], "--case")) {        /* every finding of one case, with its text */
        Case *c = case_open(argv[1]);
        if (!c) return 1;
        Sweep w; sensor_sweep(c, &w);
        for (int i = 0; i < w.n; i++) printf("[%d] %s\n    %s\n", w.f[i].level, w.f[i].x.title, w.f[i].x.text);
        case_close(c);
        return 0;
    }
    all = argc > 2 && !strcmp(argv[2], "--all");
    nftw(argv[1], visit, 32, FTW_PHYS);
    printf("%d cases, %d problems, %d warnings, %d would ask before a run\n", nCases, nBad, nWarn, nAsk);
    return nBad ? 1 : 0;
}
