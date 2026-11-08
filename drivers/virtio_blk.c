/* A user-mode virtio block driver (VIRTIO 1.4 5.2) over the PCI
   transport's modern interface (4.1). It starts as every virtio driver
   here does (virtio_driver.h), with one request virtqueue (2.7) in a
   pinned page, and runs in one of two modes (virtio_blk.h).

   Inspecting, it checks the boot disk (docs/spec/disk-layout.md): LBA 1
   must hold a GPT header's signature, the first entry of the partition
   entry array the header names a type and a first LBA, and the sector
   there, the first of the EFI System Partition, must end with the boot
   signature. A disk that passes is not logged; one that does not is,
   with the check it fails. Either way the driver resets the device,
   releases its pin and exits, with the result as its exit code.

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

/* The GPT header's signature and its entry array's LBA (UEFI 5.3.2), a
   partition entry's type and first LBA (5.3.3), and the signature at
   byte 510 that ends a FAT boot sector (fatgen103). */
#define GPT_HEADER_LBA 1
#define GPT_SIGNATURE "EFI PART"
#define GPT_SIGNATURE_SIZE 8
#define GPT_ENTRIES_LBA 72
#define GPT_ENTRY_TYPE 0
#define GPT_ENTRY_TYPE_SIZE 16
#define GPT_ENTRY_START 32
#define BOOT_SIGNATURE 510
#define BOOT_SIGNATURE_FIRST 0x55
#define BOOT_SIGNATURE_SECOND 0xaa

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
    UINT32 client;          /* serving: the driver's end of the client's */
    UINT32 section_pin;     /* the attached section's, or HANDLE_INVALID */
    UINT64 section_size;
    UINT64 reads, writes, flushes;
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

_Noreturn static void fail(const char *event, UINT32 status)
{
    virtio_driver_fail(&device.virtio, event, status);
}

