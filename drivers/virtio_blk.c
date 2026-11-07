/* A user-mode virtio block driver (VIRTIO 1.4 5.2) over the PCI
   transport's modern interface (4.1). It starts from the platform
   driver's bootstrap message (pci.h); maps its function's configuration
   space and the BARs the virtio structures lie in (4.1.4); sets the
   device up in the order 3.1.1 gives, with one request virtqueue (2.7)
   in a pinned page and its used-buffer notifications on MSI-X table
   entry 0 (4.1.5.1.2), delivered as packets on a port; and reads
   sectors: the GPT header at LBA 1, the partition entry array it names,
   and the first sector of the first partition, the EFI System Partition
   (docs/spec/disk-layout.md). On a disk without a GPT it reads the MBR's
   first partition instead. It logs what it read, resets the device,
   releases its pins and exits. */

#include "virtio_blk.h"
#include "log.h"
#include "mmio.h"
#include "native.h"
#include "pci.h"

/* The virtio structure capability (4.1.4): cfg_type, bar, offset and
   length; the notification capability adds notify_off_multiplier
   (4.1.4.4). */
#define CAP_CFG_TYPE 3
#define CAP_BAR 4
#define CAP_OFFSET 8
#define CAP_LENGTH 12
#define CAP_NOTIFY_MULTIPLIER 16
#define CAP_COMMON 1
#define CAP_NOTIFY 2
#define CAP_DEVICE 4
#define CAP_TYPES 5

/* struct virtio_pci_common_cfg (4.1.4.3). */
#define COMMON_DEVICE_FEATURE_SELECT 0
#define COMMON_DEVICE_FEATURE 4
#define COMMON_DRIVER_FEATURE_SELECT 8
#define COMMON_DRIVER_FEATURE 12
#define COMMON_CONFIG_MSIX_VECTOR 16
#define COMMON_DEVICE_STATUS 20
#define COMMON_CONFIG_GENERATION 21
#define COMMON_QUEUE_SELECT 22
#define COMMON_QUEUE_SIZE 24
#define COMMON_QUEUE_MSIX_VECTOR 26
#define COMMON_QUEUE_ENABLE 28
#define COMMON_QUEUE_NOTIFY_OFF 30
#define COMMON_QUEUE_DESC 32
#define COMMON_QUEUE_DRIVER 40
#define COMMON_QUEUE_DEVICE 48
#define COMMON_SIZE 56

/* Device status bits (2.1). */
#define STATUS_ACKNOWLEDGE 1
#define STATUS_DRIVER 2
#define STATUS_DRIVER_OK 4
#define STATUS_FEATURES_OK 8
#define STATUS_FAILED 128

/* Feature bits, as bit numbers in the 64 the driver negotiates: the block
   device's RO (5.2.3) and the reserved VERSION_1, ACCESS_PLATFORM and
   ORDER_PLATFORM (6). The driver accepts VERSION_1, which it requires;
   ACCESS_PLATFORM, since the addresses it gives the device are those
   section_pin returns; ORDER_PLATFORM, since its barriers are the
   processor's fences, which suit hardware devices (6.1); and RO. It
   accepts nothing else. */
#define FEATURE_RO 5
#define FEATURE_VERSION_1 32
#define FEATURE_ACCESS_PLATFORM 33
#define FEATURE_ORDER_PLATFORM 36
#define FEATURE_WORD_BITS 32
#define FEATURES_ACCEPTED                                                \
    (1ULL << FEATURE_RO | 1ULL << FEATURE_VERSION_1 |                    \
     1ULL << FEATURE_ACCESS_PLATFORM | 1ULL << FEATURE_ORDER_PLATFORM)

/* The MSI-X table entry the queue uses, and the value that leaves an
   event without a vector (4.1.5.1.2). */
#define QUEUE_VECTOR 0
#define NO_VECTOR 0xffff

/* The request queue, 0, at QUEUE_SIZE entries, a power of 2 no larger
   than the device's maximum (4.1.5.1.3). Its three parts share the first
   DMA page at their alignments (2.7): the descriptor table at 0 (16
   bytes each, 16-aligned), the available ring at 256 (2-aligned) and
   the used ring at 512 (4-aligned). */
