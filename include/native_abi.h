/** @file
    The numbers the kernel and the native layer agree on: status codes and
    call numbers (docs/spec/syscalls.md S7 and S8), handle values, object
    types and rights (docs/spec/objects.md O3 and O5), process states and
    the limits of the calls the kernel implements. The kernel and the
    native layer both include this file, so each number has one
    definition. Assembly includes it too, for the call numbers and status
    codes; the C part is hidden from it, and no value carries an integer
    suffix, since TinyCC's assembler evaluates them. */

#ifndef NOVA_NATIVE_ABI_H
#define NOVA_NATIVE_ABI_H

/** @name Status codes
    Status codes (S7): what every call returns, 0 for success and any
    other value for the check that failed.
    @{ */
/** Success. */
#define OK 0
/** No such call, an operation the object's type does not support, or a
    handle that may not travel in this message. */
#define ERR_NOT_SUPPORTED 1
/** An argument outside its domain, such as unknown flag bits, a size of
    zero or past a limit, or a misaligned address. */
#define ERR_INVALID_ARGS 2
/** A buffer outside the user range or not mapped with the access the
    call needs. */
#define ERR_BAD_ADDRESS 3
/** The value names no handle in the caller's table. */
#define ERR_BAD_HANDLE 4
/** The handle names an object of another type. */
#define ERR_WRONG_TYPE 5
/** The handle lacks a right the call needs, or the rights asked for are
    not a subset of the handle's. */
#define ERR_ACCESS_DENIED 6
/** Kernel memory, or the job's quota of handles, objects, message bytes
    or pages, is exhausted. */
#define ERR_NO_MEMORY 7
/** A count past a fixed limit, or an offset past an object's size or a
    resource's range. */
#define ERR_OUT_OF_RANGE 8
/** The operation would block: a queue is empty or full. */
#define ERR_SHOULD_WAIT 9
/** The deadline passed. */
#define ERR_TIMED_OUT 10
/** A handle being waited on was closed, or a request was cancelled. */
#define ERR_CANCELED 11
/** The other end of a channel is closed. */
#define ERR_PEER_CLOSED 12
/** A receive buffer cannot hold the message. */
#define ERR_BUFFER_TOO_SMALL 13
/** The object's state forbids the call. */
#define ERR_BAD_STATE 14
/** A resource other than memory is exhausted, such as the job's quota of
    interrupt vectors. */
#define ERR_NO_RESOURCES 15
/** @} */

/** @name Completion status codes
    Status codes only I/O completions carry (S7): a name a directory does
    not hold, a name it holds already, data that fails the check of its
    digest, and a request a device failed.
    @{ */
#define ERR_NOT_FOUND 16      /**< a name a directory does not hold */
#define ERR_ALREADY_EXISTS 17 /**< a name a directory holds already */
/** data that fails the check of its digest */
#define ERR_DATA_INTEGRITY 18
#define ERR_IO 19 /**< a request a device failed */
/** @} */

/** @name Call numbers
    The numbers of the calls this kernel implements (S8), the values of
    RAX at SYSCALL. Number 0 is reserved and names no call.
    @{ */
