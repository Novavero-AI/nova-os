/* A user-mode virtio block driver (VIRTIO 1.4 5.2) over the PCI
   transport's modern interface (4.1). It starts as every virtio driver
   here does (virtio_driver.h), with one request virtqueue (2.7) in a
   pinned page, and runs in one of two modes (virtio_blk.h).

   Inspecting, it reads sectors: the GPT header at LBA 1, the partition
   entry array it names, and the first sector of the first partition, the
   EFI System Partition (docs/spec/disk-layout.md). On a disk without a
   GPT it reads the MBR's first partition instead. It logs what it read,
   resets the device, releases its pins and exits.

   Serving, it reports with a client's endpoint and serves the block
   protocol there (block_protocol.h): it pins the section the client
   attaches and moves each read and write by DMA between the device and
   that section, and sends a cache flush for a flush. It has one request
   on the device at a time and answers each before it reads the next, so
   a request the client sends after an answer reaches the device after
   the request answered: a flush sent once the writes before it are
   answered makes them stable (5.2.6.2), and a write sent once that flush
   is answered lands after them. */

#include "virtio_blk.h"
#include "block_protocol.h"
#include "log.h"
#include "mmio.h"
#include "virtio_driver.h"

/* The block device's feature bits (5.2.3) the driver accepts beside the
   reserved ones (virtio_driver.h): RO in both modes, and FLUSH when
   serving, since it then sends VIRTIO_BLK_T_FLUSH (5.2.5.1). With FLUSH
   negotiated a write is volatile until a flush completes after it
   (5.2.6.2). */
#define FEATURE_RO 5
#define FEATURE_FLUSH 9
#define FEATURES_INSPECT (1ULL << FEATURE_RO | VIRTIO_FEATURES_RESERVED)
#define FEATURES_SERVE (FEATURES_INSPECT | 1ULL << FEATURE_FLUSH)

/* The request queue, 0, at QUEUE_SIZE entries, a power of 2 no larger
   than the device's maximum (4.1.5.1.3). Its three parts share the first
   DMA page at their alignments (2.7): the descriptor table at 0 (16
   bytes each, 16-aligned), the available ring at 256 (2-aligned) and
   the used ring at 512 (4-aligned). */
#define REQUEST_QUEUE 0
#define QUEUE_SIZE 16
#define AVAIL_OFFSET 256
#define USED_OFFSET 512

/* A request (5.2.6) is a chain of descriptors from 0, readable before
   writable (2.7.4.2): the device-readable header (type, reserved,
   sector) at 0 in the second DMA page, the data, and the device-writable
   status byte at STATUS_OFFSET in that page. Inspecting, the data is one
   sector at DATA_OFFSET in that page; serving, it lies in the client's
   section, one descriptor per page it touches. */
#define REQUEST_TYPE 0
#define REQUEST_RESERVED 4
#define REQUEST_SECTOR 8
#define REQUEST_HEADER_SIZE 16
#define REQUEST_TYPE_IN 0
#define REQUEST_TYPE_OUT 1
#define REQUEST_TYPE_FLUSH 4
#define DATA_OFFSET 512
#define STATUS_OFFSET 1024
#define REQUEST_STATUS_OK 0
#define REQUEST_STATUS_UNSUPPORTED 2
#define REQUEST_STATUS_UNSET 0xff
#define SECTOR_SIZE 512
#define DMA_PAGES 2

/* The most data descriptors one request takes: a transfer of
   BLOCK_TRANSFER_MAX bytes that does not start on a page boundary
   touches one page more than it fills. */
#define SEGMENTS_MAX (BLOCK_TRANSFER_MAX / PAGE_SIZE + 1)
#define SECTION_PAGES_MAX (SECTION_MAX_SIZE / PAGE_SIZE)

_Static_assert(SEGMENTS_MAX + 2 <= QUEUE_SIZE,
               "a request's chain fits the queue");
_Static_assert(SECTOR_SIZE == BLOCK_SECTOR_SIZE,
               "the protocol counts the device's sectors");

