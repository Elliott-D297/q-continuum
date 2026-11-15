/* gpu.h -- the NVIDIA GPU as nvidia-smi reports it (workflow plan D3, 2026-11-14), polled on a background thread so a frame
 * never waits for it (nvidia-smi takes ~75 ms): every 30 s, every second while gpu_poll_fast(true). */
#ifndef GPU_H
#define GPU_H
#include <stdbool.h>
typedef struct {
    bool probed;          /* asked at least once                                  */
    bool ok;              /* nvidia-smi answered                                  */
    char name[96];
    int  totalMB, usedMB;
} GpuInfo;
GpuInfo gpu_get(void);    /* starts the poller on first use; probed false until its first answer */
void gpu_poll_fast(bool on);
#endif
