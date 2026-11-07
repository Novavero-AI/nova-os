/* A user-mode virtio input driver (VIRTIO 1.4 5.8) over the PCI
   transport's modern interface (4.1). It starts as every virtio driver
   here does (virtio_driver.h); reads the device's name and the event
   types and key codes it supports from the device-specific
   configuration (5.8.4); fills the event queue with buffers (5.8.5);
   and reports to the platform driver with the endpoint its client reads
   key events from (docs/spec/input.md). From then on each used-buffer
   notification, an interrupt packet, has it take the events the device
   wrote, send each key event as a message on that endpoint and give the
   buffers back (5.8.6). A watch of the endpoint queues a packet on the
   same port once the client has closed its end, and a send can find it
   closed too; either way the driver resets the device, releases its pin
   and exits, with no key needed to tell it. */

#include "virtio_input.h"
#include "input_protocol.h"
#include "log.h"
#include "mmio.h"
#include "virtio_driver.h"

/* The input device has no feature bits of its own (5.8.3), so the
   driver accepts the reserved ones alone (virtio_driver.h). */
#define FEATURES_ACCEPTED VIRTIO_FEATURES_RESERVED

/* struct virtio_input_config (5.8.4): select, subsel and size, then a
   union of CONFIG_DATA_SIZE bytes at CONFIG_DATA, CONFIG_STRUCT_SIZE
   bytes in all. The driver writes select and subsel alone (5.8.5.1). */
#define CONFIG_SELECT 0
#define CONFIG_SUBSEL 1
#define CONFIG_SIZE 2
#define CONFIG_DATA 8
#define CONFIG_DATA_SIZE 128
#define CONFIG_STRUCT_SIZE (CONFIG_DATA + CONFIG_DATA_SIZE)
#define CFG_ID_NAME 0x01
#define CFG_EV_BITS 0x11

/* A configuration that changes across this many reads in a row is
   changing faster than the driver can read it, and the driver gives
   up. */
#define CONFIG_READS_MAX 16

/* Event types are evdev's, below EVENT_TYPES; each type's codes are bits
   of its EV_BITS bitmap, code n in bit n % 8 of byte n / 8. VIRTIO 1.4
   leaves the bit order to the evdev implementation; QEMU's devices were
   observed to use this one. */
#define EVENT_TYPES 32
#define BITS_PER_BYTE 8
#define KEY_CODES (CONFIG_DATA_SIZE * BITS_PER_BYTE)

/* struct virtio_input_event (5.8.6): type, code and value. */
#define EVENT_TYPE 0
#define EVENT_CODE 2
#define EVENT_VALUE 4
#define EVENT_SIZE 8

/* The event queue, 0, at QUEUE_SIZE entries, a power of 2 no larger
   than the device's maximum (4.1.5.1.3), and the status queue, 1, which
   the driver leaves unused (5.8.2). The event queue's parts and its
   buffers share the one DMA page at their alignments (2.7): the
   descriptor table at 0 (16 bytes each, 16-aligned), the available ring
   (2-aligned), the used ring (4-aligned), and buffer i, the one
   descriptor i names, EVENT_SIZE bytes at EVENTS_OFFSET plus
   i * EVENT_SIZE. */
#define EVENT_QUEUE 0
#define QUEUE_SIZE 64
#define AVAIL_OFFSET 1024
#define USED_OFFSET 1280
#define EVENTS_OFFSET 2048
#define AVAIL_SIZE                                                       \
    (VIRTQ_RING_EXTRA_SIZE + VIRTQ_AVAIL_ENTRY_SIZE * QUEUE_SIZE)
#define USED_SIZE                                                        \
    (VIRTQ_RING_EXTRA_SIZE + VIRTQ_USED_ELEMENT_SIZE * QUEUE_SIZE)
#define DMA_PAGES 1

_Static_assert(QUEUE_SIZE * VIRTQ_DESCRIPTOR_SIZE <= AVAIL_OFFSET &&
                   AVAIL_OFFSET + AVAIL_SIZE <= USED_OFFSET &&
                   USED_OFFSET % 4 == 0 &&
                   USED_OFFSET + USED_SIZE <= EVENTS_OFFSET &&
                   EVENTS_OFFSET + QUEUE_SIZE * EVENT_SIZE <= PAGE_SIZE,
               "the event queue and its buffers fit one page");

