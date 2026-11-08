/* A virtio driver's start over the PCI transport, shared by the virtio
   drivers. */

#include "virtio_driver.h"
#include "log.h"
#include "mmio.h"

/* Where a driver maps its sections: from MAP_BASE up, each mapping at
   the next free address, far above its image and below its stack. */
#define MAP_BASE 0x0000000200000000ULL

#define FEATURE_WORD_BITS 32
#define PORT_CAPACITY 1

/* The value that leaves an event without an MSI-X vector
   (4.1.5.1.2). */
#define NO_VECTOR 0xffff

/* The notification address must leave room for the 16-bit value written
   there (4.1.4.4.1, 4.1.5.2). */
#define NOTIFY_WIDTH 2

_Noreturn void virtio_driver_fail(const VIRTIO_DRIVER *driver,
                                  const char *event, UINT32 status)
{
    say(driver->name);
    say(": ");
    say(event);
    say(" failed status=");
    say_number(status);
    say("\r\n");
    process_exit(driver->exit_failed);
}

void virtio_driver_check(const VIRTIO_DRIVER *driver, UINT32 status,
                         const char *event)
{
    if (status != OK)
        virtio_driver_fail(driver, event, status);
}

/* ------------------------------------------------------------------------
   Bootstrap and mappings. */

static void read_bootstrap(VIRTIO_DRIVER *driver, UINT32 channel)
{
    RECEIVED_HANDLE handles[PCI_HANDLES_MAX];
    UINT32 byte_count, handle_count, i, next = PCI_HANDLE_BARS;

    if (channel_read(channel, &driver->device, sizeof driver->device,
                     handles, PCI_HANDLES_MAX, &byte_count,
                     &handle_count) != OK ||
        byte_count != sizeof driver->device || handle_count < PCI_HANDLE_BARS)
        process_exit(driver->exit_failed);
    driver->debug_log = handles[PCI_HANDLE_DEBUG_LOG].handle;
    say_open(driver->debug_log);
    driver->self = handles[PCI_HANDLE_SELF].handle;
    driver->initiator = handles[PCI_HANDLE_INITIATOR].handle;
    driver->irq = handles[PCI_HANDLE_IRQ].handle;
    driver->config_resource = handles[PCI_HANDLE_CONFIG].handle;
    for (i = 0; i < PCI_BARS; i++) {
        driver->bar_resource[i] = HANDLE_INVALID;
        if (driver->device.bar_size[i] && next < handle_count)
            driver->bar_resource[i] = handles[next++].handle;
    }
}

volatile UINT8 *virtio_driver_map(VIRTIO_DRIVER *driver, UINT32 section,
                                  UINT64 size, const char *event)
{
    UINT64 address = driver->next_map;

    virtio_driver_check(driver,
                        process_map(driver->self, section, 0, address, size,
                                    MAP_READ | MAP_WRITE),
                        event);
    driver->next_map += size;
    return (volatile UINT8 *)address;
}

/* Map size bytes of device memory at base, covered by resource, uncached
   into the driver, and return where; event names the step in the line
   a failure logs. */
static volatile UINT8 *map_device(VIRTIO_DRIVER *driver, UINT32 resource,
                                  UINT64 base, UINT64 size, const char *event)
{
    volatile UINT8 *address;
    UINT32 section;

    virtio_driver_check(driver,
                        section_create_physical(resource, base, size,
                                                CACHE_UNCACHED, &section),
                        event);
    address = virtio_driver_map(driver, section, size, event);
    handle_close(section);
    return address;
}

static volatile UINT8 *bar(VIRTIO_DRIVER *driver, UINT32 index)
{
    const PCI_DEVICE_BOOTSTRAP *device = &driver->device;

    if (index >= PCI_BARS || !device->bar_size[index])
        virtio_driver_fail(driver, "structure bar", ERR_NOT_SUPPORTED);
    if (!driver->bars[index])
        driver->bars[index] =
            map_device(driver, driver->bar_resource[index],
                       device->bar_base[index], device->bar_size[index],
                       "bar map");
    return driver->bars[index];
}

