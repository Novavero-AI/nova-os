/* init as the platform driver (docs/spec/ipc.md I6, I10): it holds the
   root device resources, enumerates PCI, starts the virtio block and
   input drivers with resources covering only their functions, and
   supervises them and the input driver's client. */

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
   device. Then find the first virtio input function and start its
   driver, \nova\virtinp.exe, and, once the driver reports, its client,
   \nova\inputlog.exe, leaving both running. A driver that fails is
   started again as the restart policy allows (platform.c). Returns 1 if
   the block driver ended with VIRTIO_BLK_EXIT_DONE, or if there is no
   block function or no driver file; 0, logged, if it was given up, and
   then before the input driver starts, so that init can end with
   nothing it started left running unsupervised; 0, logged, too if
   waiting on the supervisor's port fails. The input driver does not
   count: a system without a keyboard runs on, as one whose keyboard
   driver failed does. */
int platform_start(const PLATFORM *platform);

/* Supervise what platform_start left running for the life of the
   system: restart a driver or its client that ends, as the policy
   allows, and hand the client each endpoint a restarted driver reports.
   Returns only if waiting on the supervisor's port fails, with its
   status. */
UINT32 platform_supervise(const PLATFORM *platform);

#endif