/* The most key events queued for the client before the driver waits for
   it to read: each message holds kernel memory until it is read. */
#define CLIENT_QUEUE_CAPACITY 32

/* The keys of the port's packets: the event queue's interrupt, and the
   watch of the client's endpoint. */
#define QUEUE_KEY 1
#define CLIENT_KEY 2

typedef struct {
    VIRTIO_DRIVER virtio;
    volatile UINT8 *notify; /* the event queue's notify address */
    volatile UINT8 *queue;  /* the DMA page */
    UINT32 pin;
    UINT32 events;          /* the endpoint key events go out on */
    UINT16 avail_index, used_seen;
    UINT8 held[QUEUE_SIZE]; /* whether the device holds buffer i */
    UINT8 keys[CONFIG_DATA_SIZE]; /* the EV_KEY bitmap */
    UINT64 sent;
} DEVICE;

static DEVICE device;

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

/* ------------------------------------------------------------------------
   The device's configuration. */

/* Select select and subsel, and return the size the device gives
   there, at most CONFIG_DATA_SIZE however large a size it claims, with
   capacity bytes written to data: the bytes the device gives, then
   zeros. The size and the bytes are read within one configuration
   generation (2.5.1). A size of 0 means the device has nothing there
   (5.8.5.2). */
static UINT32 query(UINT8 select, UINT8 subsel, UINT8 *data,
                    UINT32 capacity)
{
    volatile UINT8 *config = device.virtio.device_config;
    UINT32 reads, size, i;
    UINT8 before;

    mmio_write8(config + CONFIG_SELECT, select);
    mmio_write8(config + CONFIG_SUBSEL, subsel);
    for (reads = 0; reads < CONFIG_READS_MAX; reads++) {
        before = virtio_driver_generation(&device.virtio);
        size = mmio_read8(config + CONFIG_SIZE);
        if (size > CONFIG_DATA_SIZE)
            size = CONFIG_DATA_SIZE;
        for (i = 0; i < capacity; i++)
            data[i] = i < size ? mmio_read8(config + CONFIG_DATA + i) : 0;
        if (virtio_driver_generation(&device.virtio) == before)
            return size;
    }
    fail("the device's configuration kept changing", ERR_TIMED_OUT);
}

static int has_key(UINT32 code)
{
    return code < KEY_CODES &&
           (device.keys[code / BITS_PER_BYTE] >> code % BITS_PER_BYTE & 1);
}

/* The name, the event types and the key codes, logged; a device with no
   key codes has nothing for the driver to send. A string has no NUL
   terminator (5.8.4), but QEMU 11.1's devices were observed to count
   one in the name's size, so the name ends at its first NUL. */
static void describe(void)
{
    UINT8 name[CONFIG_DATA_SIZE];
    UINT32 type, size, length, code, keys = 0, types = 0;

    size = query(CFG_ID_NAME, 0, name, sizeof name);
    for (length = 0; length < size && name[length]; length++)
        ;
    say("nova-virtio-input: ");
    pci_say_function(device.virtio.device.requester);
    say(" is \"");
    say_printable(name, length);
    say("\"");
    for (type = 0; type < EVENT_TYPES; type++)
        if (query(CFG_EV_BITS, (UINT8)type, name, 0))
            types |= 1u << type;
    query(CFG_EV_BITS, INPUT_TYPE_KEY, device.keys, sizeof device.keys);
    for (code = 0; code < KEY_CODES; code++)
        keys += (UINT32)has_key(code);
    say(", event types ");
    say_hex(types);
    say(", ");
    say_number(keys);
    say(" key codes\r\n");
    if (!keys)
        fail("the device has no keys", ERR_NOT_SUPPORTED);
}

/* ------------------------------------------------------------------------
   The event queue. */

/* Hand buffer id back to the device in the available ring's next entry;
   virtio_driver_publish exposes it. */
static void give(UINT16 id)
{
    volatile UINT8 *avail = device.queue + AVAIL_OFFSET;

    *(volatile UINT16 *)(avail + VIRTQ_RING_ENTRIES +
                         device.avail_index % QUEUE_SIZE *
                             VIRTQ_AVAIL_ENTRY_SIZE) = id;
    device.held[id] = 1;
    device.avail_index++;
}