#define REQUEST_QUEUE 0
#define QUEUE_SIZE 16
#define DESCRIPTOR_SIZE 16
#define AVAIL_OFFSET 256
#define USED_OFFSET 512
#define DESC_ADDRESS 0
#define DESC_LENGTH 8
#define DESC_FLAGS 12
#define DESC_NEXT 14
#define DESC_F_NEXT 1
#define DESC_F_WRITE 2
#define RING_FLAGS 0
#define RING_INDEX 2
#define RING_ENTRIES 4
#define USED_ELEMENT_SIZE 8
#define USED_F_NO_NOTIFY 1

/* A read request (5.2.6) in the second DMA page: the device-readable
   header (type, reserved, sector) at 0, the device-writable data at
   DATA_OFFSET and the status byte at STATUS_OFFSET, as three chained
   descriptors, readable before writable (2.7.4.2). */
#define REQUEST_TYPE 0
#define REQUEST_RESERVED 4
#define REQUEST_SECTOR 8
#define REQUEST_HEADER_SIZE 16
#define REQUEST_TYPE_IN 0
#define DATA_OFFSET 512
#define STATUS_OFFSET 1024
#define REQUEST_STATUS_OK 0
#define REQUEST_STATUS_UNSET 0xff
#define SECTOR_SIZE 512
#define DMA_PAGES 2

/* struct virtio_blk_config: capacity in 512-byte sectors (5.2.4). */
#define CONFIG_CAPACITY 0

/* How long a request may take before the driver gives up. */
#define REQUEST_TIMEOUT_NS 5000000000ULL

/* Where the driver maps its sections: from MAP_BASE up, each mapping at
   the next free address, far above its image and below its stack. */
#define MAP_BASE 0x0000000200000000ULL
#define RESET_POLLS 1000000
#define PORT_CAPACITY 1
#define QUEUE_KEY 1

/* The GPT header (UEFI 5.3.2) and partition entry (5.3.3), the protective
   or legacy MBR (5.2.1), and the FAT boot sector (fatgen103: BS_OEMName
   at 3, BS_FilSysType at 82 for FAT32 and 54 for FAT12 and FAT16, and
   the signature at 510). */
#define GPT_HEADER_LBA 1
#define GPT_SIGNATURE "EFI PART"
#define GPT_SIGNATURE_SIZE 8
#define GPT_REVISION 8
#define GPT_ENTRIES_LBA 72
#define GPT_ENTRY_TYPE 0
#define GPT_ENTRY_TYPE_SIZE 16
#define GPT_ENTRY_START 32
#define GPT_ENTRY_END 40
#define MBR_LBA 0
#define MBR_FIRST_PARTITION 446
#define MBR_PARTITION_TYPE 4
#define MBR_PARTITION_START 8
#define BOOT_SIGNATURE 510
#define BOOT_SIGNATURE_FIRST 0x55
#define BOOT_SIGNATURE_SECOND 0xaa
#define FAT_OEM_NAME 3
#define FAT_OEM_NAME_SIZE 8
#define FAT32_TYPE 82
#define FAT16_TYPE 54
#define FAT_TYPE_SIZE 8
#define FAT_TYPE_PREFIX "FAT"
#define FAT_TYPE_PREFIX_SIZE 3

/* The notification address must leave room for the 16-bit value written
   there (4.1.4.4.1). */
#define NOTIFY_WIDTH 2

typedef struct {
    UINT32 debug_log, self, initiator, irq, config_resource;
    UINT32 bar_resource[PCI_BARS];
    PCI_DEVICE_BOOTSTRAP device;
} BOOTSTRAP;

typedef struct {
    volatile UINT8 *config;           /* the function's configuration space */
    volatile UINT8 *bars[PCI_BARS];   /* the BARs mapped, or 0 */
    volatile UINT8 *common;
    volatile UINT8 *device_config;
    volatile UINT8 *notify;           /* the request queue's notify address */
    volatile UINT8 *queue;            /* the first DMA page */
    volatile UINT8 *request;          /* the second */
    UINT64 queue_device;              /* their device addresses */
    UINT64 request_device;
    UINT64 capacity;
    UINT32 port, interrupt, pin;
    UINT16 avail_index, used_seen;
    UINT32 reads, interrupts;
} DEVICE;

