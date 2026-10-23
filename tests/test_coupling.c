/* test_coupling.c -- open a coupled simulation, print participants, commands and checks;
 *   test_coupling <folder> [--run <bashrc> <precice-env>]   runs it to the end and prints the outcome */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "coupling.h"

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <folder> [--run bashrc preciceEnv]\n", argv[0]); return 2; }
    Coupling *c = coupling_open(argv[1]);
    if (!c) { printf("no precice-config.xml in %s\n", argv[1]); return 1; }
    char tag[64] = ""; XNode *cs = coupling_scheme(c); if (cs) snprintf(tag, sizeof tag, "%s", cs->tag);
    printf("%s: %d participants, scheme %s\n", c->name, c->n, tag);
    for (int i = 0; i < c->n; i++) {
        CParticipant *p = &c->p[i];
        char cmd[700]; coupling_default_cmd(c, p, cmd, sizeof cmd);
        printf("  %-8s dir=%s (of %d) foam=%d solver=%s runsh=%d cmd=[%s]\n", p->name, p->dirSel >= 0 ? p->dirs[p->dirSel] : "-", p->nDirs, p->foam, p->solver, p->hasRunSh, cmd);
    }
    CCheck ck[64]; int n = coupling_check(c, ck, 64);
    for (int i = 0; i < n; i++) printf("  [%s] %-10s %s%s\n", ck[i].kind == 2 ? "ERR " : ck[i].kind ? "WARN" : " ok ", ck[i].who, ck[i].msg, ck[i].fix ? " (fixable)" : "");
    int bad = 0;
    if (argc >= 5 && !strcmp(argv[2], "--run")) {
        char err[256];
        if (!coupling_start(c, argv[3], argv[4], now(), err, sizeof err)) { printf("start failed: %s\n", err); return 1; }
        while (c->running) { coupling_poll(c, now()); usleep(200000); }
        coupling_read_precice_logs(c);
        for (int i = 0; i < c->n; i++) {
            CParticipant *p = &c->p[i];
            printf("  %-8s exit=%d failed=%d %.1f s, %d windows in iterations log, %d convergence rows (%s)\n", p->name, p->exitCode, p->failed, p->t1 - p->t0, p->nIt, p->nCv, p->cvName);
            bad |= p->failed;
        }
    }
    coupling_close(c);
    return bad;
}
