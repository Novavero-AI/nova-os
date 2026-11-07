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

/* The numbers of the calls this kernel implements (S8). Number 0 is
   reserved and names no call. */
#define CALL_HANDLE_CLOSE 1
#define CALL_CHANNEL_READ 12
#define CALL_CLOCK_MONOTONIC 17
#define CALL_PROCESS_EXIT 21
#define CALL_PROCESS_INFO 22
#define CALL_THREAD_EXIT 25
#define CALL_DEBUG_WRITE 41

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
#define TYPE_CHANNEL 5
#define TYPE_RESOURCE 8

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

#define CHANNEL_RIGHT_READ 0x1
#define CHANNEL_RIGHT_WRITE 0x2

#define RESOURCE_RIGHT_USE 0x1

/* A process's state as process_info reports it: created and not yet
   started, started, or terminated. Every value is nonzero, since a
   failed call reports 0 (S7). */
#define PROCESS_STATE_NEW 1
#define PROCESS_STATE_RUNNING 2
#define PROCESS_STATE_TERMINATED 3

/* The most handles one message carries (ipc.md I1). */
#define CHANNEL_MAX_HANDLES 64

/* The most bytes one debug_write writes. The kernel copies them all
   before it writes any, so a call that fails on its buffer writes
   nothing (S7). */
#define DEBUG_WRITE_MAX_BYTES 256

#ifndef __ASSEMBLER__

#include "types.h"

/* What channel_read writes for each handle it moves into the reader's
   table (ipc.md I2). */
typedef struct {
    UINT32 handle;
    UINT32 type;
    UINT32 rights;
} RECEIVED_HANDLE;

#endif
#endif