#define CALL_HANDLE_CLOSE 1             /**< handle_close() */
#define CALL_HANDLE_DUPLICATE 2         /**< handle_duplicate() */
#define CALL_OBJECT_WAIT_ONE 6          /**< object_wait_one() */
#define CALL_OBJECT_WAIT_MANY 7         /**< object_wait_many() */
#define CALL_OBJECT_WAIT_ASYNC 8        /**< object_wait_async() */
#define CALL_CHANNEL_CREATE 10          /**< channel_create() */
#define CALL_CHANNEL_WRITE 11           /**< channel_write() */
#define CALL_CHANNEL_READ 12            /**< channel_read() */
#define CALL_PORT_CREATE 14             /**< port_create() */
#define CALL_PORT_WAIT 16               /**< port_wait() */
#define CALL_CLOCK_MONOTONIC 17         /**< clock_monotonic() */
#define CALL_JOB_CREATE 18              /**< job_create() */
#define CALL_PROCESS_CREATE 19          /**< process_create() */
#define CALL_PROCESS_START 20           /**< process_start() */
#define CALL_PROCESS_EXIT 21            /**< process_exit() */
#define CALL_PROCESS_INFO 22            /**< process_info() */
#define CALL_THREAD_CREATE 23           /**< thread_create() */
#define CALL_THREAD_START 24            /**< thread_start() */
#define CALL_THREAD_EXIT 25             /**< thread_exit() */
#define CALL_SECTION_CREATE 27          /**< section_create() */
#define CALL_PROCESS_MAP 28             /**< process_map() */
#define CALL_PROCESS_UNMAP 29           /**< process_unmap() */
#define CALL_RESOURCE_CREATE 31         /**< resource_create() */
#define CALL_SECTION_CREATE_PHYSICAL 32 /**< section_create_physical() */
#define CALL_INTERRUPT_CREATE 33        /**< interrupt_create() */
#define CALL_INTERRUPT_BIND 34          /**< interrupt_bind() */
#define CALL_INTERRUPT_ACK 35           /**< interrupt_ack() */
#define CALL_INITIATOR_CREATE 37        /**< initiator_create() */
#define CALL_SECTION_PIN 38             /**< section_pin() */
#define CALL_PIN_RELEASE 39             /**< pin_release() */
/** initiator_release_quarantine() */
#define CALL_INITIATOR_RELEASE_QUARANTINE 40
#define CALL_DEBUG_WRITE 41 /**< debug_write() */
#define CALL_RANDOM_READ 42 /**< random_read() */
/** thread_set_tls_base, which sets the calling thread's FS base for
    thread-local storage (syscalls.md S10) */
#define CALL_THREAD_SET_TLS_BASE 44
/** @} */

/** The vector of the 32-bit lane's gate, which a thread of a 32-bit
    process enters with INT (S11). */
#define COMPAT_GATE_VECTOR 0x80

/** @name Handle values
    A handle value holds its table index in bits 31:8 and a generation
    from 1 to 255 in bits 7:0 (O3). Generation 0 is never issued, so 0 is
    never a valid handle.
    @{ */
#define HANDLE_INVALID 0 /**< no handle: never issued */
/** the generation's bits, the low ones; the index lies above them */
#define HANDLE_GENERATION_BITS 8
/** the bits of a handle value that hold its generation */
#define HANDLE_GENERATION_MASK 0xff
/** @} */

/** @name Object types
    Object types as channel_read reports them, numbered in the order of
    the first-set table of objects.md O2: process 1, thread 2, section 3,
    event 4, channel 5, port 6, job 7, resource 8, interrupt 9, initiator
    10, pin 11. These are the types this kernel implements.
    @{ */
/** An address space and a handle table: the unit of isolation and of
    termination. */
#define TYPE_PROCESS 1
/** A schedulable processor context running in one process. */
#define TYPE_THREAD 2
/** A range of pages that can be mapped into processes. */
#define TYPE_SECTION 3
/** A pair of endpoints carrying messages of bytes and handles; a handle
    names one endpoint. */
#define TYPE_CHANNEL 5
/** A queue of packets: user packets, signal packets from watched objects
    and interrupt packets. */
#define TYPE_PORT 6
/** A node in the tree of processes and jobs, carrying quotas and the
    authority to create processes. */
#define TYPE_JOB 7
/** Authority over a range of device memory, interrupt sources, I/O
    ports or bus requester IDs, or over named system privileges
    (ipc.md I6). */
#define TYPE_RESOURCE 8
/** One interrupt source, delivered as port packets. */
#define TYPE_INTERRUPT 9
/** A device's authority to do DMA, used to pin sections for it. */
#define TYPE_INITIATOR 10
/** Pages of a section held for a device's DMA. */
#define TYPE_PIN 11
/** @} */

