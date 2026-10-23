/* fuzz_logparse.c -- libFuzzer harness for the solver-log parser: any bytes, in any chunking, must parse without a
 * memory error and leave the series consistent. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "runner.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    LogMon *m = calloc(1, sizeof *m);
    size_t at = 0, k = 1;
    while (at < size) {                                      /* uneven chunks, as a growing log is read */
        size_t n = (k * 7919) % 97 + 1;
        if (n > size - at) n = size - at;
        logmon_feed(m, (const char *)data + at, (int)n);
        at += n; k++;
    }
    for (int i = 0; i < m->nSeries; i++) if (m->series[i].n < 0 || m->series[i].n > m->series[i].cap) abort();
    for (int i = 0; i < m->nLines; i++) { int len; const char *l = logmon_line(m, i, &len); if (!l || len < 0) abort(); }
    logmon_reset(m);
    free(m);
    return 0;
}
