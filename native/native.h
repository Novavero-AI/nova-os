/** @file
    The native layer: the C functions through which a user program calls
    the nova-os kernel (docs/spec/syscalls.md S8). Each is a stub in
    native.S that enters the kernel with SYSCALL in the register
    convention of S4. Each returns the call's status (S7), and writes its
    results through the pointers it takes, 0 when the call fails, except
    where a function says otherwise. These are the calls the kernel
    implements. */

#ifndef NOVA_NATIVE_H
#define NOVA_NATIVE_H

#include "native_abi.h"

/** Remove handle from the caller's table and drop its reference. */
UINT32 handle_close(UINT32 handle);

/** Read the first message queued at channel, which needs CHANNEL_RIGHT_READ:
    copy its bytes to bytes and move its handles into the caller's table,
    describing each at handles. Reports the message's sizes in byte_count
    and handle_count, also when it fails with ERR_BUFFER_TOO_SMALL, which
    leaves the message queued. An empty queue gives ERR_SHOULD_WAIT, or
    ERR_PEER_CLOSED once the other end is closed. */
UINT32 channel_read(UINT32 channel, void *bytes, UINT32 byte_capacity,
                    RECEIVED_HANDLE *handles, UINT32 handle_capacity,
                    UINT32 *byte_count, UINT32 *handle_count);

/** The time on the monotonic clock in nanoseconds. */
UINT32 clock_monotonic(UINT64 *nanoseconds);

/** End the calling process with exit_code, which its creator reads with
    process_info. */
_Noreturn void process_exit(UINT32 exit_code);

/** The state of process, PROCESS_STATE_NEW, _RUNNING or _TERMINATED, and
    its exit code, 0 until it has terminated. Needs PROCESS_RIGHT_INSPECT. */
UINT32 process_info(UINT32 process, UINT32 *state, UINT32 *exit_code);

/** End the calling thread; the process ends with exit code 0 if it was
    the last. */
_Noreturn void thread_exit(void);

/** Write count bytes, at most DEBUG_WRITE_MAX_BYTES, to the kernel's debug
    log. Needs RESOURCE_RIGHT_USE on a resource that covers the debug log. */
UINT32 debug_write(UINT32 resource, const void *bytes, UINT64 count);

/** Write count random bytes, at most RANDOM_READ_MAX_BYTES, from the
    kernel's generator to bytes. ERR_BAD_STATE while the generator has no
    seed. */
UINT32 random_read(void *bytes, UINT64 count);

/** Wait until any of signals is active on the object handle names, which
    needs RIGHT_WAIT, or the deadline on the monotonic clock passes
    (ERR_TIMED_OUT), and report the signals observed. If another thread
    closes handle meanwhile, the wait ends with ERR_CANCELED and reports
    HANDLE_CLOSED among them. */
UINT32 object_wait_one(UINT32 handle, UINT32 signals, UINT64 deadline,
                       UINT32 *observed);

/** object_wait_one on up to WAIT_MANY_MAX objects at once, writing each
    entry's observed signals, also when the wait times out or is
    cancelled. */
UINT32 object_wait_many(WAIT_ENTRY *items, UINT32 count, UINT64 deadline);

/** Watch the object handle names, which needs RIGHT_WAIT, once: when any
    of signals is active, now or later, queue one PACKET_SIGNAL carrying
    key on port, which needs PORT_RIGHT_QUEUE. Closing handle first ends
    the watch with no packet. */
UINT32 object_wait_async(UINT32 handle, UINT32 port, UINT64 key,
                         UINT32 signals);

/** A new channel whose endpoints each queue up to capacity messages. */
UINT32 channel_create(UINT32 capacity, UINT32 *first, UINT32 *second);

/** Write a message of byte_count bytes and handle_count handles, which
    leave the caller's table, to channel, which needs CHANNEL_RIGHT_WRITE.
    A full queue at the other end gives ERR_SHOULD_WAIT; a closed one
    ERR_PEER_CLOSED. */
UINT32 channel_write(UINT32 channel, const void *bytes, UINT32 byte_count,
                     const HANDLE_TRANSFER *handles, UINT32 handle_count);

/** A new job, a child of parent, which needs JOB_RIGHT_CREATE_JOB, granted
    limits out of what parent has left (ERR_NO_RESOURCES if it has too
    few). Its handle carries JOB_RIGHT_WRITE_EXECUTE only if parent's
    does. */
UINT32 job_create(UINT32 parent, const JOB_LIMITS *limits, UINT32 *job);

/** A new process in job, which needs JOB_RIGHT_CREATE_PROCESS, with
    PROCESS_OPTION_ bits; PROCESS_OPTION_WRITE_EXECUTE also needs
    JOB_RIGHT_WRITE_EXECUTE. */
UINT32 process_create(UINT32 job, UINT32 options, UINT32 *process);

/** Start process's first thread, thread, at entry with stack, passing it
    bootstrap, which leaves the caller's table, and argument. Needs
    PROCESS_RIGHT_CREATE_THREAD, THREAD_RIGHT_START and RIGHT_TRANSFER on
    the bootstrap handle. */