static BOOTSTRAP boot;
static DEVICE device;
static UINT64 next_map = MAP_BASE;

_Noreturn static void fail(const char *what, UINT32 status)
{
    say("nova-virtio-blk: ");
    say(what);
    say(", status ");
    say_number(status);
    say("\r\n");
    process_exit(VIRTIO_BLK_EXIT_FAILED);
}

static void check(UINT32 status, const char *what)
{
    if (status != OK)
        fail(what, status);
}

static UINT32 read32_le(const volatile UINT8 *p)
{
    return (UINT32)p[0] | (UINT32)p[1] << 8 | (UINT32)p[2] << 16 |
           (UINT32)p[3] << 24;
}

static UINT64 read64_le(const volatile UINT8 *p)
{
    return read32_le(p) | (UINT64)read32_le(p + 4) << 32;
}

static int same_text(const volatile UINT8 *bytes, const char *text,
                     UINT32 count)
{
    UINT32 i;

    for (i = 0; i < count; i++)
        if (bytes[i] != (UINT8)text[i])
            return 0;
    return 1;
}

/* ------------------------------------------------------------------------
   Bootstrap and mappings. */

static void read_bootstrap(UINT32 channel)
{
    RECEIVED_HANDLE handles[PCI_HANDLES_MAX];
    UINT32 byte_count, handle_count, i, next = PCI_HANDLE_BARS;

    if (channel_read(channel, &boot.device, sizeof boot.device, handles,
                     PCI_HANDLES_MAX, &byte_count, &handle_count) != OK ||
        byte_count != sizeof boot.device || handle_count < PCI_HANDLE_BARS)
        process_exit(VIRTIO_BLK_EXIT_FAILED);
    boot.debug_log = handles[PCI_HANDLE_DEBUG_LOG].handle;
    say_open(boot.debug_log);
    boot.self = handles[PCI_HANDLE_SELF].handle;
    boot.initiator = handles[PCI_HANDLE_INITIATOR].handle;
    boot.irq = handles[PCI_HANDLE_IRQ].handle;
    boot.config_resource = handles[PCI_HANDLE_CONFIG].handle;
    for (i = 0; i < PCI_BARS; i++) {
        boot.bar_resource[i] = HANDLE_INVALID;
        if (boot.device.bar_size[i] && next < handle_count)
            boot.bar_resource[i] = handles[next++].handle;
    }
    handle_close(channel);
}

/* Map size bytes of section, from its start, read-write at the next free
   address in the driver, and return where. */
static volatile UINT8 *map(UINT32 section, UINT64 size, const char *what)
{
    UINT64 address = next_map;

    check(process_map(boot.self, section, 0, address, size,
                      MAP_READ | MAP_WRITE),
          what);
    next_map += size;
    return (volatile UINT8 *)address;
}

/* Map size bytes of device memory at base, covered by resource, uncached
   into the driver, and return where. */
static volatile UINT8 *map_device(UINT32 resource, UINT64 base, UINT64 size,
                                  const char *what)
{
    volatile UINT8 *address;
    UINT32 section;

    check(section_create_physical(resource, base, size, CACHE_UNCACHED,
                                  &section),
          what);
    address = map(section, size, what);
    handle_close(section);
    return address;
}

static volatile UINT8 *bar(UINT32 index)
{
    if (index >= PCI_BARS || !boot.device.bar_size[index])
        fail("a virtio structure names a BAR with no memory",
             ERR_NOT_SUPPORTED);
    if (!device.bars[index])
        device.bars[index] =
            map_device(boot.bar_resource[index], boot.device.bar_base[index],
                       boot.device.bar_size[index], "cannot map a BAR");
    return device.bars[index];
}

/* The first virtio structure capability of each type, and the MSI-X
   capability, from the capability list (4.1.4.1). The list is walked at
   most PCI_CAPS_MAX steps, so a looping list ends. */
