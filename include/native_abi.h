/* The numbers the kernel and the native layer agree on: status codes and
   call numbers (docs/spec/syscalls.md S7 and S8), handle values, object
   types and rights (docs/spec/objects.md O3 and O5), process states and
   the limits of the calls the kernel implements. The kernel and the
   native layer both include this file, so each number has one
   definition. Assembly includes it too, for the call numbers and status
   codes; the C part is hidden from it, and no value carries an integer
   suffix, since TinyCC's assembler evaluates them. */

#ifndef NOVA_NATIVE_ABI_H
#define NOVA_NATIVE_ABI_H

/* Status codes (S7). */
#define OK 0
#define ERR_NOT_SUPPORTED 1
#define ERR_INVALID_ARGS 2
#define ERR_BAD_ADDRESS 3
#define ERR_BAD_HANDLE 4
#define ERR_WRONG_TYPE 5
#define ERR_ACCESS_DENIED 6
#define ERR_NO_MEMORY 7
#define ERR_OUT_OF_RANGE 8
#define ERR_SHOULD_WAIT 9
#define ERR_TIMED_OUT 10
#define ERR_CANCELED 11
#define ERR_PEER_CLOSED 12
#define ERR_BUFFER_TOO_SMALL 13
#define ERR_BAD_STATE 14
#define ERR_NO_RESOURCES 15

/* Status codes only I/O completions carry (S7): a name a directory does
   not hold, a name it holds already, and data that fails the check of
   its digest. */
#define ERR_NOT_FOUND 16
#define ERR_ALREADY_EXISTS 17
#define ERR_DATA_INTEGRITY 18

/* The numbers of the calls this kernel implements (S8). Number 0 is
   reserved and names no call. */
#define CALL_HANDLE_CLOSE 1
#define CALL_HANDLE_DUPLICATE 2
#define CALL_OBJECT_WAIT_ONE 6
#define CALL_OBJECT_WAIT_MANY 7
#define CALL_OBJECT_WAIT_ASYNC 8
#define CALL_CHANNEL_CREATE 10
#define CALL_CHANNEL_WRITE 11
#define CALL_CHANNEL_READ 12
#define CALL_PORT_CREATE 14
#define CALL_PORT_WAIT 16
#define CALL_CLOCK_MONOTONIC 17
#define CALL_JOB_CREATE 18
#define CALL_PROCESS_CREATE 19
#define CALL_PROCESS_START 20
#define CALL_PROCESS_EXIT 21
#define CALL_PROCESS_INFO 22
#define CALL_THREAD_CREATE 23
#define CALL_THREAD_START 24
#define CALL_THREAD_EXIT 25
#define CALL_SECTION_CREATE 27
#define CALL_PROCESS_MAP 28
#define CALL_PROCESS_UNMAP 29
#define CALL_RESOURCE_CREATE 31
#define CALL_SECTION_CREATE_PHYSICAL 32
#define CALL_INTERRUPT_CREATE 33
#define CALL_INTERRUPT_BIND 34
#define CALL_INTERRUPT_ACK 35
#define CALL_INITIATOR_CREATE 37
#define CALL_SECTION_PIN 38
#define CALL_PIN_RELEASE 39
#define CALL_INITIATOR_RELEASE_QUARANTINE 40
#define CALL_DEBUG_WRITE 41
#define CALL_THREAD_SET_TLS_BASE 44

/* The vector of the 32-bit lane's gate, which a thread of a 32-bit
   process enters with INT (S11). */
#define COMPAT_GATE_VECTOR 0x80

/* A handle value holds its table index in bits 31:8 and a generation
   from 1 to 255 in bits 7:0 (O3). Generation 0 is never issued, so 0 is
   never a valid handle. */
#define HANDLE_INVALID 0
#define HANDLE_GENERATION_BITS 8
#define HANDLE_GENERATION_MASK 0xff

/* Object types as channel_read reports them, numbered in the order of
   the first-set table of objects.md O2: process 1, thread 2, section 3,
   event 4, channel 5, port 6, job 7, resource 8, interrupt 9, initiator
   10, pin 11. These are the types this kernel implements. */
#define TYPE_PROCESS 1
#define TYPE_THREAD 2
#define TYPE_SECTION 3
#define TYPE_CHANNEL 5
#define TYPE_PORT 6
#define TYPE_JOB 7
#define TYPE_RESOURCE 8
#define TYPE_INTERRUPT 9
#define TYPE_INITIATOR 10
#define TYPE_PIN 11

/* Rights (O5): bits 16 to 19 are common to every type, bits 0 to 15
   specific to one. RIGHTS_SAME keeps a source's rights. */