/** @name Common rights
    Rights (O5): bits 16 to 19 are common to every type, bits 0 to 15
    specific to one. RIGHTS_SAME keeps a source's rights.
    @{ */
#define RIGHT_DUPLICATE 0x10000 /**< handle_duplicate() */
/** moving the handle in a channel message or by process_start() */
#define RIGHT_TRANSFER 0x20000
/** object_wait_one(), object_wait_many() and object_wait_async() */
#define RIGHT_WAIT 0x40000
/** setting and clearing the object's user signals */
#define RIGHT_SIGNAL 0x80000
/** in place of a rights mask: keep the source handle's rights */
#define RIGHTS_SAME 0xffffffff
/** @} */

/** @name Process rights
    @{ */
/** map and unmap in the process's address space: process_map() and
    process_unmap() */
#define PROCESS_RIGHT_MAP 0x1
/** create and start the process's threads: thread_create() and
    process_start() */
#define PROCESS_RIGHT_CREATE_THREAD 0x2
#define PROCESS_RIGHT_TERMINATE 0x4 /**< terminate the process */
/** read the process's state and exit code: process_info() */
#define PROCESS_RIGHT_INSPECT 0x8
/** @} */

/** @name Thread rights
    @{ */
/** start the thread: thread_start(), or process_start() for a process's
    first */
#define THREAD_RIGHT_START 0x1
#define THREAD_RIGHT_TERMINATE 0x2 /**< terminate the thread */
/** @} */

/** @name Section rights
    Mapping a section with a protection needs the matching right.
    @{ */
#define SECTION_RIGHT_READ 0x1    /**< map the section with MAP_READ */
#define SECTION_RIGHT_WRITE 0x2   /**< map the section with MAP_WRITE */
#define SECTION_RIGHT_EXECUTE 0x4 /**< map the section with MAP_EXECUTE */
/** pin the section for a device: section_pin() */
#define SECTION_RIGHT_PIN 0x8
/** @} */

/** @name Channel rights
    @{ */
#define CHANNEL_RIGHT_READ 0x1  /**< read messages: channel_read() */
#define CHANNEL_RIGHT_WRITE 0x2 /**< write messages: channel_write() */
/** @} */

/** @name Port rights
    @{ */
/** have packets queued on the port: name it in object_wait_async() or
    interrupt_bind() */
#define PORT_RIGHT_QUEUE 0x1
#define PORT_RIGHT_READ 0x2 /**< take packets: port_wait() */
/** @} */

/** @name Job rights
    @{ */
/** create processes in the job: process_create() */
#define JOB_RIGHT_CREATE_PROCESS 0x1
/** create child jobs: job_create() */
#define JOB_RIGHT_CREATE_JOB 0x2
#define JOB_RIGHT_TERMINATE 0x4 /**< terminate the job */
/** create processes with PROCESS_OPTION_WRITE_EXECUTE, whose mappings may
    be writable and executable at once */
#define JOB_RIGHT_WRITE_EXECUTE 0x8
/** @} */

/** @name Resource, interrupt and initiator rights
    @{ */
/** Derive sub-resources and create objects over the resource's range,
    and write the debug log through a resource that covers it. */
#define RESOURCE_RIGHT_USE 0x1

/** Deliver the interrupt to a port: interrupt_bind(). */
#define INTERRUPT_RIGHT_BIND 0x1
/** Let the interrupt deliver its next packet: interrupt_ack(). */
#define INTERRUPT_RIGHT_ACK 0x2

/** Pin sections for the initiator's device: section_pin(). */
#define INITIATOR_RIGHT_PIN 0x1
/** Free the initiator's quarantined pins:
    initiator_release_quarantine(). */
#define INITIATOR_RIGHT_RELEASE 0x2
/** @} */