UINT32 process_start(UINT32 process, UINT32 thread, UINT64 entry,
                     UINT64 stack, UINT32 bootstrap, UINT64 argument);

/** A new thread of process, which needs PROCESS_RIGHT_CREATE_THREAD. */
UINT32 thread_create(UINT32 process, UINT32 *thread);

/** Start thread, a further thread of a running process, at entry with
    stack and its two arguments in RCX and RDX. Needs THREAD_RIGHT_START. A
    process that is terminating starts no thread (ERR_BAD_STATE). */
UINT32 thread_start(UINT32 thread, UINT64 entry, UINT64 stack,
                    UINT64 argument1, UINT64 argument2);

/** A new section of size bytes, rounded up to pages, all zero; options
    must be 0. */
UINT32 section_create(UINT64 size, UINT32 options, UINT32 *section);

/** Map size bytes of section from offset at address in process, which
    needs PROCESS_RIGHT_MAP, with MAP_ protection bits, which need the
    matching section rights. */
UINT32 process_map(UINT32 process, UINT32 section, UINT64 offset,
                   UINT64 address, UINT64 size, UINT32 protection);

/** Remove the mappings that make up the size bytes at address in
    process, which needs PROCESS_RIGHT_MAP: each a whole mapping
    process_map made, with no page of the range left out (ERR_BAD_STATE
    otherwise, removing nothing). */
UINT32 process_unmap(UINT32 process, UINT64 address, UINT64 size);

/** A second handle to handle's object, with rights, RIGHTS_SAME or a
    subset of handle's rights. Needs RIGHT_DUPLICATE. */
UINT32 handle_duplicate(UINT32 handle, UINT32 rights, UINT32 *duplicate);

/** A new port that holds up to capacity user packets. */
UINT32 port_create(UINT32 capacity, UINT32 *port);

/** Take the next packet from port into packet, waiting for one until the
    deadline, an absolute time on the monotonic clock. If another thread
    closes port meanwhile, the wait ends with ERR_CANCELED. Needs
    PORT_RIGHT_READ. */
UINT32 port_wait(UINT32 port, UINT64 deadline, PORT_PACKET *packet);

/** A resource covering [base, base + size) of parent's range, of parent's
    kind. Needs RESOURCE_RIGHT_USE. */
UINT32 resource_create(UINT32 parent, UINT64 base, UINT64 size,
                       UINT32 *resource);

/** A section over the device memory [base, base + size), page aligned,
    that the MMIO resource covers, mapped with cache, CACHE_UNCACHED or
    CACHE_WRITE_COMBINING. Its handle has SECTION_RIGHT_READ and _WRITE,
    never _EXECUTE or _PIN. Needs RESOURCE_RIGHT_USE. */
UINT32 section_create_physical(UINT32 resource, UINT64 base, UINT64 size,
                               UINT32 cache, UINT32 *section);

/** An interrupt for source, which the IRQ resource covers; options must
    be 0. For a message-signalled source, address and data receive the
    message the device is to write; its vector counts against the calling
    process's job's quota until the interrupt's last handle closes
    (ERR_NO_RESOURCES once the quota is used). Needs RESOURCE_RIGHT_USE. */
UINT32 interrupt_create(UINT32 resource, UINT64 source, UINT32 options,
                        UINT32 *interrupt, UINT64 *address, UINT32 *data);

/** Deliver interrupt as PACKET_INTERRUPT packets carrying key to port.
    Needs INTERRUPT_RIGHT_BIND and PORT_RIGHT_QUEUE. */
UINT32 interrupt_bind(UINT32 interrupt, UINT32 port, UINT64 key);

/** The device is serviced: let interrupt deliver its next packet. Needs
    INTERRUPT_RIGHT_ACK. */
UINT32 interrupt_ack(UINT32 interrupt);

/** An initiator for bus requester ID requester, which the DMA resource
    covers. Needs RESOURCE_RIGHT_USE. */
UINT32 initiator_create(UINT32 resource, UINT32 requester,
                        UINT32 *initiator);

/** Pin size bytes of section from offset for initiator's device, which
    options, PIN_DEVICE_READ, PIN_DEVICE_WRITE or both, says reads or
    writes them, and write the device address of each page to addresses.
    Needs
    INITIATOR_RIGHT_PIN, SECTION_RIGHT_PIN and the section rights options
    needs. */
UINT32 section_pin(UINT32 initiator, UINT32 section, UINT64 offset,
                   UINT64 size, UINT32 options, UINT64 *addresses,
                   UINT32 *pin);

/** End pin, asserting that the device no longer uses its pages, and close
    the handle. A pin whose last handle closes otherwise is quarantined. */
UINT32 pin_release(UINT32 pin);

/** Free initiator's quarantined pins, once its device is reset. Needs
    INITIATOR_RIGHT_RELEASE. */
UINT32 initiator_release_quarantine(UINT32 initiator);

#endif