#define RIGHT_DUPLICATE 0x10000
#define RIGHT_TRANSFER 0x20000
#define RIGHT_WAIT 0x40000
#define RIGHT_SIGNAL 0x80000
#define RIGHTS_SAME 0xffffffff

#define PROCESS_RIGHT_MAP 0x1
#define PROCESS_RIGHT_CREATE_THREAD 0x2
#define PROCESS_RIGHT_TERMINATE 0x4
#define PROCESS_RIGHT_INSPECT 0x8

#define THREAD_RIGHT_START 0x1
#define THREAD_RIGHT_TERMINATE 0x2

#define SECTION_RIGHT_READ 0x1
#define SECTION_RIGHT_WRITE 0x2
#define SECTION_RIGHT_EXECUTE 0x4
#define SECTION_RIGHT_PIN 0x8

#define CHANNEL_RIGHT_READ 0x1
#define CHANNEL_RIGHT_WRITE 0x2

#define PORT_RIGHT_QUEUE 0x1
#define PORT_RIGHT_READ 0x2

#define JOB_RIGHT_CREATE_PROCESS 0x1
#define JOB_RIGHT_CREATE_JOB 0x2
#define JOB_RIGHT_TERMINATE 0x4
#define JOB_RIGHT_WRITE_EXECUTE 0x8

#define RESOURCE_RIGHT_USE 0x1

#define INTERRUPT_RIGHT_BIND 0x1
#define INTERRUPT_RIGHT_ACK 0x2

#define INITIATOR_RIGHT_PIN 0x1
#define INITIATOR_RIGHT_RELEASE 0x2

/* Signals (O9): a process's and a thread's, a channel endpoint's, and
   the synthetic bit 23 a wait observes on an object whose handle closed
   during the wait. */
#define HANDLE_CLOSED 0x800000
#define PROCESS_TERMINATED 0x1
#define THREAD_TERMINATED 0x1
#define CHANNEL_READABLE 0x1
#define CHANNEL_WRITABLE 0x2
#define CHANNEL_PEER_CLOSED 0x4

/* process_create's options (S8): the process's threads run 32-bit code
   in compatibility mode and may enter by the 32-bit lane (S11); and its
   mappings may be writable and executable at once, which the job handle
   must carry JOB_RIGHT_WRITE_EXECUTE for (O5). */
#define PROCESS_OPTION_32_BIT 0x1
#define PROCESS_OPTION_WRITE_EXECUTE 0x2

/* process_map's protection: read, read-write, read-execute, or all
   three; any other combination is refused. */
#define MAP_READ 0x1
#define MAP_WRITE 0x2
#define MAP_EXECUTE 0x4

/* A process's state as process_info reports it: created and not yet
   started, started, or terminated. Every value is nonzero, since a
   failed call reports 0 (S7). */
#define PROCESS_STATE_NEW 1
#define PROCESS_STATE_RUNNING 2
#define PROCESS_STATE_TERMINATED 3

/* The most bytes and handles one message carries, and the most messages
   an endpoint's queue holds (ipc.md I1). */
#define CHANNEL_MAX_BYTES 65536
#define CHANNEL_MAX_HANDLES 64
#define CHANNEL_MAX_CAPACITY 1024

/* The most user packets a port holds (ipc.md I4), and the packet types. */
#define PORT_MAX_CAPACITY 4096
#define PACKET_USER 0
#define PACKET_SIGNAL 1
#define PACKET_INTERRUPT 2

/* The memory types section_create_physical offers (ipc.md I6): strong
   uncacheable, for registers, and write-combining, for framebuffers. */
#define CACHE_UNCACHED 0
#define CACHE_WRITE_COMBINING 1

/* What a device does with pinned pages (ipc.md I8), for section_pin. */
#define PIN_DEVICE_READ 0x1
#define PIN_DEVICE_WRITE 0x2

/* Interrupt sources as interrupt_create names them and an IRQ resource
   covers them (ipc.md I7): ACPI global system interrupt g is source g,
   and the message-signalled interrupts of bus requester ID r (ipc.md I8)
   are source INTERRUPT_SOURCE_MSI + r. */
#define INTERRUPT_SOURCE_MSI 0x100000000
#define INTERRUPT_SOURCE_LIMIT 0x200000000

/* The named system privileges a system resource's range counts, from 0
   (ipc.md I6); in the first set only the debug log. */
#define SYSTEM_PRIVILEGE_DEBUG_LOG 0
#define SYSTEM_PRIVILEGES 1

/* The page size: every section's size, every offset into one and every
   address a section is mapped at is a multiple of it (syscalls.md S8). */
