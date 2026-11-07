/* The virtio block driver's face to the platform driver that starts it:
   the PCI IDs it drives, its entry, and its exit codes. */

#ifndef NOVA_VIRTIO_BLK_H
#define NOVA_VIRTIO_BLK_H

#include "native_abi.h"

/* A virtio device has PCI vendor ID 1AF4h; a block device, virtio device
   ID 2, has PCI device ID 1040h + 2, or 1001h if it is transitional
   (VIRTIO 1.4 4.1.2, 5.2.1). */
#define VIRTIO_PCI_VENDOR 0x1af4
#define VIRTIO_BLK_PCI_DEVICE 0x1042
#define VIRTIO_BLK_PCI_TRANSITIONAL 0x1001

/* The driver ends with VIRTIO_BLK_EXIT_DONE once it has read and logged
   the sectors and reset the device, else with VIRTIO_BLK_EXIT_FAILED.
   Both lie above the exception vectors a faulting process ends with
   (docs/spec/objects.md O10). */
#define VIRTIO_BLK_EXIT_DONE 100
#define VIRTIO_BLK_EXIT_FAILED 101

/* The driver's entry, from a PCI device bootstrap message (pci.h). */
_Noreturn void virtio_blk_main(UINT32 bootstrap, UINT64 argument);

#endif
