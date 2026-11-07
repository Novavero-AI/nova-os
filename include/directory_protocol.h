/* The directory protocol (docs/spec/storage.md F1 to F4) as messages,
   shaped as ipc.md I9 shapes I/O requests: every message on a directory
   or a file starts with an IO_REQUEST, every reply with an IO_REPLY
   carrying the request's transaction and a status of syscalls.md S7.
   Both are little-endian, and every name and path is UTF-8 with '/'
   between components, relative to the directory it is sent to.

   Open: on a directory, an IO_REQUEST with operation IO_OPEN, the rights
   wanted, length the path's bytes, which follow, and one handle: an
   endpoint of a new channel whose other end the client keeps. A path
   holds no empty, "." or ".." component (O8, rule 2: the client resolves
   them before sending); an empty path opens the directory itself. The
   rights wanted are at most the directory's, and READ, WRITE and WAIT on
   the endpoint. The server's one reply goes on the endpoint, never the
   directory: an IO_REPLY and the node's IO_ATTRIBUTES. Once the path
   reaches a symbolic link, the server stops there (F3): the attributes
   are the link's, its target and then the part of the path after it
   follow them, and the server closes the endpoint, as it does after a
   failure. Otherwise it serves the endpoint as what it opened. An open
   that would take its client past the channels a server allows it, or
   start a client the server has no room for, fails with
   ERR_NO_RESOURCES (storage.md F17).

   Create: an open's flags are IO_OPEN_ bits. With IO_OPEN_CREATE, a
   missing last component of the path is created, a file, or a directory
   with IO_OPEN_DIRECTORY, in a directory that exists; this needs WRITE
   on the directory the open is sent to, whatever rights are wanted for
   the endpoint. A node already there is opened, unless IO_OPEN_EXCLUSIVE
   is set or it is not of the kind the open would create, which fails
   with ERR_ALREADY_EXISTS. IO_OPEN_EXCLUSIVE and IO_OPEN_DIRECTORY
   without IO_OPEN_CREATE, and any other bit, fail with
   ERR_INVALID_ARGS. A server that creates nothing refuses
   IO_OPEN_CREATE on its own directories.

   Read a directory: IO_READ_DIR with offset the cookie, 0 to start, and
   length the entries wanted. The reply's information is the cookie to
   continue from, and IO_DIRECTORY_ENTRYs follow it in bytewise order of
   name, each followed by its name, as many as fill the reply; one with
   none is the end. Needs LIST.

   Attributes: IO_GET_ATTRIBUTES, on a directory or a file; the reply is
   followed by the node's IO_ATTRIBUTES.

   Read a file: IO_READ with offset where and length the bytes wanted;
   the reply's information is the bytes read, which follow it, fewer at
   the end of the file. Needs READ.

   Section of a file: IO_GET_SECTION with the rights wanted, READ and
   EXECUTE among the open's; the reply's information is the file's size,
   and it carries one handle, a section holding the file's bytes from
   its start with SECTION_RIGHT_READ, SECTION_RIGHT_EXECUTE if EXECUTE
   was wanted, and never SECTION_RIGHT_WRITE (F9).

   Write a file: IO_WRITE with offset where and length the bytes, at most
   IO_TRANSFER_MAX, which follow; the reply's information is the bytes
   written, all of them. A write past the end extends the file, and the
   bytes between the old end and offset read as zero. Needs WRITE.

   Truncate a file: IO_TRUNCATE with offset the file's new size and
   length 0. Bytes past it are dropped; a size past the end extends the
   file with zeros. Needs WRITE.

   Flush: IO_FLUSH, on a file or a directory, with length 0. The reply
   comes once every change the volume holds, through any channel, is
   durable: the server commits its transaction group (F6), and a failed
   commit is the reply's status. Needs WRITE.

   Remove: IO_REMOVE on a directory with length the path's bytes, which
   follow; it removes the file or empty directory there. A directory that
   is not empty fails with ERR_BAD_STATE. Channels already open on the
   node removed answer ERR_NOT_FOUND from then on. Needs WRITE.

   Rename: IO_RENAME on a directory with offset the bytes of the path
   moved from and length those of both paths, which follow, from first,
   then to; both are resolved against the directory, and to's parent must
   exist. A node already at to fails with ERR_ALREADY_EXISTS, unless
   flags hold IO_RENAME_REPLACE: then a file replaces a file and a
   directory an empty directory, in the same step, and any other pair
   fails with ERR_ALREADY_EXISTS. Moving a directory into itself or below
   it fails with ERR_INVALID_ARGS; renaming a node to its own path does
   nothing. Channels open on the node moved stay open on it. Needs
   WRITE.

   A change by write, truncate, create, remove or rename is seen at once
   by every channel of the volume, and is durable once a flush that
   follows it has answered, or the server has committed on its own; a
   crash keeps every commit whole or none of it (F6).

   Publish, replace and unpublish (F4), on the namespace server's own
   directories: IO_PUBLISH, IO_REPLACE or IO_UNPUBLISH with length the
   name's bytes, one path component, which follow; publish and replace
   carry one handle, the directory channel published, which needs
   CHANNEL_RIGHT_WRITE. Needs PUBLISH.

   A message shorter than an IO_REQUEST ends its channel: the server
   closes it unanswered. Any other request the server cannot read as one
   of these is answered with ERR_INVALID_ARGS or ERR_NOT_SUPPORTED, and
   the handles it carries are closed. An open is answered on its endpoint
   unless its length is not its path's bytes or it carries no endpoint
   with the rights a server needs; such an open is answered on the
   directory. Every error reply is shorter than a request, and a reply's
   status 0 is no operation, so a server given both ends of one channel
   ends them rather than answering its own replies. */