static void find_capabilities(UINT32 *vendor, UINT32 *msix)
{
    UINT32 at, steps, type;

    for (type = 0; type < CAP_TYPES; type++)
        vendor[type] = 0;
    *msix = 0;
    if (!(mmio_read16(device.config + PCI_STATUS) & PCI_STATUS_CAPABILITIES))
        fail("the function has no capability list", ERR_NOT_SUPPORTED);
    at = mmio_read8(device.config + PCI_CAPABILITIES) & PCI_CAP_POINTER_MASK;
    for (steps = 0; at && steps < PCI_CAPS_MAX; steps++) {
        UINT32 id = mmio_read8(device.config + at + PCI_CAP_ID);

        if (id == PCI_CAP_MSIX && !*msix)
            *msix = at;
        if (id == PCI_CAP_VENDOR) {
            type = mmio_read8(device.config + at + CAP_CFG_TYPE);
            if (type < CAP_TYPES && !vendor[type])
                vendor[type] = at;
        }
        at = mmio_read8(device.config + at + PCI_CAP_NEXT) &
             PCI_CAP_POINTER_MASK;
    }
}

/* Where in the driver the structure capability cap describes lies, from
   at bytes into it on, checked to hold size bytes from there within the
   structure and within its BAR. */
static volatile UINT8 *structure(UINT32 cap, UINT64 at, UINT64 size)
{
    UINT32 index = mmio_read8(device.config + cap + CAP_BAR);
    UINT64 offset = mmio_read32(device.config + cap + CAP_OFFSET);
    UINT64 length = mmio_read32(device.config + cap + CAP_LENGTH);

    if (at > length || size > length - at || index >= PCI_BARS ||
        offset + at > boot.device.bar_size[index] ||
        size > boot.device.bar_size[index] - offset - at)
        fail("a virtio structure lies outside its BAR", ERR_OUT_OF_RANGE);
    return bar(index) + offset + at;
}

/* ------------------------------------------------------------------------
   The device. */

static void set_status(UINT8 bits)
{
    mmio_write8(device.common + COMMON_DEVICE_STATUS,
                (UINT8)(mmio_read8(device.common + COMMON_DEVICE_STATUS) |
                        bits));
}

/* Writing 0 resets the device, which is done once a read gives 0 again
   (4.1.4.3.2, 2.4.2). */
static void reset(void)
{
    UINT32 polls;

    mmio_write8(device.common + COMMON_DEVICE_STATUS, 0);
    for (polls = 0; polls < RESET_POLLS; polls++)
        if (mmio_read8(device.common + COMMON_DEVICE_STATUS) == 0)
            return;
    fail("the device did not finish its reset", ERR_TIMED_OUT);
}

static UINT64 device_features(void)
{
    UINT64 features;

    mmio_write32(device.common + COMMON_DEVICE_FEATURE_SELECT, 0);
    features = mmio_read32(device.common + COMMON_DEVICE_FEATURE);
    mmio_write32(device.common + COMMON_DEVICE_FEATURE_SELECT, 1);
    features |= (UINT64)mmio_read32(device.common + COMMON_DEVICE_FEATURE)
                << FEATURE_WORD_BITS;
    return features;
}

static void accept_features(UINT64 features)
{
    mmio_write32(device.common + COMMON_DRIVER_FEATURE_SELECT, 0);
    mmio_write32(device.common + COMMON_DRIVER_FEATURE, (UINT32)features);
    mmio_write32(device.common + COMMON_DRIVER_FEATURE_SELECT, 1);
    mmio_write32(device.common + COMMON_DRIVER_FEATURE,
                 (UINT32)(features >> FEATURE_WORD_BITS));
}

/* The capacity, read as two 32-bit halves until the configuration
   generation stays the same across them (2.5.1, 4.1.3.1). */
static UINT64 read_capacity(void)
{
    UINT32 before, after, low, high;

    do {
        before = mmio_read8(device.common + COMMON_CONFIG_GENERATION);
        low = mmio_read32(device.device_config + CONFIG_CAPACITY);
        high = mmio_read32(device.device_config + CONFIG_CAPACITY + 4);
        after = mmio_read8(device.common + COMMON_CONFIG_GENERATION);
    } while (before != after);
    return (UINT64)high << FEATURE_WORD_BITS | low;
}

