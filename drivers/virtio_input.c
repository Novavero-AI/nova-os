/* A user-mode virtio input driver (VIRTIO 1.4 5.8) over the PCI
   transport's modern interface (4.1). It starts as every virtio driver
   here does (virtio_driver.h); reads from the device-specific
   configuration (5.8.4) the device's name, the event types and codes it
   supports and its absolute axes' ranges, and from them whether it is a
   keyboard or a pointer; fills the event queue with buffers (5.8.5);
   describes the device on the endpoint its client reads from and
   reports to the platform driver with it (docs/spec/input.md). From
   then on each used-buffer notification, an interrupt packet, has it
   take the events the device wrote and give the buffers back (5.8.6): a
   keyboard's key events go to the client one message each, and a
   pointer's events one report for each EV_SYN SYN_REPORT, or more where
   one would change a button twice or too many, which pointer.c
   gathers. A watch of the endpoint queues a packet on the
   same port once the client has closed its end, and a send can find it
   closed too; either way the driver resets the device, releases its pin
   and exits, with no event needed to tell it. */

#include "virtio_input.h"
#include "input_protocol.h"
#include "log.h"
#include "mmio.h"
#include "pointer.h"
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
#define CFG_ABS_INFO 0x12

/* struct virtio_input_absinfo (5.8.4) starts with the axis's minimum
   and maximum, each le32; the driver reads those two. */
#define ABS_INFO_MIN 0
#define ABS_INFO_MAX 4
#define ABS_INFO_RANGE_SIZE 8

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
#define BITMAP_CODES (CONFIG_DATA_SIZE * BITS_PER_BYTE)

_Static_assert(CONFIG_DATA_SIZE == POINTER_BITMAP_BYTES,
               "a pointer keeps the configuration's bitmaps whole");

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

/* The keys of the port's packets: the event queue's interrupt, and the
   watch of the client's endpoint. */
#define QUEUE_KEY 1
#define CLIENT_KEY 2

typedef struct {
    VIRTIO_DRIVER virtio;
    volatile UINT8 *notify; /* the event queue's notify address */
    volatile UINT8 *queue;  /* the DMA page */
    UINT32 pin;
    UINT32 events;          /* the endpoint messages go out on */
    UINT16 avail_index, used_seen;
    UINT8 held[QUEUE_SIZE]; /* whether the device holds buffer i */
    INPUT_DEVICE description;
    POINTER pointer; /* the bitmaps, and a pointer's buttons and report */
    UINT64 sent;            /* key events or pointer reports */
} DEVICE;

/* What the driver's running line gives of the device's configuration
   (docs/spec/input.md K1): its name, its event types as a mask by type,
   and how many key codes it has. */
typedef struct {
    UINT8 name[CONFIG_DATA_SIZE];
    UINT32 name_length;
    UINT32 types;
    UINT32 keys;
} IDENTITY;

static DEVICE device;

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

/* A two's complement le32, as evdev's signed values are written. */
static INT32 le32(const UINT8 *bytes)
{
    return (INT32)((UINT32)bytes[0] | (UINT32)bytes[1] << 8 |
                   (UINT32)bytes[2] << 16 | (UINT32)bytes[3] << 24);
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
    fail("config read", ERR_TIMED_OUT);
}

/* How many of the codes from first up to end bitmap has. */
static UINT32 count(const UINT8 *bitmap, UINT32 first, UINT32 end)
{
    UINT32 code, found = 0;

    for (code = first; code < end; code++)
        found += (UINT32)pointer_has(bitmap, code);
    return found;
}

/* The range of absolute axis code into *r, if abs, the EV_ABS bitmap,
   has the axis and its maximum lies above its minimum; whether it
   does. */
static int range(const UINT8 *abs, UINT32 code, INPUT_RANGE *r)
{
    UINT8 info[ABS_INFO_RANGE_SIZE];

    if (!pointer_has(abs, code) ||
        query(CFG_ABS_INFO, (UINT8)code, info, sizeof info) <
            ABS_INFO_RANGE_SIZE ||
        le32(info + ABS_INFO_MAX) <= le32(info + ABS_INFO_MIN))
        return 0;
    r->min = le32(info + ABS_INFO_MIN);
    r->max = le32(info + ABS_INFO_MAX);
    return 1;
}

/* Read the name, the event types and the key codes into id, and for a
   pointer, a device that has EV_ABS or EV_REL, its EV_REL bitmap and
   the ranges of ABS_X and ABS_Y, into the description. A device with no
   key codes has nothing for the driver to send, and is refused. A
   string has no NUL terminator (5.8.4), but QEMU 11.1's devices were
   observed to count one in the name's size, so the name ends at its
   first NUL. */