static void publish(void)
{
    virtio_driver_publish(device.queue + AVAIL_OFFSET,
                          device.queue + USED_OFFSET, device.notify,
                          EVENT_QUEUE, device.avail_index);
}

/* One page for the queue and its buffers: descriptor i names buffer i,
   device-writable (5.8.6.1); every buffer goes to the device. */
static void set_up_queue(void)
{
    UINT64 address;
    UINT16 i;
    volatile UINT8 *d;

    device.queue = virtio_driver_dma(&device.virtio, DMA_PAGES, &address,
                                     &device.pin);
    for (i = 0; i < QUEUE_SIZE; i++) {
        d = device.queue + i * VIRTQ_DESCRIPTOR_SIZE;
        *(volatile UINT64 *)(d + VIRTQ_DESC_ADDRESS) =
            address + EVENTS_OFFSET + i * EVENT_SIZE;
        *(volatile UINT32 *)(d + VIRTQ_DESC_LENGTH) = EVENT_SIZE;
        *(volatile UINT16 *)(d + VIRTQ_DESC_FLAGS) = VIRTQ_DESC_F_WRITE;
        *(volatile UINT16 *)(d + VIRTQ_DESC_NEXT) = 0;
    }
    device.notify = virtio_driver_queue(&device.virtio, EVENT_QUEUE,
                                        QUEUE_SIZE, address,
                                        address + AVAIL_OFFSET,
                                        address + USED_OFFSET);
    for (i = 0; i < QUEUE_SIZE; i++)
        give(i);
}

/* 3.1.1: the reset and bus mastering come with virtio_driver_open; then
   the interrupt, the features, the configuration, which the driver may
   not write before FEATURES_OK (step 4), the queue, filled with
   buffers, and DRIVER_OK, before which the device gets no
   notification. */
static void start_device(UINT32 bootstrap)
{
    VIRTIO_DRIVER *virtio = &device.virtio;
    UINT64 accepted;

    virtio_driver_open(virtio, "nova-virtio-input", VIRTIO_INPUT_EXIT_FAILED,
                       bootstrap, CONFIG_STRUCT_SIZE);
    virtio_driver_interrupt(virtio, QUEUE_KEY);
    accepted = virtio_driver_negotiate(virtio, FEATURES_ACCEPTED);
    describe();
    set_up_queue();
    virtio_driver_set_status(virtio, VIRTIO_STATUS_DRIVER_OK);
    publish();
    say("nova-virtio-input: ");
    pci_say_function(virtio->device.requester);
    say(" running: features ");
    say_hex(accepted);
    say(", an event queue of ");
    say_number(QUEUE_SIZE);
    say(" on MSI-X\r\n");
}

/* Give the platform driver the client's endpoint, and close the
   bootstrap channel. */
static void report(UINT32 bootstrap, UINT32 client)
{
    HANDLE_TRANSFER transfer;
    UINT32 transaction = 0;

    transfer.handle = client;
    transfer.rights = VIRTIO_INPUT_CLIENT_RIGHTS;
    check(channel_write(bootstrap, &transaction, VIRTIO_INPUT_REPORT_SIZE,
                        &transfer, 1),
          "cannot report to the platform driver");
    handle_close(bootstrap);
}

/* ------------------------------------------------------------------------
   Events. */

/* The client has closed its endpoint: no event has anywhere to go. The
   device is reset before the pin is released, so it writes none of the
   pages any more (3.3.1). */
_Noreturn static void stop(void)
{
    virtio_driver_reset(&device.virtio);
    check(pin_release(device.pin), "pin_release");
    say("nova-virtio-input: the client closed its endpoint after ");
    say_number(device.sent);
    say(" key events; device reset, pin released\r\n");
    process_exit(VIRTIO_INPUT_EXIT_DONE);
}

/* Send one key event to the client, waiting while its queue is full;
   the buffers not yet given back meanwhile are what the device can still
   fill, and past them it may drop events (5.8.6.2). */