/* MSI-X table entry 0 gets the message the kernel allocated for the
   function, with the function masked while the entry changes; then the
   function is unmasked. */
static void enable_msix(UINT32 msix, UINT64 address, UINT32 data)
{
    volatile UINT8 *control = device.config + msix + MSIX_CONTROL;
    UINT32 table = mmio_read32(device.config + msix + MSIX_TABLE);
    UINT32 index = table & MSIX_TABLE_BIR;
    UINT32 offset = table & MSIX_TABLE_OFFSET;
    volatile UINT8 *entry;

    if (index >= PCI_BARS || offset > boot.device.bar_size[index] ||
        MSIX_ENTRY_SIZE > boot.device.bar_size[index] - offset)
        fail("the MSI-X table lies outside its BAR", ERR_OUT_OF_RANGE);
    entry = bar(index) + offset + QUEUE_VECTOR * MSIX_ENTRY_SIZE;
    mmio_write16(control, (UINT16)(mmio_read16(control) |
                                   MSIX_CONTROL_ENABLE |
                                   MSIX_CONTROL_MASK_ALL));
    mmio_write64(entry + MSIX_ENTRY_ADDRESS, address);
    mmio_write32(entry + MSIX_ENTRY_DATA, data);
    mmio_write32(entry + MSIX_ENTRY_CONTROL, 0);
    mmio_write16(control,
                 (UINT16)(mmio_read16(control) & ~MSIX_CONTROL_MASK_ALL));
}

static void set_up_interrupt(UINT32 msix)
{
    UINT64 address;
    UINT32 data;

    if (!msix)
        fail("the function has no MSI-X capability", ERR_NOT_SUPPORTED);
    check(port_create(PORT_CAPACITY, &device.port), "port_create");
    check(interrupt_create(boot.irq,
                           INTERRUPT_SOURCE_MSI + boot.device.requester, 0,
                           &device.interrupt, &address, &data),
          "interrupt_create");
    check(interrupt_bind(device.interrupt, device.port, QUEUE_KEY),
          "interrupt_bind");
    enable_msix(msix, address, data);
}

/* Two pages of DMA memory, both pinned for a device that reads and
   writes them: the queue, and the request. */
static void set_up_dma(void)
{
    UINT64 addresses[DMA_PAGES];
    volatile UINT8 *mapped;
    UINT32 section;

    check(section_create(DMA_PAGES * PAGE_SIZE, 0, &section),
          "section_create");
    mapped = map(section, DMA_PAGES * PAGE_SIZE, "mapping the DMA pages");
    check(section_pin(boot.initiator, section, 0, DMA_PAGES * PAGE_SIZE,
                      PIN_DEVICE_READ | PIN_DEVICE_WRITE, addresses,
                      &device.pin),
          "section_pin");
    handle_close(section);
    device.queue = mapped;
    device.request = mapped + PAGE_SIZE;
    device.queue_device = addresses[0];
    device.request_device = addresses[1];
}

/* 4.1.5.1.3: select the queue, size it, give the device its parts'
   addresses as two 32-bit halves each (4.1.3.1) and its MSI-X entry,
   which the device confirms by reading it back (4.1.5.1.2.2), then
   enable it. */