/* Where in the driver the structure capability cap describes lies, from
   at bytes into it on, checked to hold size bytes from there within the
   structure and within its BAR. */
static volatile UINT8 *structure(VIRTIO_DRIVER *driver, UINT32 cap,
                                 UINT64 at, UINT64 size)
{
    const PCI_DEVICE_BOOTSTRAP *device = &driver->device;
    UINT32 index = mmio_read8(driver->config + cap + VIRTIO_CAP_BAR);
    UINT64 offset = mmio_read32(driver->config + cap + VIRTIO_CAP_OFFSET);
    UINT64 length = mmio_read32(driver->config + cap + VIRTIO_CAP_LENGTH);

    if (at > length || size > length - at || index >= PCI_BARS ||
        offset + at > device->bar_size[index] ||
        size > device->bar_size[index] - offset - at)
        virtio_driver_fail(driver, "structure range", ERR_OUT_OF_RANGE);
    return bar(driver, index) + offset + at;
}

void virtio_driver_open(VIRTIO_DRIVER *driver, const char *name,
                        UINT32 exit_failed, UINT32 channel,
                        UINT64 device_config_size)
{
    volatile UINT8 *command;
    UINT32 *structures = driver->structures;

    driver->name = name;
    driver->exit_failed = exit_failed;
    driver->next_map = MAP_BASE;
    read_bootstrap(driver, channel);
    driver->config = map_device(driver, driver->config_resource,
                                driver->device.config, PCI_CONFIG_SIZE,
                                "config map");
    command = driver->config + PCI_COMMAND;
    mmio_write16(command, (UINT16)(mmio_read16(command) | PCI_COMMAND_MEMORY |
                                   PCI_COMMAND_INTX_DISABLE));
    if (!(mmio_read16(driver->config + PCI_STATUS) & PCI_STATUS_CAPABILITIES))
        virtio_driver_fail(driver, "capability list", ERR_NOT_SUPPORTED);
    virtio_pci_capabilities(driver->config, structures, &driver->msix);
    if (!structures[VIRTIO_CAP_COMMON] || !structures[VIRTIO_CAP_NOTIFY] ||
        !structures[VIRTIO_CAP_DEVICE])
        virtio_driver_fail(driver, "structure capability", ERR_NOT_SUPPORTED);
    driver->common =
        structure(driver, structures[VIRTIO_CAP_COMMON], 0, VIRTIO_COMMON_SIZE);
    driver->device_config = structure(driver, structures[VIRTIO_CAP_DEVICE],
                                      0, device_config_size);
    virtio_driver_reset(driver);
    mmio_write16(command,
                 (UINT16)(mmio_read16(command) | PCI_COMMAND_BUS_MASTER));
}

/* ------------------------------------------------------------------------
   The device. */

void virtio_driver_set_status(VIRTIO_DRIVER *driver, UINT8 bits)
{
    volatile UINT8 *status = driver->common + VIRTIO_COMMON_DEVICE_STATUS;

    mmio_write8(status, (UINT8)(mmio_read8(status) | bits));
}

void virtio_driver_reset(VIRTIO_DRIVER *driver)
{
    if (!virtio_pci_reset(driver->common))
        virtio_driver_fail(driver, "reset", ERR_TIMED_OUT);
}

UINT8 virtio_driver_generation(const VIRTIO_DRIVER *driver)
{
    return mmio_read8(driver->common + VIRTIO_COMMON_CONFIG_GENERATION);
}

static UINT64 device_features(const VIRTIO_DRIVER *driver)
{
    volatile UINT8 *common = driver->common;
    UINT64 features;

    mmio_write32(common + VIRTIO_COMMON_DEVICE_FEATURE_SELECT, 0);
    features = mmio_read32(common + VIRTIO_COMMON_DEVICE_FEATURE);
    mmio_write32(common + VIRTIO_COMMON_DEVICE_FEATURE_SELECT, 1);
    features |= (UINT64)mmio_read32(common + VIRTIO_COMMON_DEVICE_FEATURE)
                << FEATURE_WORD_BITS;
    return features;
}

