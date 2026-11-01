/* test_monitor: the Monitors scanner (monitor.c). A forces time series across a restart is one series with two parts;
 * a sampled-sets function object (a folder per write time, no "# Time" header) is skipped and never listed again, so
 * damBreak's 100 000 sampleSets folders no longer freeze the window (2026-10-22); a time series whose first file is
 * still empty is picked up once it is written. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include "monitor.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void put(const char *path, const char *text)
{
    char d[PL_PATH]; pl_strlcpy(d, path, sizeof d);
    for (char *p = d + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(d, 0755); *p = '/'; }
    FILE *f = fopen(path, "w");
    if (f) { fputs(text, f); fclose(f); }
}

static double ms(void) { struct timeval tv; gettimeofday(&tv, NULL); return tv.tv_sec * 1e3 + tv.tv_usec / 1e3; }

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: test_monitor <scratch dir>\n"); return 2; }
    char c[2048], p[PL_PATH];
    snprintf(c, sizeof c, "%s/monitor.%d", argv[1], (int)getpid());
    mkdir(argv[1], 0755);
    mkdir(c, 0755);

    snprintf(p, sizeof p, "%s/postProcessing/forces/0/force.dat", c);   put(p, "# Force\n# Time  total_x  total_y\n0.1 1 2\n0.2 3 4\n");
    snprintf(p, sizeof p, "%s/postProcessing/forces/0.2/force.dat", c); put(p, "# Force\n# Time  total_x  total_y\n0.3 5 6\n");
    /* sampled sets: many write times, plain columns */
    for (int i = 0; i < 2000; i++) {
        snprintf(p, sizeof p, "%s/postProcessing/sampleSets/%g/line_U.xy", c, 0.001 * (i + 1)); put(p, "0 0 0 0\n0.1 1 0 0\n");
    }
    /* wallHeatFlux: a row per wall at each time (the patch's name a column), merged into one row a time */
    snprintf(p, sizeof p, "%s/postProcessing/wallHeatFlux/0/wallHeatFlux.dat", c);
    put(p, "# Wall heat-flux\n# Time          \tpatch           \tmin             \tmax             \tintegral        \n"
           "1\thot\t3.1e+01\t3.3e+01\t3.2e-02\n1\tcold\t-3.3e+01\t-3.1e+01\t-3.2e-02\n2\thot\t2.5e+01\t3.0e+01\t2.6e-02\n2\tcold\t-3.0e+01\t-2.5e+01\t-2.7e-02\n");
    /* a probe file not written yet at the first scan */
    snprintf(p, sizeof p, "%s/postProcessing/probes/0/p", c); put(p, "");

    MonSet *s = calloc(1, sizeof *s);
    monset_open(s, c);
    int fi = monset_find(s, "forces/force.dat");
    CHECK(fi >= 0, "forces series found (%d files)", s->n);
    if (fi >= 0) CHECK(s->f[fi].nParts == 2 && s->f[fi].n == 3, "forces: %d parts, %d rows", s->f[fi].nParts, s->f[fi].n);
    bool sk = false;
    for (int i = 0; i < s->nSkip; i++) if (!strcmp(s->skip[i], "sampleSets")) sk = true;
    CHECK(sk, "sampleSets skipped (%d skipped)", s->nSkip);
    for (int i = 0; i < s->n; i++) CHECK(strncmp(s->f[i].key, "sampleSets/", 11), "sampleSets file taken as a series: %s", s->f[i].key);
    CHECK(monset_find(s, "probes/p") < 0, "empty probe file is no series yet");
    int hi = monset_find(s, "wallHeatFlux/wallHeatFlux.dat");
    CHECK(hi >= 0, "wallHeatFlux found");
    if (hi >= 0) {
        MonFile *h = &s->f[hi];
        int ki = -1, kc = -1;
        for (int k = 0; k < h->nCols; k++) { if (!strcmp(h->col[k], "hot integral")) ki = k; if (!strcmp(h->col[k], "cold integral")) kc = k; }
        CHECK(h->n == 2 && ki >= 0 && kc >= 0 && h->nCols == 6, "wallHeatFlux: %d rows, %d columns (%s ...)", h->n, h->nCols, h->nCols ? h->col[0] : "");
        if (ki >= 0 && kc >= 0) CHECK(fabsf(h->v[ki][1] - 0.026f) < 1e-6f && fabsf(h->v[kc][1] + 0.027f) < 1e-6f, "the walls' heat per row");
    }

    /* later scans: the skipped folder costs nothing, the probe file shows up once written */
    snprintf(p, sizeof p, "%s/postProcessing/probes/0/p", c);
    put(p, "# Probe 0 (0 0 0)\n# Time  0\n0.1 1e5\n");
    double t0 = ms();
    monset_poll(s, 100);
    double dt = ms() - t0;
    CHECK(monset_find(s, "probes/p") >= 0, "probe series found after it was written");
    CHECK(dt < 50, "rescan took %.1f ms", dt);
    printf("rescan %.2f ms, %d series, %d skipped\n", dt, s->n, s->nSkip);

    monset_close(s);
    free(s);
    char cmd[PL_PATH + 16]; snprintf(cmd, sizeof cmd, "rm -rf '%s'", c);
    if (system(cmd) != 0) fails++;

    printf(fails ? "%d FAILED\n" : "ok\n", fails);
    return fails != 0;
}
