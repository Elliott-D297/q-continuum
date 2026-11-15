/* gpu.c -- see gpu.h */
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "gpu.h"

static pthread_mutex_t gMu = PTHREAD_MUTEX_INITIALIZER;
static GpuInfo gInfo;
static bool gStarted, gFast;

static bool query(GpuInfo *g)
{
    static const char *const CMD[] = {
        "nvidia-smi --query-gpu=name,memory.total,memory.used --format=csv,noheader,nounits 2>/dev/null",
        "/usr/lib/wsl/lib/nvidia-smi --query-gpu=name,memory.total,memory.used --format=csv,noheader,nounits 2>/dev/null",
    };
    for (int i = 0; i < 2; i++) {
        FILE *p = popen(CMD[i], "r");
        if (!p) continue;
        char line[256] = "";
        bool got = fgets(line, sizeof line, p) != NULL;
        pclose(p);
        if (!got) continue;
        char *c1 = strchr(line, ','), *c2 = c1 ? strchr(c1 + 1, ',') : NULL;
        if (!c1 || !c2) continue;
        *c1 = 0;
        snprintf(g->name, sizeof g->name, "%s", line);
        int t = 0, u = 0;
        if (sscanf(c1 + 1, "%d", &t) == 1 && sscanf(c2 + 1, "%d", &u) == 1 && t > 0) { g->totalMB = t; g->usedMB = u; return true; }
    }
    return false;
}

static void *poller(void *arg)
{
    (void)arg;
    double last = -1e9;
    for (;;) {
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        double now = (double)ts.tv_sec + ts.tv_nsec * 1e-9;
        pthread_mutex_lock(&gMu); bool fast = gFast; pthread_mutex_unlock(&gMu);
        if (now - last >= (fast ? 1.0 : 30.0)) {
            GpuInfo g; memset(&g, 0, sizeof g);
            g.ok = query(&g); g.probed = true;
            pthread_mutex_lock(&gMu); gInfo = g; pthread_mutex_unlock(&gMu);
            last = now;
        }
        usleep(250000);
    }
    return NULL;
}

GpuInfo gpu_get(void)
{
    pthread_mutex_lock(&gMu);
    if (!gStarted) {
        gStarted = true;
        pthread_t th;
        if (pthread_create(&th, NULL, poller, NULL) == 0) pthread_detach(th);
    }
    GpuInfo g = gInfo;
    pthread_mutex_unlock(&gMu);
    return g;
}

void gpu_poll_fast(bool on) { pthread_mutex_lock(&gMu); gFast = on; pthread_mutex_unlock(&gMu); }
