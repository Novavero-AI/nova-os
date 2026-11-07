/** @file
    A process's view of the namespace and the client side of the directory
    protocol (docs/spec/storage.md F3, F5; directory_protocol.h): the path
    functions resolve an absolute path by the view entry with the longest
    matching prefix and send the rest of it to that entry's directory.
    They resolve "." and ".." textually first, never above "/", and follow
    symbolic links themselves, against the same view, so a path reaches
    only what the view's directories hold. They keep their buffers in
    static memory and serve one thread at a time. */

#ifndef NOVA_PATH_H
#define NOVA_PATH_H

#include "directory_protocol.h"
#include "launch.h"

/** The most links one open follows, Linux's MAXSYMLINKS. */
#define SYMLINK_MAX_HOPS 40

/** The most entries one listing takes from its server, so that a server
    whose listing never ends fails the call instead of holding it
    (storage.md F1): 65,536, 1,024 full replies, sixteen times the most
    paths the store service lists at its root (STORE_PATHS_MAX, store.c). */
#define LIST_ENTRIES_MAX 65536

/** A view entry: a prefix, and the directory it names. */
typedef struct {
    UINT32 directory;                /**< the directory's channel */
    UINT32 length;                   /**< the prefix's bytes */
    UINT8 prefix[LAUNCH_PREFIX_MAX]; /**< the prefix */
} VIEW_ENTRY;

/** A process's view of the namespace. */
typedef struct {
    UINT32 count;                        /**< the entries filled in */
    VIEW_ENTRY entries[LAUNCH_VIEW_MAX]; /**< the entries */
} VIEW;

/** Add the directory to view under prefix, an absolute path, which is
    resolved as a path is; "/" names the whole namespace. Returns OK;
    ERR_INVALID_ARGS for a prefix that is not absolute or is too long;
    ERR_ALREADY_EXISTS if the view holds it; ERR_OUT_OF_RANGE if the view
    is full. */
UINT32 view_add(VIEW *view, const char *prefix, UINT32 directory);

/** Open path, absolute and NUL-terminated, with rights, IO_RIGHT_ bits,
    following each link on the way and at its end. On OK, *channel is the
    open node's channel and *attributes what the server reported. Returns
    the server's status; ERR_NOT_FOUND for a path no entry covers or a
    link to one; ERR_INVALID_ARGS for a path that is not absolute or too
    long; ERR_OUT_OF_RANGE after SYMLINK_MAX_HOPS links; ERR_BAD_STATE
    for a reply that breaks the protocol. */
UINT32 view_open(const VIEW *view, const char *path, UINT32 rights,
                 UINT32 *channel, IO_ATTRIBUTES *attributes);

/** view_open with flags, IO_OPEN_ bits: with IO_OPEN_CREATE, a missing
    file, or directory with IO_OPEN_DIRECTORY, at the path's end is
    created (directory_protocol.h). */
UINT32 view_create(const VIEW *view, const char *path, UINT32 rights,
                   UINT32 flags, UINT32 *channel,
                   IO_ATTRIBUTES *attributes);

/** Call each(context, name, length, kind) for every entry of the
    directory at path, in bytewise order of name: the server's entries,
    with the next component of any view prefix nested below path (F5,
    rule 4). name lasts only for the call, and each calls no path
    function. Returns OK or a status as view_open's; ERR_OUT_OF_RANGE
    once the server has given LIST_ENTRIES_MAX entries and the listing
    has not ended, having called each for those. */
UINT32 view_list(const VIEW *view, const char *path,
                 void (*each)(void *context, const UINT8 *name,
                              UINT32 length, UINT32 kind),
                 void *context);

/** Send operation, IO_OPEN or a server's own open such as the store
    service's STORE_OPEN_CLOSURE, for the length bytes at path on
    directory, with rights, and on OK give the opened node's channel and
    attributes. A path that reaches a link is not followed here:
    ERR_NOT_SUPPORTED. */
UINT32 io_open(UINT32 directory, UINT32 operation, const UINT8 *path,
               UINT32 length, UINT32 rights, UINT32 *channel,
               IO_ATTRIBUTES *attributes);

/** Read up to length bytes, at most IO_TRANSFER_MAX, from offset of the
    file open on channel into buffer, and report how many in *count. */
UINT32 io_read(UINT32 channel, UINT64 offset, void *buffer, UINT32 length,
               UINT32 *count);

/** Write length bytes, at most IO_TRANSFER_MAX, from buffer at offset of
    the file open on channel, and report how many in *count. */
UINT32 io_write(UINT32 channel, UINT64 offset, const void *buffer,
                UINT32 length, UINT32 *count);

/** Set the size of the file open on channel. */
UINT32 io_truncate(UINT32 channel, UINT64 size);

/** Return once every change to the volume channel's node is on is
    durable. */
UINT32 io_flush(UINT32 channel);

/** Remove the file or empty directory at the length bytes of path below
    the directory open on channel. */
UINT32 io_remove(UINT32 channel, const UINT8 *path, UINT32 length);

/** Move the node at from to to, each of its length's bytes and below the
    directory open on channel, with flags, IO_RENAME_ bits. */
UINT32 io_rename(UINT32 channel, const UINT8 *from, UINT32 from_length,
                 const UINT8 *to, UINT32 to_length, UINT32 flags);

/** A section holding the file open on channel, with rights, IO_RIGHT_READ
    and IO_RIGHT_EXECUTE, and the file's size. *received is the handle as
    channel_read reported it, with its type and rights. */
UINT32 io_get_section(UINT32 channel, UINT32 rights,
                      RECEIVED_HANDLE *received, UINT64 *size);

/** Send a request of length bytes at request, carrying handle_count
    handles, on channel, setting its transaction. The handles move only
    if it returns OK (ipc.md I2). */
UINT32 io_send(UINT32 channel, IO_REQUEST *request, UINT32 length,
               const HANDLE_TRANSFER *handles, UINT32 handle_count);

/** Wait for the reply to request on channel; its bytes go to reply, at
    most capacity, and its handle, if it carries one, to *received, which
    is closed again unless the status is OK. Returns the reply's status,
    or the call's that failed; ERR_BAD_STATE for a reply that breaks the
    protocol. */
UINT32 io_receive(UINT32 channel, const IO_REQUEST *request, UINT8 *reply,
                  UINT32 capacity, UINT32 *reply_length,
                  RECEIVED_HANDLE *received);

/** io_send of a request that carries no handle, then io_receive. */
UINT32 io_call(UINT32 channel, IO_REQUEST *request, UINT32 length,
               UINT8 *reply, UINT32 capacity, UINT32 *reply_length,
               RECEIVED_HANDLE *received);

#endif