static void set_up_queue(UINT32 notify_cap)
{
    volatile UINT8 *common = device.common;
    UINT32 multiplier =
        mmio_read32(device.config + notify_cap + CAP_NOTIFY_MULTIPLIER);
    UINT32 notify_off;
    UINT16 maximum;

    mmio_write16(common + COMMON_QUEUE_SELECT, REQUEST_QUEUE);
    maximum = mmio_read16(common + COMMON_QUEUE_SIZE);
    if (maximum < QUEUE_SIZE)
        fail("the request queue is smaller than the driver needs", maximum);
    mmio_write16(common + COMMON_QUEUE_SIZE, QUEUE_SIZE);
    mmio_write64(common + COMMON_QUEUE_DESC, device.queue_device);
    mmio_write64(common + COMMON_QUEUE_DRIVER,
                 device.queue_device + AVAIL_OFFSET);
    mmio_write64(common + COMMON_QUEUE_DEVICE,
                 device.queue_device + USED_OFFSET);
    mmio_write16(common + COMMON_QUEUE_MSIX_VECTOR, QUEUE_VECTOR);
    if (mmio_read16(common + COMMON_QUEUE_MSIX_VECTOR) != QUEUE_VECTOR)
        fail("the device refused the queue's MSI-X entry", ERR_NOT_SUPPORTED);
    mmio_write16(common + COMMON_CONFIG_MSIX_VECTOR, NO_VECTOR);
    notify_off = mmio_read16(common + COMMON_QUEUE_NOTIFY_OFF);
    device.notify = structure(notify_cap, (UINT64)notify_off * multiplier,
                              NOTIFY_WIDTH);
    mmio_write16(common + COMMON_QUEUE_ENABLE, 1);
}

/* 3.1.1, steps 1 to 8. Bus mastering comes on only after the reset, so a
   device a dead driver left running reaches no memory through its old
   rings. */
static void start_device(void)
{
    UINT32 vendor[CAP_TYPES], msix;
    UINT64 offered, accepted;

    find_capabilities(vendor, &msix);
    if (!vendor[CAP_COMMON] || !vendor[CAP_NOTIFY] || !vendor[CAP_DEVICE])
        fail("a virtio structure capability is missing", ERR_NOT_SUPPORTED);
    device.common = structure(vendor[CAP_COMMON], 0, COMMON_SIZE);
    device.device_config = structure(vendor[CAP_DEVICE], 0, sizeof(UINT64));
    reset();
    mmio_write16(device.config + PCI_COMMAND,
                 (UINT16)(mmio_read16(device.config + PCI_COMMAND) |
                          PCI_COMMAND_BUS_MASTER));
    set_up_interrupt(msix);
    set_status(STATUS_ACKNOWLEDGE);
    set_status(STATUS_DRIVER);
    offered = device_features();
    if (!(offered & 1ULL << FEATURE_VERSION_1))
        fail("the device offers no VERSION_1", ERR_NOT_SUPPORTED);
    accepted = offered & FEATURES_ACCEPTED;
    accept_features(accepted);
    set_status(STATUS_FEATURES_OK);
    if (!(mmio_read8(device.common + COMMON_DEVICE_STATUS) &
          STATUS_FEATURES_OK)) {
        set_status(STATUS_FAILED);
        fail("the device refused the features", ERR_NOT_SUPPORTED);
    }
    device.capacity = read_capacity();
    set_up_dma();
    set_up_queue(vendor[CAP_NOTIFY]);
    set_status(STATUS_DRIVER_OK);
    say("nova-virtio-blk: ");
    pci_say_function(boot.device.requester);
    say(" running: features ");
    say_hex(accepted);
    say(", capacity ");
    say_number(device.capacity);
    say(" sectors, one queue of ");
    say_number(QUEUE_SIZE);
    say(" on MSI-X\r\n");
}

/* ------------------------------------------------------------------------
   Requests. */

static volatile UINT8 *descriptor(UINT32 index)
{
    return device.queue + index * DESCRIPTOR_SIZE;
}

static void set_descriptor(UINT32 index, UINT64 address, UINT32 length,
                           UINT16 flags, UINT16 next)
{
    volatile UINT8 *d = descriptor(index);

    *(volatile UINT64 *)(d + DESC_ADDRESS) = address;
    *(volatile UINT32 *)(d + DESC_LENGTH) = length;
    *(volatile UINT16 *)(d + DESC_FLAGS) = flags;
    *(volatile UINT16 *)(d + DESC_NEXT) = next;
}

static UINT16 used_index(void)
{
    return *(volatile UINT16 *)(device.queue + USED_OFFSET + RING_INDEX);
}

