/* The virtio input driver's face to the platform driver that starts it:
   the PCI ID it drives, its report once the device is live, its entry,
   and its exit codes (docs/spec/input.md). */

#ifndef NOVA_VIRTIO_INPUT_H
#define NOVA_VIRTIO_INPUT_H

#include "native_abi.h"

/* An input device, virtio device ID 18, has PCI device ID 1040h + 18
   and no transitional ID (VIRTIO 1.4 4.1.2, 5.8.1). */
#define VIRTIO_INPUT_PCI_DEVICE 0x1052

/* Once its device is live, the driver writes one message on its
   bootstrap channel: VIRTIO_INPUT_REPORT_SIZE bytes, a transaction ID
   of 0 (ipc.md I3), and one handle, the endpoint a client reads key
   events from (input_protocol.h), with VIRTIO_INPUT_CLIENT_RIGHTS. It
   then closes the channel. */
#define VIRTIO_INPUT_REPORT_SIZE 4
#define VIRTIO_INPUT_CLIENT_RIGHTS                                       \
    (CHANNEL_RIGHT_READ | RIGHT_WAIT | RIGHT_TRANSFER)

/* The driver ends with VIRTIO_INPUT_EXIT_DONE once a send has found
   its client's endpoint closed and it has reset the device and released
   its pin, else with VIRTIO_INPUT_EXIT_FAILED. Both lie above the exception
   vectors a faulting process ends with (docs/spec/objects.md O10). */
#define VIRTIO_INPUT_EXIT_DONE 100
#define VIRTIO_INPUT_EXIT_FAILED 101

/* The driver's entry, from a PCI device bootstrap message (pci.h). */
_Noreturn void virtio_input_main(UINT32 bootstrap, UINT64 argument);

#endif
