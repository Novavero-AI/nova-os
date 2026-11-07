/* init as the platform driver (docs/spec/ipc.md I6, I10): it holds the
   root device resources, enumerates PCI, starts the virtio block and
   input drivers with resources covering only their functions, and
   supervises them and the input driver's client, the console. */

#ifndef NOVA_PLATFORM_H
#define NOVA_PLATFORM_H

#include "native.h"
#include "pci.h"

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
   the console, \nova\console.exe, leaving both running. A driver that fails is
   started again as the restart policy allows (platform.c). Returns 1 if
   the block driver ended with VIRTIO_BLK_EXIT_DONE, or if there is no
   block function or no driver file; 0, logged, if it was given up, and
   then before the input driver starts, so that init can end with no
   driver this function started left running unsupervised; 0, logged,
   too if waiting on the supervisor's port fails. The data disk's driver
   and file system service, which data.c starts before this and nothing
   supervises, outlive such an end and wind down on their own: the
   service once init's end of its root closes, the driver once the
   service's end of its channel does. The input driver does not
   count: a system without a keyboard runs on, as one whose keyboard
   driver failed does. */
int platform_start(const PLATFORM *platform);

/* The data disk, the second virtio block function in the order of the
   walk platform_start makes, whose first is the boot disk: whether there
   is one, and if so its driver's bootstrap record, BARs sized, in
   device. The walk is made once, for both. */
int platform_data_disk(const PLATFORM *platform, PCI_DEVICE_BOOTSTRAP *device);

/* The handles of a driver's bootstrap message for the function device
   describes, as pci.h orders them: the debug log, process with MAP, the
   initiator with PIN alone, and resources covering the function's
   interrupts, configuration space and BARs. Returns their count, or 0,
   with none left open, if a call fails. */
UINT32 platform_driver_handles(const PLATFORM *platform,
                               const PCI_DEVICE_BOOTSTRAP *device,
                               UINT32 process, UINT32 initiator,
                               HANDLE_TRANSFER *handles);

/* A job of its own for a driver, a child of the root job that grants it
   the one interrupt vector a driver takes, as I10 has a supervisor do;
   HANDLE_INVALID, logged, if it cannot be made. */
UINT32 platform_driver_job(const PLATFORM *platform);

/* Supervise what platform_start left running for the life of the
   system: restart a driver or its client that ends, as the policy
   allows, and hand the client each endpoint a restarted driver reports.
   Returns only if waiting on the supervisor's port fails, with its
   status. */
UINT32 platform_supervise(const PLATFORM *platform);

#endif