static void send(UINT16 code, UINT32 value)
{
    INPUT_EVENT event;
    UINT32 status, observed;

    event.transaction = 0;
    event.type = INPUT_TYPE_KEY;
    event.code = code;
    event.value = value;
    event.reserved = 0;
    check(clock_monotonic(&event.time), "clock_monotonic");
    for (;;) {
        status = channel_write(device.events, &event, sizeof event,
                               no_handles, 0);
        if (status == OK)
            break;
        if (status == ERR_PEER_CLOSED)
            stop();
        if (status != ERR_SHOULD_WAIT)
            fail("cannot send a key event", status);
        check(object_wait_one(device.events,
                              CHANNEL_WRITABLE | CHANNEL_PEER_CLOSED,
                              DEADLINE_INFINITE, &observed),
              "waiting for the client");
    }
    device.sent++;
}

/* Send the event in buffer id if it is a key event the device said it
   has (5.8.6.1: anything else is ignored). The event is copied out of
   DMA memory once, so what is checked is what is sent. */
static void take_event(UINT32 id)
{
    const volatile UINT8 *buffer =
        device.queue + EVENTS_OFFSET + id * EVENT_SIZE;
    UINT16 type = *(const volatile UINT16 *)(buffer + EVENT_TYPE);
    UINT16 code = *(const volatile UINT16 *)(buffer + EVENT_CODE);
    UINT32 value = *(const volatile UINT32 *)(buffer + EVENT_VALUE);

    if (type == INPUT_TYPE_KEY && has_key(code) &&
        (value == INPUT_KEY_RELEASED || value == INPUT_KEY_PRESSED ||
         value == INPUT_KEY_REPEATED))
        send(code, value);
}

/* Take every buffer the used ring returned since the last pass, in
   order (2.7.14): a buffer the device did not hold, the same one twice,
   or more returned than it holds, is a broken device. A buffer with
   fewer than EVENT_SIZE bytes written holds no event (2.7.8.3). The
   buffers go back to the device once the pass has sent their events. */
static void take_used(void)
{
    const volatile UINT8 *used = device.queue + USED_OFFSET;
    const volatile UINT8 *element;
    UINT16 index = *(const volatile UINT16 *)(used + VIRTQ_RING_INDEX);
    UINT16 returned[QUEUE_SIZE];
    UINT32 id, length, count = 0, i;

    if (index == device.used_seen)
        return;
    if ((UINT16)(index - device.used_seen) > QUEUE_SIZE)
        fail("the device returned more buffers than it holds",
             (UINT16)(index - device.used_seen));
    load_fence();
    while (device.used_seen != index) {
        element = used + VIRTQ_RING_ENTRIES +
                  device.used_seen % QUEUE_SIZE * VIRTQ_USED_ELEMENT_SIZE;
        id = *(const volatile UINT32 *)(element + VIRTQ_USED_ID);
        length = *(const volatile UINT32 *)(element + VIRTQ_USED_LENGTH);
        device.used_seen++;
        if (id >= QUEUE_SIZE || !device.held[id])
            fail("the device returned a buffer it did not hold", id);
        device.held[id] = 0;
        returned[count++] = (UINT16)id;
        if (length >= EVENT_SIZE)
            take_event(id);
    }
    for (i = 0; i < count; i++)
        give(returned[i]);
    publish();
}

_Noreturn void virtio_input_main(UINT32 bootstrap, UINT64 argument)
{
    PORT_PACKET packet;
    UINT32 client;

    (void)argument;
    start_device(bootstrap);
    check(channel_create(CLIENT_QUEUE_CAPACITY, &device.events, &client),
          "channel_create");
    check(object_wait_async(device.events, device.virtio.port, CLIENT_KEY,
                            CHANNEL_PEER_CLOSED),
          "cannot watch the client's endpoint");
    report(bootstrap, client);
    for (;;) {
        check(port_wait(device.virtio.port, DEADLINE_INFINITE, &packet),
              "port_wait");
        if (packet.type == PACKET_SIGNAL && packet.key == CLIENT_KEY)
            stop();
        if (packet.type != PACKET_INTERRUPT || packet.key != QUEUE_KEY)
            fail("an unexpected packet came", packet.type);
        check(interrupt_ack(device.virtio.interrupt), "interrupt_ack");
        take_used();
    }
}