/* Wait for the device's notification, an interrupt packet, and
   acknowledge it, until the used ring has passed the request. With the
   available ring's flags 0 the device must notify once it has used the
   buffer (2.7.7.2), so the driver waits for a packet before it looks; a
   notification that finds nothing new is spurious, and it waits on
   (2.7.7.1). */
static void wait_used(void)
{
    PORT_PACKET packet;
    UINT64 now;

    check(clock_monotonic(&now), "clock_monotonic");
    do {
        check(port_wait(device.port, now + REQUEST_TIMEOUT_NS, &packet),
              "no interrupt came for a request");
        if (packet.type != PACKET_INTERRUPT || packet.key != QUEUE_KEY)
            fail("an unexpected packet came", packet.type);
        device.interrupts++;
        check(interrupt_ack(device.interrupt), "interrupt_ack");
    } while (used_index() == device.used_seen);
    load_fence();
}

/* Read one sector, at lba, into the request page's data (5.2.6). The
   descriptors and the ring entry are stored before the fence and the
   index after it, so the device sees the chain before the index exposes
   it (2.7.13, step 4 and 2.7.13.3.1); the index is stored before the
   fence that comes ahead of reading the device's flags (step 6,
   2.7.13.4.1); the notification is a write of the queue's index to its
   notify address (4.1.5.2). */
static const volatile UINT8 *read_sector(UINT64 lba)
{
    volatile UINT8 *avail = device.queue + AVAIL_OFFSET;
    volatile UINT8 *used = device.queue + USED_OFFSET;
    volatile UINT8 *element;
    UINT32 slot = device.avail_index % QUEUE_SIZE;

    if (lba >= device.capacity)
        fail("a read past the device's capacity", ERR_OUT_OF_RANGE);
    *(volatile UINT32 *)(device.request + REQUEST_TYPE) = REQUEST_TYPE_IN;
    *(volatile UINT32 *)(device.request + REQUEST_RESERVED) = 0;
    *(volatile UINT64 *)(device.request + REQUEST_SECTOR) = lba;
    device.request[STATUS_OFFSET] = REQUEST_STATUS_UNSET;
    set_descriptor(0, device.request_device, REQUEST_HEADER_SIZE, DESC_F_NEXT,
                   1);
    set_descriptor(1, device.request_device + DATA_OFFSET, SECTOR_SIZE,
                   DESC_F_WRITE | DESC_F_NEXT, 2);
    set_descriptor(2, device.request_device + STATUS_OFFSET, 1, DESC_F_WRITE,
                   0);
    *(volatile UINT16 *)(avail + RING_ENTRIES + slot * sizeof(UINT16)) = 0;
    store_fence();
    device.avail_index++;
    *(volatile UINT16 *)(avail + RING_INDEX) = device.avail_index;
    memory_fence();
    if (!(*(volatile UINT16 *)(used + RING_FLAGS) & USED_F_NO_NOTIFY))
        mmio_write16(device.notify, REQUEST_QUEUE);
    wait_used();
    element = used + RING_ENTRIES + device.used_seen % QUEUE_SIZE *
                                        USED_ELEMENT_SIZE;
    device.used_seen++;
    if (*(volatile UINT32 *)element != 0 ||
        device.request[STATUS_OFFSET] != REQUEST_STATUS_OK)
        fail("a read request failed", device.request[STATUS_OFFSET]);
    device.reads++;
    return device.request + DATA_OFFSET;
}

/* ------------------------------------------------------------------------
   What the disk holds. */

/* The longest text field the driver logs from a sector. */
#define QUOTED_MAX 8

/* count bytes of DMA memory, at most QUOTED_MAX, in quotes. */
static void say_quoted(const volatile UINT8 *bytes, UINT32 count)
{
    UINT8 copy[QUOTED_MAX];
    UINT32 i;

    for (i = 0; i < count && i < QUOTED_MAX; i++)
        copy[i] = bytes[i];
    say("\"");
    say_printable(copy, i);
    say("\"");
}

static void say_sector_text(UINT64 lba, const char *what,
                            const volatile UINT8 *bytes, UINT32 count)
{
    say("nova-virtio-blk: lba ");
    say_number(lba);
    say(": ");
    say(what);
    say(" ");
    say_quoted(bytes, count);
}