#ifndef NOVA_DIRECTORY_PROTOCOL_H
#define NOVA_DIRECTORY_PROTOCOL_H

#include "native_abi.h"

#define IO_OPEN 1
#define IO_READ_DIR 2
#define IO_GET_ATTRIBUTES 3
#define IO_PUBLISH 4
#define IO_REPLACE 5
#define IO_UNPUBLISH 6
#define IO_READ 7
#define IO_GET_SECTION 8
#define IO_WRITE 9
#define IO_TRUNCATE 10
#define IO_FLUSH 11
#define IO_REMOVE 12
#define IO_RENAME 13

/* An open's flags: create a missing node, fail if one is there, and
   create a directory rather than a file. */
#define IO_OPEN_CREATE 0x1
#define IO_OPEN_EXCLUSIVE 0x2
#define IO_OPEN_DIRECTORY 0x4
#define IO_OPEN_FLAGS (IO_OPEN_CREATE | IO_OPEN_EXCLUSIVE | IO_OPEN_DIRECTORY)

/* A rename's flags: replace the node already at the destination. */
#define IO_RENAME_REPLACE 0x1

/* The rights a directory or file channel carries, which its server
   records and checks (O8): read a file, write or create, map a file
   executable, list a directory, publish in it. */
#define IO_RIGHT_READ 0x1
#define IO_RIGHT_WRITE 0x2
#define IO_RIGHT_EXECUTE 0x4
#define IO_RIGHT_LIST 0x8
#define IO_RIGHT_PUBLISH 0x10
#define IO_RIGHTS_ALL                                                    \
    (IO_RIGHT_READ | IO_RIGHT_WRITE | IO_RIGHT_EXECUTE | IO_RIGHT_LIST |  \
     IO_RIGHT_PUBLISH)

/* The rights a server needs on an open's endpoint: to read requests,
   write replies and wait for both. */
#define IO_ENDPOINT_RIGHTS                                                \
    (CHANNEL_RIGHT_READ | CHANNEL_RIGHT_WRITE | RIGHT_WAIT)

/* A node's kind (F2). */
#define IO_KIND_DIRECTORY 1
#define IO_KIND_FILE 2
#define IO_KIND_LINK 3

/* An attribute record's flags: the file is executable; digest holds the
   node's content digest. */
#define IO_EXECUTABLE 0x1
#define IO_HAS_DIGEST 0x2

/* The most bytes a reply's data holds, and the longest path a request
   carries: PATH_MAX of Linux, which a path a program builds rarely
   passes. */
#define IO_TRANSFER_MAX (CHANNEL_MAX_BYTES / 2)
#define IO_PATH_MAX 4096

#define IO_DIGEST_SIZE 32

typedef struct {
    UINT32 transaction;
    UINT32 operation;
    UINT64 offset;
    UINT32 length;
    UINT32 rights;
    UINT32 flags; /* IO_OPEN_ or IO_RENAME_ bits; 0 for any other */
    UINT32 reserved;
} IO_REQUEST;

typedef struct {
    UINT32 transaction;
    UINT32 status;
    UINT64 information;
} IO_REPLY;

/* What an open or IO_GET_ATTRIBUTES reports (F2). A store node has no
   time and no node ID, which this record leaves out until a data volume
   has them. */
typedef struct {
    UINT32 kind;
    UINT32 flags;
    UINT64 size;      /* a file's bytes, a link's target's */
    UINT8 digest[IO_DIGEST_SIZE];
    UINT32 rights;    /* what the open carries */
    UINT32 remaining; /* after a link: the bytes of the path not resolved */
} IO_ATTRIBUTES;

typedef struct {
    UINT16 length; /* the name's bytes, which follow */
    UINT8 kind;
    UINT8 reserved;
} IO_DIRECTORY_ENTRY;

_Static_assert(sizeof(IO_REQUEST) == 32, "one layout everywhere");
_Static_assert(sizeof(IO_REPLY) == 16, "one layout everywhere");
_Static_assert(sizeof(IO_ATTRIBUTES) == 56, "one layout everywhere");
_Static_assert(sizeof(IO_DIRECTORY_ENTRY) == 4, "one layout everywhere");

#endif
