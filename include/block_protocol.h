/* The block protocol between a block driver and its one client, a file
   system service, shaped as ipc.md I9 shapes I/O requests: every request
   is a BLOCK_REQUEST, answered by one BLOCK_REPLY with the request's
   transaction and a status of syscalls.md S7. Both are little-endian.

   The client's first request is BLOCK_ATTACH, with length the bytes of
   the section it carries as its one handle, a multiple of PAGE_SIZE and
   at most SECTION_MAX_SIZE, with SECTION_RIGHT_READ, _WRITE and _PIN.
   The driver pins the whole section for its device to read and write
   (ipc.md I8) and keeps the pin until it ends; the reply's information is
   the device's capacity in BLOCK_SECTOR_SIZE sectors. Every later
   request names a range of that section, so the bytes move by DMA
   between the device and the client's pages, never through a message.

   BLOCK_READ and BLOCK_WRITE move length bytes, a multiple of
   BLOCK_SECTOR_SIZE from one sector to BLOCK_TRANSFER_MAX, between the
   sectors from sector on and the section from buffer on, a multiple of
   BLOCK_SECTOR_SIZE. A range past the capacity or the section fails with
   ERR_OUT_OF_RANGE; a write to a read-only device with
   ERR_ACCESS_DENIED. A write is volatile when it is answered.

   BLOCK_FLUSH, with sector, length and buffer 0, answers once every
   write answered before it was sent is stable: on the device's
   persistent storage, so that a power cut keeps it (VIRTIO 1.4 5.2.6.2).
   A device that cannot flush fails it with ERR_NOT_SUPPORTED, since its
   writes may be lost in a crash however long ago they were answered.

   The device's failure of a request is ERR_IO. A request before
   BLOCK_ATTACH, or a second one, fails with ERR_BAD_STATE; a message of
   another size, or one that carries a handle it should not, is answered
   with ERR_INVALID_ARGS and its handles closed. Once the client closes
   its end, the driver resets the device, releases its pins and ends. */

#ifndef NOVA_BLOCK_PROTOCOL_H
#define NOVA_BLOCK_PROTOCOL_H

#include "native_abi.h"

#define BLOCK_ATTACH 1
#define BLOCK_READ 2
#define BLOCK_WRITE 3
#define BLOCK_FLUSH 4

#define BLOCK_SECTOR_SIZE 512
#define BLOCK_TRANSFER_MAX (8 * PAGE_SIZE)

/* The rights the client's endpoint carries: to send requests, read
   replies and wait for them, and to travel to the client. */
#define BLOCK_CLIENT_RIGHTS                                              \
    (CHANNEL_RIGHT_READ | CHANNEL_RIGHT_WRITE | RIGHT_WAIT | RIGHT_TRANSFER)

/* The rights the attached section carries. */
#define BLOCK_SECTION_RIGHTS                                             \
    (SECTION_RIGHT_READ | SECTION_RIGHT_WRITE | SECTION_RIGHT_PIN |      \
     RIGHT_TRANSFER)

typedef struct {
    UINT32 transaction;
    UINT32 operation;
    UINT64 sector;
    UINT32 length;
    UINT32 buffer;
} BLOCK_REQUEST;

typedef struct {
    UINT32 transaction;
    UINT32 status;
    UINT64 information;
} BLOCK_REPLY;

_Static_assert(sizeof(BLOCK_REQUEST) == 24, "one layout everywhere");
_Static_assert(sizeof(BLOCK_REPLY) == 16, "one layout everywhere");

#endif