/* The first partition's first LBA, from the GPT if LBA 1 holds its
   header, else from the MBR; 0 if neither names one. */
static UINT64 first_partition(void)
{
    const volatile UINT8 *sector = read_sector(GPT_HEADER_LBA);
    UINT64 entries, start;
    UINT32 i;

    if (same_text(sector, GPT_SIGNATURE, GPT_SIGNATURE_SIZE)) {
        say_sector_text(GPT_HEADER_LBA, "GPT header, signature", sector,
                        GPT_SIGNATURE_SIZE);
        say(", revision ");
        say_hex(read32_le(sector + GPT_REVISION));
        entries = read64_le(sector + GPT_ENTRIES_LBA);
        say(", partition entries at lba ");
        say_number(entries);
        say("\r\n");
        sector = read_sector(entries);
        for (i = 0; i < GPT_ENTRY_TYPE_SIZE; i++)
            if (sector[GPT_ENTRY_TYPE + i])
                break;
        if (i == GPT_ENTRY_TYPE_SIZE)
            return 0;
        start = read64_le(sector + GPT_ENTRY_START);
        say("nova-virtio-blk: lba ");
        say_number(entries);
        say(": first partition, lba ");
        say_number(start);
        say(" to ");
        say_number(read64_le(sector + GPT_ENTRY_END));
        say("\r\n");
        return start;
    }
    say_sector_text(GPT_HEADER_LBA, "no GPT header, it begins", sector,
                    GPT_SIGNATURE_SIZE);
    say("\r\n");
    sector = read_sector(MBR_LBA);
    if (sector[BOOT_SIGNATURE] != BOOT_SIGNATURE_FIRST ||
        sector[BOOT_SIGNATURE + 1] != BOOT_SIGNATURE_SECOND)
        return 0;
    start = read32_le(sector + MBR_FIRST_PARTITION + MBR_PARTITION_START);
    say("nova-virtio-blk: lba 0: MBR, first partition type ");
    say_hex(sector[MBR_FIRST_PARTITION + MBR_PARTITION_TYPE]);
    say(" at lba ");
    say_number(start);
    say("\r\n");
    return start;
}

/* Log the boot sector's OEM name, file system type and signature. */
static void say_boot_sector(UINT64 lba)
{
    const volatile UINT8 *sector = read_sector(lba);
    UINT32 type = same_text(sector + FAT32_TYPE, FAT_TYPE_PREFIX,
                            FAT_TYPE_PREFIX_SIZE)
                      ? FAT32_TYPE
                      : FAT16_TYPE;

    say_sector_text(lba, "boot sector, OEM name", sector + FAT_OEM_NAME,
                    FAT_OEM_NAME_SIZE);
    say(", file system type ");
    say_quoted(sector + type, FAT_TYPE_SIZE);
    say(", signature ");
    say_hex_width(sector[BOOT_SIGNATURE], 2);
    say(" ");
    say_hex_width(sector[BOOT_SIGNATURE + 1], 2);
    say("\r\n");
}

_Noreturn void virtio_blk_main(UINT32 bootstrap, UINT64 argument)
{
    UINT64 start;

    (void)argument;
    read_bootstrap(bootstrap);
    device.config = map_device(boot.config_resource, boot.device.config,
                               PCI_CONFIG_SIZE,
                               "cannot map the configuration space");
    mmio_write16(device.config + PCI_COMMAND,
                 (UINT16)(mmio_read16(device.config + PCI_COMMAND) |
                          PCI_COMMAND_MEMORY | PCI_COMMAND_INTX_DISABLE));
    start_device();
    start = first_partition();
    if (!start)
        fail("the disk names no partition", ERR_NOT_SUPPORTED);
    say_boot_sector(start);
    reset();
    check(pin_release(device.pin), "pin_release");
    say("nova-virtio-blk: ");
    say_number(device.reads);
    say(" sectors read, ");
    say_number(device.interrupts);
    say(" MSI-X interrupts taken; device reset, pin released\r\n");
    process_exit(VIRTIO_BLK_EXIT_DONE);
}