static void accept_features(const VIRTIO_DRIVER *driver, UINT64 features)
{
    volatile UINT8 *common = driver->common;

    mmio_write32(common + VIRTIO_COMMON_DRIVER_FEATURE_SELECT, 0);
    mmio_write32(common + VIRTIO_COMMON_DRIVER_FEATURE, (UINT32)features);
    mmio_write32(common + VIRTIO_COMMON_DRIVER_FEATURE_SELECT, 1);
    mmio_write32(common + VIRTIO_COMMON_DRIVER_FEATURE,
                 (UINT32)(features >> FEATURE_WORD_BITS));
}

UINT64 virtio_driver_negotiate(VIRTIO_DRIVER *driver, UINT64 accepted)
{
    UINT64 offered;

    virtio_driver_set_status(driver, VIRTIO_STATUS_ACKNOWLEDGE);
    virtio_driver_set_status(driver, VIRTIO_STATUS_DRIVER);
    offered = device_features(driver);
    if (!(offered & 1ULL << VIRTIO_F_VERSION_1))
        virtio_driver_fail(driver, "version", ERR_NOT_SUPPORTED);
    accepted &= offered;
    accept_features(driver, accepted);
    virtio_driver_set_status(driver, VIRTIO_STATUS_FEATURES_OK);
    if (!(mmio_read8(driver->common + VIRTIO_COMMON_DEVICE_STATUS) &
          VIRTIO_STATUS_FEATURES_OK)) {
        virtio_driver_set_status(driver, VIRTIO_STATUS_FAILED);
        virtio_driver_fail(driver, "feature negotiation", ERR_NOT_SUPPORTED);
    }
    return accepted;
}

/* ------------------------------------------------------------------------
   The interrupt, DMA memory and queues. */

/* The queues' MSI-X table entry gets the message the kernel allocated
   for the function, with the function masked while the entry changes;
   then the function is unmasked. */
static void enable_msix(VIRTIO_DRIVER *driver, UINT64 address, UINT32 data)
{
    const PCI_DEVICE_BOOTSTRAP *device = &driver->device;
    volatile UINT8 *control = driver->config + driver->msix + MSIX_CONTROL;
    UINT32 table = mmio_read32(driver->config + driver->msix + MSIX_TABLE);
    UINT32 index = table & MSIX_TABLE_BIR;
    UINT32 offset = table & MSIX_TABLE_OFFSET;
    volatile UINT8 *entry;

    if (index >= PCI_BARS || offset > device->bar_size[index] ||
        (VIRTIO_QUEUE_VECTOR + 1) * MSIX_ENTRY_SIZE >
            device->bar_size[index] - offset)
        virtio_driver_fail(driver, "msix table", ERR_OUT_OF_RANGE);
    entry =
        bar(driver, index) + offset + VIRTIO_QUEUE_VECTOR * MSIX_ENTRY_SIZE;
    mmio_write16(control, (UINT16)(mmio_read16(control) |
                                   MSIX_CONTROL_ENABLE |
                                   MSIX_CONTROL_MASK_ALL));
    mmio_write64(entry + MSIX_ENTRY_ADDRESS, address);
    mmio_write32(entry + MSIX_ENTRY_DATA, data);
    mmio_write32(entry + MSIX_ENTRY_CONTROL, 0);
    mmio_write16(control,
                 (UINT16)(mmio_read16(control) & ~MSIX_CONTROL_MASK_ALL));
}

