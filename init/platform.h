/* init as the platform driver (docs/spec/ipc.md I6, I10): it holds the
   root device resources, enumerates PCI, starts the virtio block and
   input drivers with resources covering only their functions, and
   supervises them and the input drivers' client, the console or the
   compositor. */

#ifndef NOVA_PLATFORM_H
#define NOVA_PLATFORM_H

#include "native.h"
#include "pci.h"
#include "program.h"

/* What is the input drivers' client from the kernel's release on
   (docs/spec/display.md D2): a compositor, which the console takes over
   from if it is given up (D3), or the console, where no compositor runs,
   on the type specimen's disk. */
typedef enum { DRAWER_CONSOLE, DRAWER_COMPOSITOR } DRAWER;

typedef struct {
    UINT32 debug_log; /* the root system resource */
    UINT32 self;      /* init's own process, with MAP */
    UINT32 job;       /* the root job */
    UINT32 mmio, irq, dma;
    const BOOTSTRAP_PLATFORM *boot;
    DRAWER drawer;
} PLATFORM;

/* On a test disk, find the first virtio block function and run its
   driver, \nova\virtblk.exe, to its end, then stop the function and
   reset its device, and report the boot-disk test (tests.h). Then start
   a driver, \nova\virtinp.exe, for each virtio input function, up to
   INPUT_DEVICES_MAX, one after another as each reports, and, once the
   first reports, their client, the console, \nova\console.exe, leaving
   them running; with drawer DRAWER_COMPOSITOR the endpoints wait for the
   compositor instead (platform_start_compositor). A driver that fails
   is started again as the restart policy allows (platform.c). Returns 1, or 0, logged, if waiting on
   the supervisor's port fails. The data disk's driver and file system
   service, which data.c starts before this and nothing supervises,
   outlive such an end and wind down on their own: the service once
   init's end of its root closes, the driver once the service's end of
   its channel does. The input drivers do not count: a system without a
   keyboard or a pointer runs on, as one whose input drivers failed
   does. */
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

/* Lay out a program the supervisor keeps running, with its bootstrap
   message written and its other messages queued: on OK, *program is
   loaded, not started, and *bootstrap the end of its bootstrap channel
   it is to start with; otherwise nothing is left open, and the status
   says why, which the supervisor logs. */
typedef UINT32 (*PLATFORM_LOAD)(const PLATFORM *platform, PROGRAM *program,
                                UINT32 *bootstrap);

/* Start the program load lays out, which log lines call name, and keep
   it running for the life of the system: the supervisor watches it and
   starts it again with load when it ends, as the restart policy allows.
   Returns 1 if it started; 0, logged, if it did not and was given up. */
int platform_keep(const PLATFORM *platform, const char *name,
                  PLATFORM_LOAD load);

/* With drawer DRAWER_COMPOSITOR, and once the kernel has released the
   display: start the compositor, \nova\compose.exe, in the root job on
   a client channel of its own, and hand it each input endpoint, those
   reported before it started first; then start the console beside it,
   which gets no endpoint while the compositor runs and a connection to
   it for its window (docs/spec/display.md D19, rule 2). The compositor
   is supervised as the console is; once it is given up, the console is
   the input drivers' client, and is sent the display after a compositor
   (console.h). */
void platform_start_compositor(const PLATFORM *platform);

/* End the compositor that runs, which has not answered a connection by
   its deadline (compositor.h): task_kill on its process, which needs
   nothing of it. Its end then reaches the supervisor's port as any end
   of it does, exit code PROCESS_EXIT_KILLED, and it is started again or
   given up as the restart policy allows. Returns task_kill's status. */
UINT32 platform_end_compositor(void);

/* Supervise what platform_start left running for the life of the
   system: restart a driver or its client that ends, as the policy
   allows, and hand the client each endpoint a restarted driver reports.
   Returns only if waiting on the supervisor's port fails, with its
   status. */
UINT32 platform_supervise(const PLATFORM *platform);

#endif