/* The client's endpoint queues one message each way: the client sends a
   request once the last is answered. */
#define CLIENT_CAPACITY 1

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
    UINT64 features;
    UINT32 pin;
    UINT16 avail_index, used_seen;
    UINT32 reads, interrupts;
    UINT32 client;          /* serving: the driver's end of the client's */
    UINT32 section_pin;     /* the attached section's, or HANDLE_INVALID */
    UINT64 section_size;
    UINT64 writes, flushes;
} DEVICE;

/* A run of bytes a descriptor names. */
typedef struct {
    UINT64 address;
    UINT32 length;
} SEGMENT;

static DEVICE device;

/* The attached section's pages' device addresses. */
static UINT64 section_pages[SECTION_PAGES_MAX];

/* A message from the client, read whole so that none stays queued. */
static UINT8 message[CHANNEL_MAX_BYTES];
static RECEIVED_HANDLE received[CHANNEL_MAX_HANDLES];

/* channel_write needs a valid array even for no handles. */
static const HANDLE_TRANSFER no_handles[1];

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
   the request queue in the first, and DRIVER_OK. The bootstrap channel
   stays open. */
static void start_device(UINT32 bootstrap, UINT64 features)
{
    VIRTIO_DRIVER *virtio = &device.virtio;
    UINT64 accepted, addresses[DMA_PAGES];

    device.section_pin = HANDLE_INVALID;
    virtio_driver_open(virtio, "nova-virtio-blk", VIRTIO_BLK_EXIT_FAILED,
                       bootstrap, CONFIG_SIZE);
    virtio_driver_interrupt(virtio, QUEUE_KEY);
    accepted = virtio_driver_negotiate(virtio, features);
    device.features = accepted;
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

/* Put a request of type for sector on the device with its data in the
   count segments, device-writable for a read, as a chain of descriptors
   from 0 that the ring's next entry names, and wait until the device has
   used it. Returns the status byte the device wrote. */
static UINT8 submit(UINT32 type, UINT64 sector, const SEGMENT *segments,
                    UINT32 count)
{
    volatile UINT8 *avail = device.queue + AVAIL_OFFSET;
    volatile UINT8 *used = device.queue + USED_OFFSET;
    volatile UINT8 *element;
    UINT32 slot = device.avail_index % QUEUE_SIZE, i;
    UINT16 data_flags = type == REQUEST_TYPE_IN ? VIRTQ_DESC_F_WRITE : 0;

    *(volatile UINT32 *)(device.request + REQUEST_TYPE) = type;
    *(volatile UINT32 *)(device.request + REQUEST_RESERVED) = 0;
    *(volatile UINT64 *)(device.request + REQUEST_SECTOR) = sector;
    device.request[STATUS_OFFSET] = REQUEST_STATUS_UNSET;
    set_descriptor(0, device.request_device, REQUEST_HEADER_SIZE,
                   VIRTQ_DESC_F_NEXT, 1);
    for (i = 0; i < count; i++)
        set_descriptor(1 + i, segments[i].address, segments[i].length,
                       data_flags | VIRTQ_DESC_F_NEXT, (UINT16)(2 + i));
    set_descriptor(1 + count, device.request_device + STATUS_OFFSET, 1,
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
    if (*(volatile UINT32 *)(element + VIRTQ_USED_ID) != 0)
        fail("the device used a chain it was not given",
             *(volatile UINT32 *)(element + VIRTQ_USED_ID));
    return device.request[STATUS_OFFSET];
}

/* Read one sector, at lba, into the request page's data. */
static const volatile UINT8 *read_sector(UINT64 lba)
{
    SEGMENT data;
    UINT8 status;

    if (lba >= device.capacity)
        fail("a read past the device's capacity", ERR_OUT_OF_RANGE);
    data.address = device.request_device + DATA_OFFSET;
    data.length = SECTOR_SIZE;
    status = submit(REQUEST_TYPE_IN, lba, &data, 1);
    if (status != REQUEST_STATUS_OK)
        fail("a read request failed", status);
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

/* ------------------------------------------------------------------------
   Serving a client. */

/* The device is reset before the pins are released, so it reads and
   writes none of their pages any more (3.3.1). */
_Noreturn static void stop(void)
{
    virtio_driver_reset(&device.virtio);
    check(pin_release(device.pin), "pin_release");
    if (device.section_pin != HANDLE_INVALID)
        check(pin_release(device.section_pin), "pin_release");
    say("nova-virtio-blk: the client closed its end after ");
    say_number(device.reads);
    say(" reads, ");
    say_number(device.writes);
    say(" writes and ");
    say_number(device.flushes);
    say(" flushes; device reset, pins released\r\n");
    process_exit(VIRTIO_BLK_EXIT_DONE);
}

/* Give the platform driver the client's endpoint, and close the
   bootstrap channel. */
static void report(UINT32 bootstrap, UINT32 client)
{
    HANDLE_TRANSFER transfer;
    UINT32 transaction = 0;

    transfer.handle = client;
    transfer.rights = BLOCK_CLIENT_RIGHTS;
    check(channel_write(bootstrap, &transaction, VIRTIO_BLK_REPORT_SIZE,
                        &transfer, 1),
          "cannot report to the platform driver");
    handle_close(bootstrap);
}

/* Answer the client, waiting while its queue is full. */
static void answer(UINT32 transaction, UINT32 status, UINT64 information)
{
    BLOCK_REPLY reply;
    UINT32 written, observed;

    reply.transaction = transaction;
    reply.status = status;
    reply.information = information;
    for (;;) {
        written = channel_write(device.client, &reply, sizeof reply,
                                no_handles, 0);
        if (written == OK)
            return;
        if (written == ERR_PEER_CLOSED)
            stop();
        if (written != ERR_SHOULD_WAIT)
            fail("cannot answer the client", written);
        check(object_wait_one(device.client,
                              CHANNEL_WRITABLE | CHANNEL_PEER_CLOSED,
                              DEADLINE_INFINITE, &observed),
              "waiting for the client");
    }
}

/* Pin the section the client attaches, whole, for the device. */
static UINT32 attach(const BLOCK_REQUEST *request, UINT32 handle_count)
{
    UINT32 status;

    if (device.section_pin != HANDLE_INVALID)
        return ERR_BAD_STATE;
    if (handle_count != 1 || received[0].type != TYPE_SECTION ||
        (received[0].rights & BLOCK_SECTION_RIGHTS & ~RIGHT_TRANSFER) !=
            (BLOCK_SECTION_RIGHTS & ~RIGHT_TRANSFER) ||
        request->length == 0 || request->length % PAGE_SIZE != 0 ||
        request->length > SECTION_MAX_SIZE || request->sector ||
        request->buffer)
        return ERR_INVALID_ARGS;
    status = section_pin(device.virtio.initiator, received[0].handle, 0,
                         request->length, PIN_DEVICE_READ | PIN_DEVICE_WRITE,
                         section_pages, &device.section_pin);
    if (status != OK) {
        device.section_pin = HANDLE_INVALID;
        return status;
    }
    device.section_size = request->length;
    return OK;
}

/* The pages of the section the bytes from buffer on take, as segments;
   returns their count. The range lies within the section. */
static UINT32 segments_of(UINT32 buffer, UINT32 length, SEGMENT *segments)
{
    UINT32 count = 0, within, n;

    while (length) {
        within = buffer % PAGE_SIZE;
        n = PAGE_SIZE - within < length ? PAGE_SIZE - within : length;
        segments[count].address = section_pages[buffer / PAGE_SIZE] + within;
        segments[count].length = n;
        count++;
        buffer += n;
        length -= n;
    }
    return count;
}

/* A read or a write, checked against the device and the section. */
static UINT32 transfer(const BLOCK_REQUEST *request)
{
    SEGMENT segments[SEGMENTS_MAX];
    UINT32 count, type = request->operation == BLOCK_READ ? REQUEST_TYPE_IN
                                                           : REQUEST_TYPE_OUT;
    UINT8 status;

    if (request->length == 0 || request->length % SECTOR_SIZE != 0 ||
        request->length > BLOCK_TRANSFER_MAX ||
        request->buffer % SECTOR_SIZE != 0)
        return ERR_INVALID_ARGS;
    if (request->buffer > device.section_size ||
        request->length > device.section_size - request->buffer ||
        request->sector >= device.capacity ||
        request->length / SECTOR_SIZE > device.capacity - request->sector)
        return ERR_OUT_OF_RANGE;
    if (type == REQUEST_TYPE_OUT && (device.features & 1ULL << FEATURE_RO))
        return ERR_ACCESS_DENIED;
    count = segments_of(request->buffer, request->length, segments);
    status = submit(type, request->sector, segments, count);
    if (status != REQUEST_STATUS_OK)
        return status == REQUEST_STATUS_UNSUPPORTED ? ERR_NOT_SUPPORTED
                                                    : ERR_IO;
    if (type == REQUEST_TYPE_IN)
        device.reads++;
    else
        device.writes++;
    return OK;
}

/* A flush, sector 0 and no data (5.2.6.1). */
static UINT32 flush(const BLOCK_REQUEST *request)
{
    UINT8 status;

    if (request->sector || request->length || request->buffer)
        return ERR_INVALID_ARGS;
    if (!(device.features & 1ULL << FEATURE_FLUSH))
        return ERR_NOT_SUPPORTED;
    status = submit(REQUEST_TYPE_FLUSH, 0, 0, 0);
    if (status != REQUEST_STATUS_OK)
        return status == REQUEST_STATUS_UNSUPPORTED ? ERR_NOT_SUPPORTED
                                                    : ERR_IO;
    device.flushes++;
    return OK;
}

/* Read the client's next message whole and answer it. */
static void serve_one(void)
{
    const BLOCK_REQUEST *request = (const BLOCK_REQUEST *)message;
    UINT32 bytes, count, status, i;
    UINT64 information = 0;

    status = channel_read(device.client, message, sizeof message, received,
                          CHANNEL_MAX_HANDLES, &bytes, &count);
    if (status == ERR_PEER_CLOSED)
        stop();
    check(status, "reading the client's request");
    if (bytes != sizeof *request) {
        for (i = 0; i < count; i++)
            handle_close(received[i].handle);
        answer(bytes >= sizeof request->transaction ? request->transaction
                                                    : 0,
               ERR_INVALID_ARGS, 0);
        return;
    }
    if (request->operation == BLOCK_ATTACH) {
        status = attach(request, count);
        information = status == OK ? device.capacity : 0;
    } else if (count) {
        status = ERR_INVALID_ARGS;
    } else if (device.section_pin == HANDLE_INVALID) {
        status = ERR_BAD_STATE;
    } else if (request->operation == BLOCK_READ ||
               request->operation == BLOCK_WRITE) {
        status = transfer(request);
    } else if (request->operation == BLOCK_FLUSH) {
        status = flush(request);
    } else {
        status = ERR_NOT_SUPPORTED;
    }
    for (i = 0; i < count; i++)
        handle_close(received[i].handle);
    answer(request->transaction, status, information);
}

_Noreturn static void serve(UINT32 bootstrap)
{
    UINT32 theirs, observed;

    start_device(bootstrap, FEATURES_SERVE);
    check(channel_create(CLIENT_CAPACITY, &device.client, &theirs),
          "channel_create");
    report(bootstrap, theirs);
    for (;;) {
        check(object_wait_one(device.client,
                              CHANNEL_READABLE | CHANNEL_PEER_CLOSED,
                              DEADLINE_INFINITE, &observed),
              "waiting for the client");
        serve_one();
    }
}

_Noreturn void virtio_blk_main(UINT32 bootstrap, UINT64 argument)
{
    UINT64 start;

    if (argument == VIRTIO_BLK_SERVE)
        serve(bootstrap);
    start_device(bootstrap, FEATURES_INSPECT);
    handle_close(bootstrap);
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