/** @name Signals
    Signals (O9): a process's and a thread's, a channel endpoint's, and
    the synthetic bit 23 a wait observes on an object whose handle closed
    during the wait.
    @{ */
/** the handle waited on closed during the wait */
#define HANDLE_CLOSED 0x800000
#define PROCESS_TERMINATED 0x1 /**< the process has terminated */
#define THREAD_TERMINATED 0x1  /**< the thread has terminated */
#define CHANNEL_READABLE 0x1   /**< the endpoint's queue is not empty */
/** the peer's queue has room for a message */
#define CHANNEL_WRITABLE 0x2
/** the peer's last handle is closed; messages already queued stay
    readable */
#define CHANNEL_PEER_CLOSED 0x4
/** @} */

/** @name Process options
    process_create's options (S8): the process's threads run 32-bit code
    in compatibility mode and may enter by the 32-bit lane (S11); and its
    mappings may be writable and executable at once, which the job handle
    must carry JOB_RIGHT_WRITE_EXECUTE for (O5).
    @{ */
#define PROCESS_OPTION_32_BIT 0x1 /**< the threads run 32-bit code */
/** the mappings may be writable and executable at once */
#define PROCESS_OPTION_WRITE_EXECUTE 0x2
/** @} */

/** @name Mapping protection
    process_map's protection: read, read-write, read-execute, or all
    three; any other combination is refused.
    @{ */
#define MAP_READ 0x1    /**< readable; needs SECTION_RIGHT_READ */
#define MAP_WRITE 0x2   /**< writable; needs SECTION_RIGHT_WRITE */
#define MAP_EXECUTE 0x4 /**< executable; needs SECTION_RIGHT_EXECUTE */
/** @} */

/** @name Process states
    A process's state as process_info reports it: created and not yet
    started, started, or terminated. Every value is nonzero, since a
    failed call reports 0 (S7).
    @{ */
#define PROCESS_STATE_NEW 1        /**< created and not yet started */
#define PROCESS_STATE_RUNNING 2    /**< started */
#define PROCESS_STATE_TERMINATED 3 /**< terminated */
/** @} */

/** @name Channel limits
    The most bytes and handles one message carries, and the most messages
    an endpoint's queue holds (ipc.md I1).
    @{ */
#define CHANNEL_MAX_BYTES 65536 /**< the most bytes in one message */
#define CHANNEL_MAX_HANDLES 64  /**< the most handles in one message */
/** the most messages an endpoint's queue holds: channel_create()'s
    largest capacity */
#define CHANNEL_MAX_CAPACITY 1024
/** @} */

/** @name Ports and packets
    The most user packets a port holds (ipc.md I4), and the packet types.
    @{ */
/** The most user packets a port holds: port_create()'s largest
    capacity. */
#define PORT_MAX_CAPACITY 4096
/** A user packet, whose payload the program that queued it chose. */
#define PACKET_USER 0
/** A packet from a watch object_wait_async() registered. */
#define PACKET_SIGNAL 1
/** A packet from an interrupt interrupt_bind() bound to the port. */
#define PACKET_INTERRUPT 2
/** @} */

/** @name Device memory types
    The memory types section_create_physical offers (ipc.md I6): strong
    uncacheable, for registers, and write-combining, for framebuffers.
    @{ */
#define CACHE_UNCACHED 0        /**< strong uncacheable, for registers */
#define CACHE_WRITE_COMBINING 1 /**< write-combining, for framebuffers */
/** @} */

/** @name Pin options
    What a device does with pinned pages (ipc.md I8), for section_pin.
    @{ */
#define PIN_DEVICE_READ 0x1  /**< the device reads the pages */
#define PIN_DEVICE_WRITE 0x2 /**< the device writes the pages */
/** @} */