static void describe(IDENTITY *id)
{
    INPUT_DEVICE *d = &device.description;
    POINTER *p = &device.pointer;
    UINT8 abs[CONFIG_DATA_SIZE];
    UINT32 type, size, length, types = 0;

    size = query(CFG_ID_NAME, 0, id->name, sizeof id->name);
    for (length = 0; length < size && id->name[length]; length++)
        ;
    id->name_length = length;
    for (type = 0; type < EVENT_TYPES; type++)
        if (query(CFG_EV_BITS, (UINT8)type, id->name, 0))
            types |= 1u << type;
    id->types = types;
    query(CFG_EV_BITS, INPUT_TYPE_KEY, p->keys, sizeof p->keys);
    id->keys = count(p->keys, 0, BITMAP_CODES);
    d->type = INPUT_TYPE_DEVICE;
    d->kind = INPUT_DEVICE_KEYBOARD;
    if (types & (1u << POINTER_EV_ABS | 1u << POINTER_EV_REL)) {
        d->kind = INPUT_DEVICE_POINTER;
        query(CFG_EV_BITS, POINTER_EV_REL, p->rel, sizeof p->rel);
        query(CFG_EV_BITS, POINTER_EV_ABS, abs, sizeof abs);
        if (range(abs, POINTER_ABS_X, &d->x))
            d->absolute |= INPUT_AXIS_X;
        if (range(abs, POINTER_ABS_Y, &d->y))
            d->absolute |= INPUT_AXIS_Y;
        p->absolute = d->absolute;
        pointer_start(p);
    }
    if (!id->keys) {
        say("nova-virtio-input: device refused reason=\"no keys\"\r\n");
        process_exit(VIRTIO_INPUT_EXIT_FAILED);
    }
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
    IDENTITY id;
    UINT64 accepted;

    virtio_driver_open(virtio, "nova-virtio-input", VIRTIO_INPUT_EXIT_FAILED,
                       bootstrap, CONFIG_STRUCT_SIZE);
    virtio_driver_interrupt(virtio, QUEUE_KEY);
    accepted = virtio_driver_negotiate(virtio, FEATURES_ACCEPTED);
    describe(&id);
    set_up_queue();
    virtio_driver_set_status(virtio, VIRTIO_STATUS_DRIVER_OK);
    publish();
    say("nova-virtio-input: running pci=");
    pci_say_function(virtio->device.requester);
    say(" name=");
    say_value_bytes(id.name, id.name_length);
    say(" types=");
    say_hex(id.types);
    say(" keys=");
    say_number(id.keys);
    say(" features=");
    say_hex(accepted);
    say(" queue=");
    say_number(QUEUE_SIZE);
    say("\r\n");
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
          "report");
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
    check(pin_release(device.pin), "pin release");
    say(device.description.kind == INPUT_DEVICE_KEYBOARD
            ? "nova-virtio-input: stopped events="
            : "nova-virtio-input: stopped reports=");
    say_number(device.sent);
    say("\r\n");
    process_exit(VIRTIO_INPUT_EXIT_DONE);
}

/* Send size bytes of message to the client, waiting while its queue is
   full; the buffers not yet given back meanwhile are what the device
   can still fill, and past them it may drop events (5.8.6.2). */
static void send(const void *message, UINT32 size)
{
    UINT32 status, observed;

    for (;;) {
        status = channel_write(device.events, message, size, no_handles, 0);
        if (status == OK)
            return;
        if (status == ERR_PEER_CLOSED)
            stop();
        if (status != ERR_SHOULD_WAIT)
            fail("send", status);
        check(object_wait_one(device.events,
                              CHANNEL_WRITABLE | CHANNEL_PEER_CLOSED,
                              DEADLINE_INFINITE, &observed),
              "client wait");
    }
}

static void send_key(UINT16 code, UINT32 value)
{
    INPUT_EVENT event;

    event.transaction = 0;
    event.type = INPUT_TYPE_KEY;
    event.code = code;
    event.value = value;
    event.reserved = 0;
    check(clock_monotonic(&event.time), "clock");
    send(&event, sizeof event);
    device.sent++;
}

/* Take the event in buffer id: a keyboard's goes to the client if it is
   a key event the device said it has, and a pointer's to its report,
   which goes to the client once an event ends it (5.8.6.1: anything
   else is ignored). The event is copied out of DMA memory once, so what
   is checked is what is sent. */
static void take_event(UINT32 id)
{
    const volatile UINT8 *buffer =
        device.queue + EVENTS_OFFSET + id * EVENT_SIZE;
    UINT16 type = *(const volatile UINT16 *)(buffer + EVENT_TYPE);
    UINT16 code = *(const volatile UINT16 *)(buffer + EVENT_CODE);
    UINT32 value = *(const volatile UINT32 *)(buffer + EVENT_VALUE);
    INPUT_POINTER report;

    if (device.description.kind == INPUT_DEVICE_POINTER) {
        if (pointer_take(&device.pointer, type, code, value, &report)) {
            check(clock_monotonic(&report.time), "clock");
            send(&report, sizeof report);
            device.sent++;
        }
    } else if (type == INPUT_TYPE_KEY &&
               pointer_has(device.pointer.keys, code) &&
               (value == INPUT_KEY_RELEASED || value == INPUT_KEY_PRESSED ||
                value == INPUT_KEY_REPEATED)) {
        send_key(code, value);
    }
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
        fail("used ring", ERR_BAD_STATE);
    load_fence();
    while (device.used_seen != index) {
        element = used + VIRTQ_RING_ENTRIES +
                  device.used_seen % QUEUE_SIZE * VIRTQ_USED_ELEMENT_SIZE;
        id = *(const volatile UINT32 *)(element + VIRTQ_USED_ID);
        length = *(const volatile UINT32 *)(element + VIRTQ_USED_LENGTH);
        device.used_seen++;
        if (id >= QUEUE_SIZE || !device.held[id])
            fail("used buffer", ERR_BAD_STATE);
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
    check(channel_create(INPUT_ENDPOINT_CAPACITY, &device.events, &client),
          "channel create");
    check(object_wait_async(device.events, device.virtio.port, CLIENT_KEY,
                            CHANNEL_PEER_CLOSED),
          "client watch");
    send(&device.description, sizeof device.description);
    report(bootstrap, client);
    for (;;) {
        check(port_wait(device.virtio.port, DEADLINE_INFINITE, &packet),
              "port wait");
        if (packet.type == PACKET_SIGNAL && packet.key == CLIENT_KEY)
            stop();
        if (packet.type != PACKET_INTERRUPT || packet.key != QUEUE_KEY)
            fail("packet", ERR_BAD_STATE);
        check(interrupt_ack(device.virtio.interrupt), "interrupt ack");
        take_used();
    }
}
