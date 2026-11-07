/* init as the platform driver (docs/spec/ipc.md I6, I10): it holds the
   root device resources, enumerates PCI, starts the virtio block and
   input drivers with resources covering only their functions, and
   supervises them. */

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
   \nova\virtblk.exe, to its end, then stop the function and reset its
   device, and run the driver once more if it failed. Then find the first
   virtio input function and start its driver, \nova\virtinp.exe, and,
   once the driver reports, its client, \nova\inputlog.exe, leaving both
   running; start the driver once more if it ends before it reports.
   Returns 1 if the block driver ended with VIRTIO_BLK_EXIT_DONE, or if
   there is no block function or no driver file; 0 if it failed. The
   input driver does not count: a system without a keyboard runs on, as
   one whose keyboard driver failed does. */
int platform_run(const PLATFORM *platform);

#endif