/** @name Interrupt sources
    Interrupt sources as interrupt_create names them and an IRQ resource
    covers them (ipc.md I7): ACPI global system interrupt g is source g,
    and the message-signalled interrupts of bus requester ID r (ipc.md I8)
    are source INTERRUPT_SOURCE_MSI + r.
    @{ */
/** The source of requester ID 0's message-signalled interrupts. */
#define INTERRUPT_SOURCE_MSI 0x100000000
/** The end of the sources: the root IRQ resource covers every source
    below it. */
#define INTERRUPT_SOURCE_LIMIT 0x200000000
/** @} */

/** @name System privileges
    The named system privileges a system resource's range counts, from 0
    (ipc.md I6); in the first set only the debug log.
    @{ */
/** writing the kernel's debug log: debug_write() */
#define SYSTEM_PRIVILEGE_DEBUG_LOG 0
#define SYSTEM_PRIVILEGES 1 /**< the number of named privileges */
/** @} */

/** The page size: every section's size, every offset into one and every
    address a section is mapped at is a multiple of it (syscalls.md S8). */
#define PAGE_SIZE 4096

/** The most objects one object_wait_many watches (O9). */
#define WAIT_MANY_MAX 64

/** The largest section section_create makes: its page list fills one
    page. */
#define SECTION_MAX_SIZE 0x200000

/** @name The first process's bootstrap message
    The first process's bootstrap message (ipc.md I6): the root system
    resource, the process itself, the root job, the root MMIO, IRQ and DMA
    resources, a read-only section over the store image nova-boot loaded
    (storage.md F16), then one read-only section for each other file
    nova-boot read from the `\nova` directory. Its bytes are a
    BOOTSTRAP_PLATFORM, then a BOOTSTRAP_FILE for the store image and one
    for each file, in the order of the sections. A name is ASCII,
    NUL-terminated, as the firmware reported it. The boot files are as
    many as the message has room for, one handle each.
    @{ */
#define BOOTSTRAP_ROOT_MMIO 3   /**< the root MMIO resource's handle */
#define BOOTSTRAP_ROOT_IRQ 4    /**< the root IRQ resource's handle */
#define BOOTSTRAP_ROOT_DMA 5    /**< the root DMA resource's handle */
#define BOOTSTRAP_STORE_IMAGE 6 /**< the store image's section */
#define BOOTSTRAP_FIRST_FILE 7  /**< the first boot file's section */
/** the most ranges BOOTSTRAP_PLATFORM's ecam holds */
#define BOOTSTRAP_ECAM_RANGES_MAX 8
/** the most boot files: one handle each, after the others */
#define BOOT_FILES_MAX (CHANNEL_MAX_HANDLES - BOOTSTRAP_FIRST_FILE)
/** the bytes of a boot file's name field, its NUL among them */
#define BOOT_FILE_NAME_SIZE 32
/** @} */

/** @name The kernel's background
    The second message on the first process's bootstrap channel, which
    the kernel writes once it has drawn the desktop background offscreen
    (docs/spec/boot-protocol.md, Startup done): a
    BOOTSTRAP_BACKGROUND_MESSAGE, and the image's sections, each with
    SECTION_RIGHT_READ, RIGHT_DUPLICATE and RIGHT_TRANSFER. The image is
    width x height pixels, row after row with no gap between rows, each
    pixel BOOTSTRAP_BACKGROUND_PIXEL_BYTES in the display's format: the
    pixels the fade's last frame leaves on the screen. It is held in
    bootstrap_background_sections() sections, every one SECTION_MAX_SIZE
    bytes but the last, which mapped one after another read as one image
    (display.md D6).
    @{ */
/** no image: the message carries no section */
#define BOOTSTRAP_BACKGROUND_NONE 0
/** Limb, as gfx/limb.c computes it */
#define BOOTSTRAP_BACKGROUND_LIMB 1
/** the most sections an image takes, a message's handles; the kernel
    hands on no larger image */
