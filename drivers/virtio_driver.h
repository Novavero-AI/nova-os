/* How a user-mode virtio driver starts its device over the PCI
   transport's modern interface (VIRTIO 1.4 4.1): it reads the bootstrap
   message the platform driver sends it (pci.h); maps its function's
   configuration space and the BARs the virtio structures lie in
   (4.1.4); resets the device and negotiates features in the order 3.1.1
   gives; takes MSI-X table entry 0 for its queues' used-buffer
   notifications (4.1.5.1.2), delivered as packets on a port; and sets up
   split virtqueues (2.7) in pages it pins for the device. A step that
   fails logs why under the driver's name and ends the process with the
   driver's failure code. */

#ifndef NOVA_VIRTIO_DRIVER_H
#define NOVA_VIRTIO_DRIVER_H

#include "native.h"
#include "pci.h"
#include "virtio_pci.h"

/* The reserved feature bits (6), as bit numbers in the 64 the driver
   negotiates: VERSION_1, which every driver here requires;
   ACCESS_PLATFORM, which each accepts, since the addresses it gives the
   device are those section_pin returns; and ORDER_PLATFORM, which each
   accepts, since its barriers are the processor's fences, which suit
   hardware devices (6.1). */
#define VIRTIO_F_VERSION_1 32
#define VIRTIO_F_ACCESS_PLATFORM 33
#define VIRTIO_F_ORDER_PLATFORM 36
#define VIRTIO_FEATURES_RESERVED                                         \
    (1ULL << VIRTIO_F_VERSION_1 | 1ULL << VIRTIO_F_ACCESS_PLATFORM |     \
     1ULL << VIRTIO_F_ORDER_PLATFORM)

/* A split virtqueue's parts (2.7): a descriptor (2.7.5) is 16 bytes, its
   address, length, flags and next; the available ring (2.7.6) and the
   used ring (2.7.8) each start with flags and an index, then their
   entries, 2 bytes each in the available ring and 8, an ID and a
   length, in the used ring, and end with an event index field: 6 bytes
   besides the entries (2.7's table of sizes). */
#define VIRTQ_DESCRIPTOR_SIZE 16
#define VIRTQ_DESC_ADDRESS 0
#define VIRTQ_DESC_LENGTH 8
#define VIRTQ_DESC_FLAGS 12
#define VIRTQ_DESC_NEXT 14
#define VIRTQ_DESC_F_NEXT 1
#define VIRTQ_DESC_F_WRITE 2
#define VIRTQ_RING_FLAGS 0
#define VIRTQ_RING_INDEX 2
#define VIRTQ_RING_ENTRIES 4
#define VIRTQ_RING_EXTRA_SIZE 6
#define VIRTQ_AVAIL_ENTRY_SIZE 2
#define VIRTQ_USED_ELEMENT_SIZE 8
#define VIRTQ_USED_ID 0
#define VIRTQ_USED_LENGTH 4
#define VIRTQ_USED_F_NO_NOTIFY 1

/* The MSI-X table entry every queue uses. */
#define VIRTIO_QUEUE_VECTOR 0

typedef struct {
    const char *name;   /* the driver's log prefix */
    UINT32 exit_failed; /* its exit code when a step fails */
    UINT32 debug_log, self, initiator, irq, config_resource;
    UINT32 bar_resource[PCI_BARS];
    PCI_DEVICE_BOOTSTRAP device;
    UINT64 next_map;                /* where the next mapping goes */
    volatile UINT8 *config;         /* the function's configuration space */
    volatile UINT8 *bars[PCI_BARS]; /* the BARs mapped, or 0 */
    volatile UINT8 *common;
    volatile UINT8 *device_config;
    UINT32 structures[VIRTIO_CAP_TYPES];
    UINT32 msix;
    UINT32 port, interrupt;
} VIRTIO_DRIVER;

/* Log what failed under the driver's name, with status, and end the
   process with its failure code. */