static void check(UINT32 status, const char *event)
{
    virtio_driver_check(&device.virtio, status, event);
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
    UINT64 addresses[DMA_PAGES];

    device.section_pin = HANDLE_INVALID;
    virtio_driver_open(virtio, "nova-virtio-blk", VIRTIO_BLK_EXIT_FAILED,
                       bootstrap, CONFIG_SIZE);
    virtio_driver_interrupt(virtio, QUEUE_KEY);
    device.features = virtio_driver_negotiate(virtio, features);
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

    check(clock_monotonic(&now), "clock");
    do {
        check(port_wait(device.virtio.port, now + REQUEST_TIMEOUT_NS, &packet),
              "request interrupt");
        if (packet.type != PACKET_INTERRUPT || packet.key != QUEUE_KEY)
            fail("packet", ERR_BAD_STATE);
        check(interrupt_ack(device.virtio.interrupt), "interrupt ack");
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
        fail("used chain", ERR_BAD_STATE);
    return device.request[STATUS_OFFSET];
}

/* The block protocol's status for the status byte a request ended
   with (5.2.6). */
static UINT32 request_status(UINT8 status)
{
    if (status == REQUEST_STATUS_OK)
        return OK;
    return status == REQUEST_STATUS_UNSUPPORTED ? ERR_NOT_SUPPORTED : ERR_IO;
}

/* ------------------------------------------------------------------------
   The boot disk's check. */

/* Reset the device, so that it reads and writes no page of the pin
   any more (3.3.1), release the pin, and end with exit_code. */
_Noreturn static void end_inspect(UINT32 exit_code)
{
    virtio_driver_reset(&device.virtio);
    check(pin_release(device.pin), "pin release");
    process_exit(exit_code);
}

_Noreturn static void inspect_failed(const char *reason)
{
    say("nova-virtio-blk: inspect failed reason=");
    say_value(reason);
    say("\r\n");
    end_inspect(VIRTIO_BLK_EXIT_FAILED);
}

/* Read one sector, at lba, into the request page's data. */
static const volatile UINT8 *read_sector(UINT64 lba)
{
    SEGMENT data;

    if (lba >= device.capacity)
        inspect_failed("a sector past the disk");
    data.address = device.request_device + DATA_OFFSET;
    data.length = SECTOR_SIZE;
    check(request_status(submit(REQUEST_TYPE_IN, lba, &data, 1)), "read");
    return device.request + DATA_OFFSET;
}

_Noreturn static void inspect(UINT32 bootstrap)
{
    const volatile UINT8 *sector;
    UINT64 start;
    UINT32 i;

    start_device(bootstrap, FEATURES_INSPECT);
    handle_close(bootstrap);
    sector = read_sector(GPT_HEADER_LBA);
    if (!same_text(sector, GPT_SIGNATURE, GPT_SIGNATURE_SIZE))
        inspect_failed("no GPT signature");
    sector = read_sector(read64_le(sector + GPT_ENTRIES_LBA));
    for (i = 0; i < GPT_ENTRY_TYPE_SIZE && !sector[GPT_ENTRY_TYPE + i]; i++)
        ;
    start = read64_le(sector + GPT_ENTRY_START);
    if (i == GPT_ENTRY_TYPE_SIZE || !start)
        inspect_failed("no partition");
    sector = read_sector(start);
    if (sector[BOOT_SIGNATURE] != BOOT_SIGNATURE_FIRST ||
        sector[BOOT_SIGNATURE + 1] != BOOT_SIGNATURE_SECOND)
        inspect_failed("no boot signature");
    end_inspect(VIRTIO_BLK_EXIT_DONE);
}

/* ------------------------------------------------------------------------
   Serving a client. */

/* The device is reset before the pins are released, so it reads and
   writes none of their pages any more (3.3.1). */
_Noreturn static void stop(void)
{
    virtio_driver_reset(&device.virtio);
    check(pin_release(device.pin), "pin release");
    if (device.section_pin != HANDLE_INVALID)
        check(pin_release(device.section_pin), "pin release");
    say("nova-virtio-blk: stopped reads=");
    say_number(device.reads);
    say(" writes=");
    say_number(device.writes);
    say(" flushes=");
    say_number(device.flushes);
    say("\r\n");
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
          "report");
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
            fail("answer", written);
        check(object_wait_one(device.client,
                              CHANNEL_WRITABLE | CHANNEL_PEER_CLOSED,
                              DEADLINE_INFINITE, &observed),
              "client wait");
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
    UINT32 status;

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
    status = request_status(submit(type, request->sector, segments, count));
    if (status != OK)
        return status;
    if (type == REQUEST_TYPE_IN)
        device.reads++;
    else
        device.writes++;
    return OK;
}

/* A flush, sector 0 and no data (5.2.6.1). */
static UINT32 flush(const BLOCK_REQUEST *request)
{
    UINT32 status;

    if (request->sector || request->length || request->buffer)
        return ERR_INVALID_ARGS;
    if (!(device.features & 1ULL << FEATURE_FLUSH))
        return ERR_NOT_SUPPORTED;
    status = request_status(submit(REQUEST_TYPE_FLUSH, 0, 0, 0));
    if (status == OK)
        device.flushes++;
    return status;
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
    check(status, "request read");
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
    say("nova-virtio-blk: running pci=");
    pci_say_function(device.virtio.device.requester);
    say(" features=");
    say_hex(device.features);
    say(" sectors=");
    say_number(device.capacity);
    say(" queue=");
    say_number(QUEUE_SIZE);
    say("\r\n");
    check(channel_create(CLIENT_CAPACITY, &device.client, &theirs),
          "channel create");
    report(bootstrap, theirs);
    for (;;) {
        check(object_wait_one(device.client,
                              CHANNEL_READABLE | CHANNEL_PEER_CLOSED,
                              DEADLINE_INFINITE, &observed),
              "client wait");
        serve_one();
    }
}

_Noreturn void virtio_blk_main(UINT32 bootstrap, UINT64 argument)
{
    if (argument == VIRTIO_BLK_SERVE)
        serve(bootstrap);
    inspect(bootstrap);
}
