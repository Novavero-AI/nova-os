/* The virtio block driver's face to the platform driver that starts it:
   the PCI IDs it drives, its two modes, its report when it serves a
   client, its entry, and its exit codes. */

#ifndef NOVA_VIRTIO_BLK_H
#define NOVA_VIRTIO_BLK_H

#include "native_abi.h"

/* A virtio device has PCI vendor ID 1AF4h; a block device, virtio device
   ID 2, has PCI device ID 1040h + 2, or 1001h if it is transitional
   (VIRTIO 1.4 4.1.2, 5.2.1). */
#define VIRTIO_PCI_VENDOR 0x1af4
#define VIRTIO_BLK_PCI_DEVICE 0x1042
#define VIRTIO_BLK_PCI_TRANSITIONAL 0x1001

/* The argument the driver starts with: VIRTIO_BLK_INSPECT checks the
   boot disk's partition table and boot sector, logging only a check
   they fail, then ends; VIRTIO_BLK_SERVE serves one client the block
   protocol (block_protocol.h) until the client closes its end. */
#define VIRTIO_BLK_INSPECT 0
#define VIRTIO_BLK_SERVE 1

/* Serving, once its device is live the driver writes one message on its
   bootstrap channel: VIRTIO_BLK_REPORT_SIZE bytes, a transaction ID of 0
   (ipc.md I3), and one handle, the client's endpoint, with
   BLOCK_CLIENT_RIGHTS. It then closes the channel. */
#define VIRTIO_BLK_REPORT_SIZE 4

/* The driver ends with VIRTIO_BLK_EXIT_DONE once it has done its mode's
   work, found the boot disk's sectors as its check expects or served a
   client that has closed its end, and reset the device; else, the
   check failed or a step did, with VIRTIO_BLK_EXIT_FAILED. Both lie
   above the exception vectors a faulting process ends with
   (docs/spec/objects.md O10). */
#define VIRTIO_BLK_EXIT_DONE 100
#define VIRTIO_BLK_EXIT_FAILED 101

/* The driver's entry, from a PCI device bootstrap message (pci.h), with
   its mode as argument. */
_Noreturn void virtio_blk_main(UINT32 bootstrap, UINT64 argument);

#endif