#define PAGE_SIZE 4096

/* The most objects one object_wait_many watches (O9). */
#define WAIT_MANY_MAX 64

/* The largest section section_create makes: its page list fills one
   page. */
#define SECTION_MAX_SIZE 0x200000

/* The first process's bootstrap message (ipc.md I6): the root system
   resource, the process itself, the root job, the root MMIO, IRQ and DMA
   resources, a read-only section over the store image nova-boot loaded
   (storage.md F16), then one read-only section for each other file
   nova-boot read from \nova\. Its bytes are a BOOTSTRAP_PLATFORM, then a
   BOOTSTRAP_FILE for the store image and one for each file, in the order
   of the sections. A name is ASCII, NUL-terminated, as the firmware
   reported it. The boot files are as many as the message has room for,
   one handle each. */
#define BOOTSTRAP_ROOT_MMIO 3
#define BOOTSTRAP_ROOT_IRQ 4
#define BOOTSTRAP_ROOT_DMA 5
#define BOOTSTRAP_STORE_IMAGE 6
#define BOOTSTRAP_FIRST_FILE 7
#define BOOTSTRAP_ECAM_RANGES_MAX 8
#define BOOT_FILES_MAX (CHANNEL_MAX_HANDLES - BOOTSTRAP_FIRST_FILE)
#define BOOT_FILE_NAME_SIZE 32

/* The first process reports that its startup is done with one message
   on its bootstrap channel: BOOTSTRAP_DONE_SIZE bytes, a transaction ID
   of 0 (ipc.md I3), and no handles. The kernel holds the channel's other
   end until the report comes, and the first process's endpoint carries
   CHANNEL_RIGHT_READ, CHANNEL_RIGHT_WRITE and RIGHT_WAIT but not
   RIGHT_TRANSFER, so it never leaves the first process
   (docs/spec/boot-protocol.md, Startup done). */
#define BOOTSTRAP_DONE_SIZE 4

/* The most bytes one debug_write writes. The kernel copies them all
   before it writes any, so a call that fails on its buffer writes
   nothing (S7). */
#define DEBUG_WRITE_MAX_BYTES 256

#ifndef __ASSEMBLER__

#include "types.h"

/* A deadline that never passes (O9). */
#define DEADLINE_INFINITE 0xffffffffffffffffULL

/* What channel_read writes for each handle it moves into the reader's
   table (ipc.md I2). */
typedef struct {
    UINT32 handle;
    UINT32 type;
    UINT32 rights;
} RECEIVED_HANDLE;

/* A handle channel_write moves, with the rights it travels with, or
   RIGHTS_SAME (ipc.md I2). */
typedef struct {
    UINT32 handle;
    UINT32 rights;
} HANDLE_TRANSFER;

/* One object of object_wait_many: the handle and the signals waited for,
   and the signals the call observed, which it writes (O9). */
typedef struct {
    UINT32 handle;
    UINT32 signals;
    UINT32 observed;
} WAIT_ENTRY;

/* What job_create grants a new job, reserved from what its parent has
   left (O6): the message-signalled interrupt vectors that the processes
   in it and its own child jobs may hold at once. */
typedef struct {
    UINT32 interrupt_vectors;
} JOB_LIMITS;

typedef struct {
    UINT64 size;
    char name[BOOT_FILE_NAME_SIZE];
} BOOTSTRAP_FILE;

/* The PCI Express configuration space the ACPI MCFG table describes, one
   range per segment group and bus range: bus b's configuration space
   lies 1 MiB per bus from base, the space of bus start_bus. */
typedef struct {
    UINT64 base;
    UINT16 segment;
    UINT8 start_bus;
    UINT8 end_bus;
    UINT32 reserved;
} ECAM_RANGE;

typedef struct {
    UINT32 ecam_count;
    UINT32 reserved;
    ECAM_RANGE ecam[BOOTSTRAP_ECAM_RANGES_MAX];
} BOOTSTRAP_PLATFORM;

/* A port packet (ipc.md I4). An interrupt packet's payload starts with
   the interrupt's time on the monotonic clock in nanoseconds. A signal
   packet's first word holds the signals its watch asked for in its low
   32 bits and the signals active when it fired in its high 32, the
   payload's first and second 4 bytes. */
#define PACKET_PAYLOAD_WORDS 4
#define PACKET_SIGNAL_ACTIVE_SHIFT 32

typedef struct {
    UINT64 key;
    UINT32 type;
    UINT32 status;
    UINT64 payload[PACKET_PAYLOAD_WORDS];
} PORT_PACKET;

#endif
#endif
