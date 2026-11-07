/* The file protocol between a 32-bit process and the server of its view
   of storage, a stand-in until a server speaks the directory protocol
   (directory_protocol.h) for directories a process writes
   (docs/spec/compat.md, Files; ipc.md I9 gives its shape). Every message
   starts with a FILE_REQUEST or a FILE_REPLY; both are little-endian and
   laid out alike on x86-64 and in 32-bit code.

   Open: on a directory channel, a FILE_REQUEST with operation
   FILE_OPERATION_OPEN, the disposition and access in flags, length the
   name's bytes, which follow (UTF-8, '/' between components, relative to
   the directory), and one handle: an endpoint of a new channel, whose
   other end the client keeps. The server's first message on that
   endpoint is the open's FILE_REPLY, with a FILE_OPENED-style outcome in
   information; after a failure it closes the endpoint.

   Read and write: on the opened channel, a FILE_REQUEST with offset
   FILE_OFFSET_CURRENT or a position, and length the bytes wanted or the
   bytes that follow it. The FILE_REPLY's information is the bytes moved;
   a read's data follows it. A read at the end of the file moves none.

   Position: on the opened channel, a FILE_REQUEST with offset the new
   position, or FILE_OFFSET_CURRENT to keep the current one, and length
   0. It needs the access a read or a write needs. The FILE_REPLY's
   information is the position after it.

   Size: on the opened channel, a FILE_REQUEST with length 0, whose
   FILE_REPLY's information is the file's size in bytes. It needs no
   access.

   An open's endpoint carries the rights to read, write and wait, or the
   server closes it. A message on an opened channel that is not one of
   these requests ends the open: the server closes the channel without a
   reply. */

#ifndef NOVA_FILE_PROTOCOL_H
#define NOVA_FILE_PROTOCOL_H

#include "native_abi.h"

#define FILE_OPERATION_OPEN 1
#define FILE_OPERATION_READ 2
#define FILE_OPERATION_WRITE 3
#define FILE_OPERATION_POSITION 4
#define FILE_OPERATION_SIZE 5

/* An open's flags: the disposition, NT's values (nt32.h), in bits 7:0;
   the access wanted; and whether the name matches regardless of ASCII
   case. */
#define FILE_FLAGS_DISPOSITION 0xffu
#define FILE_ACCESS_READ 0x100u
#define FILE_ACCESS_WRITE 0x200u
#define FILE_NAME_ANY_CASE 0x400u

/* A position that means the file's current position, which a read or
   write moves past the bytes it moved. A position given is below
   FILE_OFFSET_LIMIT, since NT's positions are signed (MS-FSCC
   2.4.40). */
#define FILE_OFFSET_CURRENT 0xffffffffffffffffULL
#define FILE_OFFSET_LIMIT 0x8000000000000000ULL

/* The most bytes one read or write moves: what a message holds besides
   its header. */
#define FILE_TRANSFER_MAX (CHANNEL_MAX_BYTES / 2)

/* A reply's status. */
#define FILE_OK 0
#define FILE_NOT_FOUND 1
#define FILE_EXISTS 2
#define FILE_DENIED 3
#define FILE_INVALID 4
#define FILE_FULL 5

typedef struct {
    UINT32 transaction;
    UINT32 operation;
    UINT64 offset;
    UINT32 length;
    UINT32 flags;
} FILE_REQUEST;

typedef struct {
    UINT32 transaction;
    UINT32 status;
    UINT64 information;
} FILE_REPLY;

_Static_assert(sizeof(FILE_REQUEST) == 24, "one layout on both targets");
_Static_assert(sizeof(FILE_REPLY) == 16, "one layout on both targets");

#endif
