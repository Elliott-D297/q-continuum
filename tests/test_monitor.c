/* test_monitor: the Monitors scanner (monitor.c). A forces time series across a restart is one series with two parts;
 * a sampled-sets function object (a folder per write time, no "# Time" header) is skipped and never listed again, so
 * damBreak's 100 000 sampleSets folders no longer freeze the window (2026-10-22); a time series whose first file is
 * still empty is picked up once it is written. */
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