#define BOOTSTRAP_BACKGROUND_SECTIONS_MAX CHANNEL_MAX_HANDLES
/** the bytes of one of the image's pixels */
#define BOOTSTRAP_BACKGROUND_PIXEL_BYTES 4
/** @} */

/** The first process reports that its startup is done with one message
    on its bootstrap channel: BOOTSTRAP_DONE_SIZE bytes, a transaction ID
    of 0 (ipc.md I3), and no handles. The kernel holds the channel's other
    end until the report comes and its fade into the desktop has drawn its
    last frame, then closes it and draws no more, so CHANNEL_PEER_CLOSED
    there releases the display to the first process. The first process's
    endpoint carries CHANNEL_RIGHT_READ, CHANNEL_RIGHT_WRITE and RIGHT_WAIT
    but not RIGHT_TRANSFER, so it never leaves the first process
    (docs/spec/boot-protocol.md, Startup done). */
#define BOOTSTRAP_DONE_SIZE 4

/** The most bytes one debug_write writes. The kernel copies them all
    before it writes any, so a call that fails on its buffer writes
    nothing (S7). */
#define DEBUG_WRITE_MAX_BYTES 256

/** The most bytes one random_read gives: eight 256-bit keys, more than
    any one key or salt needs, and few enough to generate on the kernel
    stack, which random_read fills since it generates with interrupts
    disabled and so cannot take a page fault on a user buffer. */
#define RANDOM_READ_MAX_BYTES 256

#ifndef __ASSEMBLER__

#include "types.h"

/** A deadline that never passes (O9). */
#define DEADLINE_INFINITE 0xffffffffffffffffULL

/** What channel_read writes for each handle it moves into the reader's
    table (ipc.md I2). */
typedef struct {
    UINT32 handle; /**< the handle's value in the reader's table */
    UINT32 type;   /**< its object's type, a TYPE_ value */
    UINT32 rights; /**< the rights it carries */
} RECEIVED_HANDLE;

/** A handle channel_write moves, with the rights it travels with, or
    RIGHTS_SAME (ipc.md I2). */
typedef struct {
    UINT32 handle; /**< a handle in the writer's table, which leaves it */
    /** the rights it travels with, a subset of its own, or
        RIGHTS_SAME */
    UINT32 rights;
} HANDLE_TRANSFER;

/** One object of object_wait_many: the handle and the signals waited for,
    and the signals the call observed, which it writes (O9). */
typedef struct {
    UINT32 handle;   /**< the object's handle, with RIGHT_WAIT */
    UINT32 signals;  /**< the signals waited for */
    UINT32 observed; /**< the signals the call observed */
} WAIT_ENTRY;

/** What job_create grants a new job, reserved from what its parent has
    left (O6): the message-signalled interrupt vectors that the processes
    in it and its own child jobs may hold at once. */
typedef struct {
    /** The message-signalled interrupt vectors the job's processes and
        child jobs may hold at once. */
    UINT32 interrupt_vectors;
} JOB_LIMITS;

/** A boot file the first process's bootstrap message carries a section
    for: its size and its name. */
typedef struct {
    /** The file's size in bytes; an empty file's section is still one
        page. */
    UINT64 size;
    /** The name as the firmware reported it, ASCII and NUL-terminated. */
    char name[BOOT_FILE_NAME_SIZE];
} BOOTSTRAP_FILE;

/** The PCI Express configuration space the ACPI MCFG table describes, one
    range per segment group and bus range: bus b's configuration space
    lies 1 MiB per bus from base, the space of bus start_bus. */
typedef struct {
    UINT64 base;     /**< the physical address of bus start_bus's space */
    UINT16 segment;  /**< the PCI segment group */
    UINT8 start_bus; /**< the first bus the range covers */
    UINT8 end_bus;   /**< the last bus the range covers */
    UINT32 reserved; /**< reserved, 0 */
} ECAM_RANGE;

