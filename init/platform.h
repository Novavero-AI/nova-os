/* init as the platform driver (docs/spec/ipc.md I6, I10): it holds the
   root device resources, enumerates PCI, starts the virtio block driver
   with resources covering only its function, and supervises it. */

#ifndef NOVA_PLATFORM_H
#define NOVA_PLATFORM_H

#include "native.h"

typedef struct {
    UINT32 debug_log; /* the root system resource */
    UINT32 self;      /* init's own process, with MAP */
    UINT32 job;       /* the root job */
    UINT32 mmio, irq, dma;
    const BOOTSTRAP_PLATFORM *boot;
} PLATFORM;

/* Find the first virtio block function and run its driver,
   \nova\virtblk.exe, to its end, then stop the function's DMA. Returns 1
   if the driver ended with VIRTIO_BLK_EXIT_DONE, or if there is no such
   function or no driver file; 0 if the driver failed. */
int platform_run(const PLATFORM *platform);

#endif