_Noreturn void virtio_driver_fail(const VIRTIO_DRIVER *driver,
                                  const char *what, UINT32 status);

/* virtio_driver_fail unless status is OK. */
void virtio_driver_check(const VIRTIO_DRIVER *driver, UINT32 status,
                         const char *what);

/* Take the handles and the record of the bootstrap message queued on
   channel, which stays open; open the log; map the function's
   configuration space, with memory decoding on and INTx off; map the
   common configuration structure and the first device_config_size bytes
   of the device-specific one, each checked to lie within its capability
   and its BAR; reset the device; and only then turn bus mastering on,
   so that a device a dead driver left running reaches no memory through
   its old rings. A message that is not a PCI device bootstrap ends the
   process at once, before there is a log to say so. */
void virtio_driver_open(VIRTIO_DRIVER *driver, const char *name,
                        UINT32 exit_failed, UINT32 channel,
                        UINT64 device_config_size);

/* Map size bytes of section, from its start, read-write at the next free
   address in the driver, and return where. */
volatile UINT8 *virtio_driver_map(VIRTIO_DRIVER *driver, UINT32 section,
                                  UINT64 size, const char *what);

/* OR bits into device_status (2.1). */
void virtio_driver_set_status(VIRTIO_DRIVER *driver, UINT8 bits);

/* Reset the device (virtio_pci_reset), or fail. */
void virtio_driver_reset(VIRTIO_DRIVER *driver);

/* The configuration generation (4.1.4.3), which changes whenever two
   reads of the device-specific configuration could see different
   versions of it (2.5). */
UINT8 virtio_driver_generation(const VIRTIO_DRIVER *driver);

/* Steps 2 to 6 of 3.1.1: set ACKNOWLEDGE and DRIVER, accept the
   features the device offers that accepted names, set FEATURES_OK and
   check that the device kept it. Fails if the device offers no
   VERSION_1. Returns the features accepted. */
UINT64 virtio_driver_negotiate(VIRTIO_DRIVER *driver, UINT64 accepted);

/* Create a port, an interrupt for the function's message-signalled
   interrupts bound to it with key, and write the message the kernel
   gives into MSI-X table entry VIRTIO_QUEUE_VECTOR. */
void virtio_driver_interrupt(VIRTIO_DRIVER *driver, UINT64 key);

/* A section of pages pages, mapped into the driver and pinned for a
   device that reads and writes it; writes each page's device address to
   addresses and the pin to pin, and returns where it is mapped. */
volatile UINT8 *virtio_driver_dma(VIRTIO_DRIVER *driver, UINT32 pages,
                                  UINT64 *addresses, UINT32 *pin);

/* 4.1.5.1.3: select queue index, check that the device's maximum holds
   size entries and set that size, give the device its parts' device
   addresses and MSI-X table entry VIRTIO_QUEUE_VECTOR, which the device
   confirms by reading it back (4.1.5.1.2.2), leave configuration changes
   without a vector, and enable the queue. Returns the queue's notify
   address (4.1.4.4). */
volatile UINT8 *virtio_driver_queue(VIRTIO_DRIVER *driver, UINT16 index,
                                    UINT16 size, UINT64 descriptors,
                                    UINT64 available, UINT64 used);

/* Expose the buffers the driver has placed in queue's available ring at
   available, up to its new index (2.7.13): fence after the descriptors
   and ring entries already stored, then store the index, so the device
   sees the chains before the index exposes them (2.7.13.3.1); fence
   again before reading the flags of the used ring at used
   (2.7.13.4.1); and notify the device by writing the queue's index to
   notify (4.1.5.2) unless those flags ask for no notification
   (2.7.10). */
void virtio_driver_publish(volatile UINT8 *available,
                           const volatile UINT8 *used,
                           volatile UINT8 *notify, UINT16 queue,
                           UINT16 index);

#endif