/** The screen nova-boot drew on (docs/spec/console.md C1): the physical
    address of pixel (0, 0), the size in pixels, the pixels per scan line,
    and the EFI_GRAPHICS_PIXEL_FORMAT, 0 (byte 0 red) or 1 (byte 0
    blue). Pixel (x, y) is the 4 bytes at base + 4 * (y * stride + x). */
typedef struct {
    UINT64 base;   /**< the physical address of pixel (0, 0) */
    UINT32 width;  /**< the width in pixels */
    UINT32 height; /**< the height in pixels */
    UINT32 stride; /**< the pixels per scan line */
    /** the EFI_GRAPHICS_PIXEL_FORMAT, 0 (byte 0 red) or 1 (byte 0
        blue) */
    UINT32 format;
} BOOTSTRAP_DISPLAY;

/** The platform record that starts the first process's bootstrap
    message's bytes: the PCI Express configuration space and the
    screen. */
typedef struct {
    UINT32 ecam_count; /**< the entries of ecam filled in */
    UINT32 reserved;   /**< reserved, 0 */
    /** the configuration space ranges, from the ACPI MCFG table */
    ECAM_RANGE ecam[BOOTSTRAP_ECAM_RANGES_MAX];
    BOOTSTRAP_DISPLAY display; /**< the screen nova-boot drew on */
} BOOTSTRAP_PLATFORM;

/** What the kernel's background holds: which background, at what size
    and in what pixel format. With BOOTSTRAP_BACKGROUND_NONE every other
    field is 0. */
typedef struct {
    /** BOOTSTRAP_BACKGROUND_LIMB, or BOOTSTRAP_BACKGROUND_NONE */
    UINT32 background;
    UINT32 width;  /**< the image's width in pixels, the screen's */
    UINT32 height; /**< the image's height in pixels, the screen's */
    /** the screen's EFI_GRAPHICS_PIXEL_FORMAT, 0 (byte 0 red) or 1
        (byte 0 blue) */
    UINT32 format;
} BOOTSTRAP_BACKGROUND;

/** The bytes of the kernel's background message. */
typedef struct {
    UINT32 transaction;              /**< 0 */
    BOOTSTRAP_BACKGROUND background; /**< what its sections hold */
} BOOTSTRAP_BACKGROUND_MESSAGE;

_Static_assert(sizeof(BOOTSTRAP_BACKGROUND_MESSAGE) == 20,
               "one layout on every target");

/** The sections an image of width x height pixels takes, every one
    SECTION_MAX_SIZE bytes but the last. */
static inline UINT64 bootstrap_background_sections(UINT32 width,
                                                   UINT32 height)
{
    UINT64 pixels_per_section =
        SECTION_MAX_SIZE / BOOTSTRAP_BACKGROUND_PIXEL_BYTES;

    return ((UINT64)width * height + pixels_per_section - 1) /
           pixels_per_section;
}

/** The 64-bit words of a port packet's payload. */
#define PACKET_PAYLOAD_WORDS 4
/** Where a signal packet's first word holds the signals active when it
    fired: its high 32 bits, above the signals its watch asked for. */
#define PACKET_SIGNAL_ACTIVE_SHIFT 32

/** A port packet (ipc.md I4). An interrupt packet's payload starts with
    the interrupt's time on the monotonic clock in nanoseconds. A signal
    packet's first word holds the signals its watch asked for in its low
    32 bits and the signals active when it fired in its high 32, the
    payload's first and second 4 bytes. */
typedef struct {
    /** chosen by whoever caused the packet: the key of the watch or the
        binding */
    UINT64 key;
    /** PACKET_USER, PACKET_SIGNAL or PACKET_INTERRUPT */
    UINT32 type;
    UINT32 status; /**< 0 for every packet type in the first set */
    /** the payload, as the packet's type gives it */
    UINT64 payload[PACKET_PAYLOAD_WORDS];
} PORT_PACKET;

#endif
#endif
