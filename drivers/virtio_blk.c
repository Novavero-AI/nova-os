/* A user-mode virtio block driver (VIRTIO 1.4 5.2) over the PCI
   transport's modern interface (4.1). It starts as every virtio driver
   here does (virtio_driver.h), with one request virtqueue (2.7) in a
   pinned page, and reads sectors: the GPT header at LBA 1, the
   partition entry array it names, and the first sector of the first
   partition, the EFI System Partition (docs/spec/disk-layout.md). On a
   disk without a GPT it reads the MBR's first partition instead. It
   logs what it read, resets the device, releases its pins and exits. */

#include "virtio_blk.h"
#include "log.h"
#include "mmio.h"
#include "virtio_driver.h"

/* The block device's RO feature bit (5.2.3), which the driver accepts
   beside the reserved ones (virtio_driver.h); it accepts nothing
   else. */
#define FEATURE_RO 5
#define FEATURES_ACCEPTED (1ULL << FEATURE_RO | VIRTIO_FEATURES_RESERVED)

/* The request queue, 0, at QUEUE_SIZE entries, a power of 2 no larger
   than the device's maximum (4.1.5.1.3). Its three parts share the first
   DMA page at their alignments (2.7): the descriptor table at 0 (16
   bytes each, 16-aligned), the available ring at 256 (2-aligned) and
   the used ring at 512 (4-aligned). */
#define REQUEST_QUEUE 0
#define QUEUE_SIZE 16
#define AVAIL_OFFSET 256
#define USED_OFFSET 512

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
#define CONFIG_SIZE 8
#define CAPACITY_HALF_BITS 32

/* How long a request may take before the driver gives up. */
#define REQUEST_TIMEOUT_NS 5000000000ULL

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

typedef struct {
    VIRTIO_DRIVER virtio;
    volatile UINT8 *notify;  /* the request queue's notify address */
    volatile UINT8 *queue;   /* the first DMA page */
    volatile UINT8 *request; /* the second */
    UINT64 request_device;   /* its device address */
    UINT64 capacity;
    UINT32 pin;
    UINT16 avail_index, used_seen;
    UINT32 reads, interrupts;
} DEVICE;

static DEVICE device;

_Noreturn static void fail(const char *what, UINT32 status)
{
    virtio_driver_fail(&device.virtio, what, status);
}

static void check(UINT32 status, const char *what)
{
    virtio_driver_check(&device.virtio, status, what);
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
   The device. */

/* The capacity, read as two 32-bit halves until the configuration
   generation stays the same across them (2.5.1, 4.1.3.1). */
static UINT64 read_capacity(void)
{
    volatile UINT8 *config = device.virtio.device_config;
    UINT32 before, after, low, high;

    do {
        before = virtio_driver_generation(&device.virtio);
        low = mmio_read32(config + CONFIG_CAPACITY);
        high = mmio_read32(config + CONFIG_CAPACITY + 4);
        after = virtio_driver_generation(&device.virtio);
    } while (before != after);
    return (UINT64)high << CAPACITY_HALF_BITS | low;
}

/* 3.1.1: the reset and bus mastering come with virtio_driver_open; then
   the interrupt, the features, the capacity, two pages of DMA memory,
   the request queue in the first, and DRIVER_OK. */
static void start_device(UINT32 bootstrap)
{
    VIRTIO_DRIVER *virtio = &device.virtio;
    UINT64 accepted, addresses[DMA_PAGES];

    virtio_driver_open(virtio, "nova-virtio-blk", VIRTIO_BLK_EXIT_FAILED,
                       bootstrap, CONFIG_SIZE);
    handle_close(bootstrap);
    virtio_driver_interrupt(virtio, QUEUE_KEY);
    accepted = virtio_driver_negotiate(virtio, FEATURES_ACCEPTED);
    device.capacity = read_capacity();
    device.queue = virtio_driver_dma(virtio, DMA_PAGES, addresses,
                                     &device.pin);
    device.request = device.queue + PAGE_SIZE;
    device.request_device = addresses[1];
    device.notify =
        virtio_driver_queue(virtio, REQUEST_QUEUE, QUEUE_SIZE, addresses[0],
                            addresses[0] + AVAIL_OFFSET,
                            addresses[0] + USED_OFFSET);
    virtio_driver_set_status(virtio, VIRTIO_STATUS_DRIVER_OK);
    say("nova-virtio-blk: ");
    pci_say_function(virtio->device.requester);
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

static void set_descriptor(UINT32 index, UINT64 address, UINT32 length,
                           UINT16 flags, UINT16 next)
{
    volatile UINT8 *d = device.queue + index * VIRTQ_DESCRIPTOR_SIZE;

    *(volatile UINT64 *)(d + VIRTQ_DESC_ADDRESS) = address;
    *(volatile UINT32 *)(d + VIRTQ_DESC_LENGTH) = length;
    *(volatile UINT16 *)(d + VIRTQ_DESC_FLAGS) = flags;
    *(volatile UINT16 *)(d + VIRTQ_DESC_NEXT) = next;
}

static UINT16 used_index(void)
{
    return *(volatile UINT16 *)(device.queue + USED_OFFSET +
                                VIRTQ_RING_INDEX);
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
        check(port_wait(device.virtio.port, now + REQUEST_TIMEOUT_NS, &packet),
              "no interrupt came for a request");
        if (packet.type != PACKET_INTERRUPT || packet.key != QUEUE_KEY)
            fail("an unexpected packet came", packet.type);
        device.interrupts++;
        check(interrupt_ack(device.virtio.interrupt), "interrupt_ack");
    } while (used_index() == device.used_seen);
    load_fence();
}

/* Read one sector, at lba, into the request page's data (5.2.6), as a
   chain of the three descriptors from 0 that the ring's next entry
   names. */
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
    set_descriptor(0, device.request_device, REQUEST_HEADER_SIZE,
                   VIRTQ_DESC_F_NEXT, 1);
    set_descriptor(1, device.request_device + DATA_OFFSET, SECTOR_SIZE,
                   VIRTQ_DESC_F_WRITE | VIRTQ_DESC_F_NEXT, 2);
    set_descriptor(2, device.request_device + STATUS_OFFSET, 1,
                   VIRTQ_DESC_F_WRITE, 0);
    *(volatile UINT16 *)(avail + VIRTQ_RING_ENTRIES +
                         slot * VIRTQ_AVAIL_ENTRY_SIZE) = 0;
    device.avail_index++;
    virtio_driver_publish(avail, used, device.notify, REQUEST_QUEUE,
                          device.avail_index);
    wait_used();
    element = used + VIRTQ_RING_ENTRIES +
              device.used_seen % QUEUE_SIZE * VIRTQ_USED_ELEMENT_SIZE;
    device.used_seen++;
    if (*(volatile UINT32 *)(element + VIRTQ_USED_ID) != 0 ||
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
    start_device(bootstrap);
    start = first_partition();
    if (!start)
        fail("the disk names no partition", ERR_NOT_SUPPORTED);
    say_boot_sector(start);
    virtio_driver_reset(&device.virtio);
    check(pin_release(device.pin), "pin_release");
    say("nova-virtio-blk: ");
    say_number(device.reads);
    say(" sectors read, ");
    say_number(device.interrupts);
    say(" MSI-X interrupts taken; device reset, pin released\r\n");
    process_exit(VIRTIO_BLK_EXIT_DONE);
}