void virtio_driver_interrupt(VIRTIO_DRIVER *driver, UINT64 key)
{
    UINT64 address;
    UINT32 data;

    if (!driver->msix)
        virtio_driver_fail(driver, "msix capability", ERR_NOT_SUPPORTED);
    virtio_driver_check(driver, port_create(PORT_CAPACITY, &driver->port),
                        "port create");
    virtio_driver_check(
        driver,
        interrupt_create(driver->irq,
                         INTERRUPT_SOURCE_MSI + driver->device.requester, 0,
                         &driver->interrupt, &address, &data),
        "interrupt create");
    virtio_driver_check(driver,
                        interrupt_bind(driver->interrupt, driver->port, key),
                        "interrupt bind");
    enable_msix(driver, address, data);
}

volatile UINT8 *virtio_driver_dma(VIRTIO_DRIVER *driver, UINT32 pages,
                                  UINT64 *addresses, UINT32 *pin)
{
    volatile UINT8 *mapped;
    UINT32 section;

    virtio_driver_check(driver,
                        section_create((UINT64)pages * PAGE_SIZE, 0, &section),
                        "section create");
    mapped = virtio_driver_map(driver, section, (UINT64)pages * PAGE_SIZE,
                               "dma map");
    virtio_driver_check(driver,
                        section_pin(driver->initiator, section, 0,
                                    (UINT64)pages * PAGE_SIZE,
                                    PIN_DEVICE_READ | PIN_DEVICE_WRITE,
                                    addresses, pin),
                        "section pin");
    handle_close(section);
    return mapped;
}

volatile UINT8 *virtio_driver_queue(VIRTIO_DRIVER *driver, UINT16 index,
                                    UINT16 size, UINT64 descriptors,
                                    UINT64 available, UINT64 used)
{
    volatile UINT8 *common = driver->common;
    UINT32 notify_cap = driver->structures[VIRTIO_CAP_NOTIFY];
    UINT32 multiplier =
        mmio_read32(driver->config + notify_cap + VIRTIO_CAP_NOTIFY_MULTIPLIER);
    UINT32 notify_off;
    UINT16 maximum;
    volatile UINT8 *notify;

    mmio_write16(common + VIRTIO_COMMON_QUEUE_SELECT, index);
    maximum = mmio_read16(common + VIRTIO_COMMON_QUEUE_SIZE);
    if (maximum < size)
        virtio_driver_fail(driver, "queue size", ERR_NOT_SUPPORTED);
    mmio_write16(common + VIRTIO_COMMON_QUEUE_SIZE, size);
    mmio_write64(common + VIRTIO_COMMON_QUEUE_DESC, descriptors);
    mmio_write64(common + VIRTIO_COMMON_QUEUE_DRIVER, available);
    mmio_write64(common + VIRTIO_COMMON_QUEUE_DEVICE, used);
    mmio_write16(common + VIRTIO_COMMON_QUEUE_MSIX_VECTOR,
                 VIRTIO_QUEUE_VECTOR);
    if (mmio_read16(common + VIRTIO_COMMON_QUEUE_MSIX_VECTOR) !=
        VIRTIO_QUEUE_VECTOR)
        virtio_driver_fail(driver, "queue vector", ERR_NOT_SUPPORTED);
    mmio_write16(common + VIRTIO_COMMON_CONFIG_MSIX_VECTOR, NO_VECTOR);
    notify_off = mmio_read16(common + VIRTIO_COMMON_QUEUE_NOTIFY_OFF);
    notify = structure(driver, notify_cap, (UINT64)notify_off * multiplier,
                       NOTIFY_WIDTH);
    mmio_write16(common + VIRTIO_COMMON_QUEUE_ENABLE, 1);
    return notify;
}

void virtio_driver_publish(volatile UINT8 *available,
                           const volatile UINT8 *used,
                           volatile UINT8 *notify, UINT16 queue,
                           UINT16 index)
{
    store_fence();
    *(volatile UINT16 *)(available + VIRTQ_RING_INDEX) = index;
    memory_fence();
    if (!(*(const volatile UINT16 *)(used + VIRTQ_RING_FLAGS) &
          VIRTQ_USED_F_NO_NOTIFY))
        mmio_write16(notify, queue);
}
