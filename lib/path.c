/* The path functions (path.h). A path is resolved to its normal form, an
   absolute path with no empty, "." or ".." component held as its
   components each after a '/', the root as no bytes at all. Each reply's
   lengths are checked against the bytes received before any is used. */

#include "path.h"
#include "native.h"

/* The messages an open's channel queues each way: one request, one
   reply, at a time. */
#define OPEN_CAPACITY 1

/* The entries one IO_READ_DIR asks for. */
#define LIST_BATCH 64

/* Transactions a program assigns itself have bit 31 clear (ipc.md I3). */
#define TRANSACTION_MASK 0x7fffffffu

/* The Windows layer's old-style store directory, which a link built on
   Windows names (storage.md F3, F5), with '\' and '/' alike, and where
   it is in the view. */
#define OLD_STORE "C:/nix/store"
#define OLD_STORE_LENGTH 12
#define STORE_PREFIX "/store"
#define STORE_PREFIX_LENGTH 6

static UINT8 request_bytes[sizeof(IO_REQUEST) + IO_PATH_MAX];
static UINT8 reply_bytes[CHANNEL_MAX_BYTES];
static UINT8 current[IO_PATH_MAX];
static UINT8 joined[3 * IO_PATH_MAX + 2];
static UINT32 last_transaction;

/* A message with no handles still names an array in user memory, which
   channel_write checks whatever the count (syscalls.md S6). */
static const HANDLE_TRANSFER no_handles[1];

static void copy(UINT8 *to, const UINT8 *from, UINT32 count)
{
    UINT32 i;

    for (i = 0; i < count; i++)
        to[i] = from[i];
}

static int same(const UINT8 *a, const UINT8 *b, UINT32 count)
{
    UINT32 i;

    for (i = 0; i < count; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}

static int compare(const UINT8 *a, UINT32 a_length, const UINT8 *b,
                   UINT32 b_length)
{
    UINT32 i, n = a_length < b_length ? a_length : b_length;

    for (i = 0; i < n; i++)
        if (a[i] != b[i])
            return a[i] < b[i] ? -1 : 1;
    return (a_length > b_length) - (a_length < b_length);
}

static UINT32 text_length(const char *text)
{
    UINT32 n = 0;

    while (text[n])
        n++;
    return n;
}

/* The normal form of the length bytes at in, an absolute path, at out,
   and its length at *out_length. Returns 0 if in is not absolute or its
   normal form passes IO_PATH_MAX. */
static int normalize(const UINT8 *in, UINT32 length, UINT8 *out,
                     UINT32 *out_length)
{
    UINT32 at = 0, i = 0, start;

    if (length == 0 || in[0] != '/')
        return 0;
    while (i < length) {
        while (i < length && in[i] == '/')
            i++;
        start = i;
        while (i < length && in[i] != '/')
            i++;
        if (i == start || (i - start == 1 && in[start] == '.'))
            continue;
        if (i - start == 2 && in[start] == '.' && in[start + 1] == '.') {
            while (at > 0 && out[at - 1] != '/')
                at--;
            if (at > 0)
                at--;
            continue;
        }
        if (i - start + 1 > IO_PATH_MAX - at)
            return 0;
        out[at++] = '/';
        copy(out + at, in + start, i - start);
        at += i - start;
    }
    *out_length = at;
    return 1;
}

/* Whether the normal form prefix, of prefix_length bytes, is path or an
   ancestor of it. The root, of no bytes, is an ancestor of every path. */
static int covers(const UINT8 *prefix, UINT32 prefix_length,
                  const UINT8 *path, UINT32 length)
{
    return prefix_length <= length && same(prefix, path, prefix_length) &&
           (length == prefix_length || path[prefix_length] == '/');
}

/* The entry whose prefix is the longest that covers path, or 0. */
static const VIEW_ENTRY *entry_for(const VIEW *view, const UINT8 *path,
                                   UINT32 length)
{
    const VIEW_ENTRY *best = 0;
    UINT32 i;

    for (i = 0; i < view->count; i++) {
        const VIEW_ENTRY *e = &view->entries[i];

        if (covers(e->prefix, e->length, path, length) &&
            (!best || e->length > best->length))
            best = e;
    }
    return best;
}

UINT32 view_add(VIEW *view, const char *prefix, UINT32 directory)
{
    VIEW_ENTRY *e;
    UINT32 i;

    if (view->count == LAUNCH_VIEW_MAX)
        return ERR_OUT_OF_RANGE;
    e = &view->entries[view->count];
    if (!normalize((const UINT8 *)prefix, text_length(prefix), current,
                   &e->length) ||
        e->length > LAUNCH_PREFIX_MAX)
        return ERR_INVALID_ARGS;
    copy(e->prefix, current, e->length);
    for (i = 0; i < view->count; i++)
        if (view->entries[i].length == e->length &&
            same(view->entries[i].prefix, e->prefix, e->length))
            return ERR_ALREADY_EXISTS;
    e->directory = directory;
    view->count++;
    return OK;
}

/* ------------------------------------------------------------------------
   Requests. */

static UINT32 next_transaction(void)
{
    last_transaction = (last_transaction + 1) & TRANSACTION_MASK;
    if (!last_transaction)
        last_transaction = 1;
    return last_transaction;
}

/* Wait for channel's next message and read it into reply, keeping at
   most one handle at *received and closing any others. */
static UINT32 await(UINT32 channel, UINT8 *reply, UINT32 capacity,
                    UINT32 *length, RECEIVED_HANDLE *received)
{
    RECEIVED_HANDLE handles[CHANNEL_MAX_HANDLES];
    UINT32 observed, count, i;
    UINT32 status = object_wait_one(channel,
                                    CHANNEL_READABLE | CHANNEL_PEER_CLOSED,
                                    DEADLINE_INFINITE, &observed);

    if (status == OK)
        status = channel_read(channel, reply, capacity, handles,
                              CHANNEL_MAX_HANDLES, length, &count);
    if (status != OK)
        return status;
    for (i = 0; i < count; i++) {
        if (i == 0 && received)
            *received = handles[0];
        else
            handle_close(handles[i].handle);
    }
    if (received && count == 0)
        received->handle = HANDLE_INVALID;
    return OK;
}

/* A reply's status, once its header is checked against the request. */
static UINT32 reply_status(const UINT8 *reply, UINT32 length,
                           UINT32 transaction)
{
    const IO_REPLY *header = (const IO_REPLY *)reply;

    if (length < sizeof(IO_REPLY) || header->transaction != transaction)
        return ERR_BAD_STATE;
    return header->status;
}

UINT32 io_send(UINT32 channel, IO_REQUEST *request, UINT32 length,
               const HANDLE_TRANSFER *handles, UINT32 handle_count)
{
    request->transaction = next_transaction();
    return channel_write(channel, request, length,
                         handle_count ? handles : no_handles, handle_count);
}

UINT32 io_receive(UINT32 channel, const IO_REQUEST *request, UINT8 *reply,
                  UINT32 capacity, UINT32 *reply_length,
                  RECEIVED_HANDLE *received)
{
    UINT32 status = await(channel, reply, capacity, reply_length, received);

    if (status == OK)
        status = reply_status(reply, *reply_length, request->transaction);
    if (status != OK && received && received->handle != HANDLE_INVALID) {
        handle_close(received->handle);
        received->handle = HANDLE_INVALID;
    }
    return status;
}

UINT32 io_call(UINT32 channel, IO_REQUEST *request, UINT32 length,
               UINT8 *reply, UINT32 capacity, UINT32 *reply_length,
               RECEIVED_HANDLE *received)
{
    UINT32 status = io_send(channel, request, length, 0, 0);

    if (received)
        received->handle = HANDLE_INVALID;
    if (status != OK)
        return status;
    return io_receive(channel, request, reply, capacity, reply_length,
                      received);
}

UINT32 io_read(UINT32 channel, UINT64 offset, void *buffer, UINT32 length,
               UINT32 *count)
{
    IO_REQUEST request;
    UINT32 status, reply_length;
    UINT64 got;

    *count = 0;
    if (length > IO_TRANSFER_MAX)
        return ERR_INVALID_ARGS;
    request.operation = IO_READ;
    request.offset = offset;
    request.length = length;
    request.rights = 0;
    request.flags = 0;
    request.reserved = 0;
    status = io_call(channel, &request, sizeof request, reply_bytes,
                     sizeof reply_bytes, &reply_length, 0);
    if (status != OK)
        return status;
    got = ((const IO_REPLY *)reply_bytes)->information;
    if (got > length || got != reply_length - sizeof(IO_REPLY))
        return ERR_BAD_STATE;
    copy(buffer, reply_bytes + sizeof(IO_REPLY), (UINT32)got);
    *count = (UINT32)got;
    return OK;
}

/* A request of operation for length bytes at offset with flags, its
   data the count bytes at data, which follow it, answered with no
   bytes, or a write's count. The request is built in reply_bytes, which
   holds the largest a request carries, and its header is kept apart to
   check the reply against. */
static UINT32 io_change(UINT32 channel, UINT32 operation, UINT64 offset,
                        UINT32 length, UINT32 flags, const UINT8 *data,
                        UINT32 count, UINT64 *information)
{
    IO_REQUEST *request = (IO_REQUEST *)reply_bytes, sent;
    UINT32 status, reply_length;

    request->operation = operation;
    request->offset = offset;
    request->length = length;
    request->rights = 0;
    request->flags = flags;
    request->reserved = 0;
    copy(reply_bytes + sizeof *request, data, count);
    status = io_send(channel, request, sizeof *request + count, 0, 0);
    if (status != OK)
        return status;
    sent = *request;
    status = io_receive(channel, &sent, reply_bytes, sizeof reply_bytes,
                        &reply_length, 0);
    if (status == OK && reply_length != sizeof(IO_REPLY))
        status = ERR_BAD_STATE;
    if (status == OK && information)
        *information = ((const IO_REPLY *)reply_bytes)->information;
    return status;
}

UINT32 io_write(UINT32 channel, UINT64 offset, const void *buffer,
                UINT32 length, UINT32 *count)
{
    UINT64 written = 0;
    UINT32 status;

    *count = 0;
    if (length > IO_TRANSFER_MAX)
        return ERR_INVALID_ARGS;
    status = io_change(channel, IO_WRITE, offset, length, 0, buffer, length,
                       &written);
    if (status == OK && written > length)
        status = ERR_BAD_STATE;
    if (status == OK)
        *count = (UINT32)written;
    return status;
}

UINT32 io_truncate(UINT32 channel, UINT64 size)
{
    return io_change(channel, IO_TRUNCATE, size, 0, 0, 0, 0, 0);
}

UINT32 io_flush(UINT32 channel)
{
    return io_change(channel, IO_FLUSH, 0, 0, 0, 0, 0, 0);
}

UINT32 io_remove(UINT32 channel, const UINT8 *path, UINT32 length)
{
    if (length > IO_PATH_MAX)
        return ERR_INVALID_ARGS;
    return io_change(channel, IO_REMOVE, 0, length, 0, path, length, 0);
}

UINT32 io_rename(UINT32 channel, const UINT8 *from, UINT32 from_length,
                 const UINT8 *to, UINT32 to_length, UINT32 flags)
{
    if (from_length > IO_PATH_MAX || to_length > IO_PATH_MAX)
        return ERR_INVALID_ARGS;
    copy(joined, from, from_length);
    copy(joined + from_length, to, to_length);
    return io_change(channel, IO_RENAME, from_length,
                     from_length + to_length, flags, joined,
                     from_length + to_length, 0);
}

UINT32 io_get_section(UINT32 channel, UINT32 rights,
                      RECEIVED_HANDLE *received, UINT64 *size)
{
    IO_REQUEST request;
    UINT32 status, reply_length;

    request.operation = IO_GET_SECTION;
    request.offset = 0;
    request.length = 0;
    request.rights = rights;
    request.flags = 0;
    request.reserved = 0;
    received->handle = HANDLE_INVALID;
    status = io_call(channel, &request, sizeof request, reply_bytes,
                     sizeof reply_bytes, &reply_length, received);
    if (status == OK && (received->handle == HANDLE_INVALID ||
                         received->type != TYPE_SECTION)) {
        if (received->handle != HANDLE_INVALID)
            handle_close(received->handle);
        received->handle = HANDLE_INVALID;
        return ERR_BAD_STATE;
    }
    if (status == OK)
        *size = ((const IO_REPLY *)reply_bytes)->information;
    return status;
}

/* ------------------------------------------------------------------------
   Opens. */

/* Send operation, an open, for the length bytes at path on directory
   and wait for the reply on the new channel, *mine, which stays open on
   OK. */
static UINT32 open_once(UINT32 directory, UINT32 operation, const UINT8 *path,
                        UINT32 length, UINT32 rights, UINT32 flags,
                        UINT32 *mine, UINT32 *reply_length)
{
    IO_REQUEST *request = (IO_REQUEST *)request_bytes;
    HANDLE_TRANSFER transfer;
    UINT32 theirs, status;

    status = channel_create(OPEN_CAPACITY, mine, &theirs);
    if (status != OK)
        return status;
    request->transaction = next_transaction();
    request->operation = operation;
    request->offset = 0;
    request->length = length;
    request->rights = rights;
    request->flags = flags;
    request->reserved = 0;
    copy(request_bytes + sizeof(IO_REQUEST), path, length);
    transfer.handle = theirs;
    transfer.rights = RIGHTS_SAME;
    status = channel_write(directory, request_bytes,
                           sizeof(IO_REQUEST) + length, &transfer, 1);
    if (status != OK)
        handle_close(theirs);
    if (status == OK)
        status = await(*mine, reply_bytes, sizeof reply_bytes, reply_length,
                       0);
    if (status == OK)
        status = reply_status(reply_bytes, *reply_length,
                              request->transaction);
    if (status == OK &&
        *reply_length < sizeof(IO_REPLY) + sizeof(IO_ATTRIBUTES))
        status = ERR_BAD_STATE;
    if (status != OK)
        handle_close(*mine);
    return status;
}

/* A link's target mapped as the Windows layer maps an old-style path,
   into joined, if it names a drive: C:\nix\store and what follows it
   becomes /store and the rest, '\' and '/' alike. Returns 1 if the
   target is not such a path or was mapped, with *length the mapped
   length; 0 if it names a drive the layer does not map. */
static int map_drive(const UINT8 *target, UINT32 length, UINT32 *mapped)
{
    UINT32 i;

    *mapped = 0;
    if (length < 2 || target[1] != ':' ||
        !((target[0] >= 'A' && target[0] <= 'Z') ||
          (target[0] >= 'a' && target[0] <= 'z')))
        return 1;
    for (i = 0; i < length; i++)
        joined[i] = target[i] == '\\' ? '/' : target[i];
    if (length < OLD_STORE_LENGTH ||
        !same(joined, (const UINT8 *)OLD_STORE, OLD_STORE_LENGTH) ||
        (length > OLD_STORE_LENGTH && joined[OLD_STORE_LENGTH] != '/'))
        return 0;
    copy(joined, (const UINT8 *)STORE_PREFIX, STORE_PREFIX_LENGTH);
    for (i = OLD_STORE_LENGTH; i < length; i++)
        joined[STORE_PREFIX_LENGTH + i - OLD_STORE_LENGTH] =
            target[i] == '\\' ? '/' : target[i];
    *mapped = STORE_PREFIX_LENGTH + length - OLD_STORE_LENGTH;
    return 1;
}

/* The path the link the server stopped at leads to, from current, of
   length bytes, and the reply: an absolute target and the rest of the
   path, or the link's directory, a relative target and the rest; into
   current again. rest is where the part sent to the server starts in
   current. */
static UINT32 follow(UINT32 *length, UINT32 rest, UINT32 reply_length)
{
    const IO_ATTRIBUTES *a =
        (const IO_ATTRIBUTES *)(reply_bytes + sizeof(IO_REPLY));
    const UINT8 *target = reply_bytes + sizeof(IO_REPLY) + sizeof *a;
    const UINT8 *remaining;
    UINT32 sent = *length - rest, link_end, slash, at = 0, mapped;

    if (a->size == 0 || a->size > IO_PATH_MAX ||
        a->remaining > IO_PATH_MAX ||
        reply_length - sizeof(IO_REPLY) - sizeof *a != a->size + a->remaining ||
        sent == 0)
        return ERR_BAD_STATE;
    remaining = target + a->size;
    if (a->remaining) {
        if (a->remaining + 2 > sent ||
            current[*length - a->remaining - 1] != '/' ||
            !same(current + *length - a->remaining, remaining, a->remaining))
            return ERR_BAD_STATE;
        link_end = *length - a->remaining - 1;
    } else {
        link_end = *length;
    }
    /* The '/' before the link's name: within the part sent, or the one
       after the entry's prefix, which ends the link's directory. */
    for (slash = link_end - 1; slash >= rest && current[slash] != '/'; slash--)
        ;
    if (!map_drive(target, (UINT32)a->size, &mapped))
        return ERR_NOT_FOUND;
    if (mapped) {
        at = mapped;
    } else if (target[0] == '/') {
        copy(joined, target, (UINT32)a->size);
        at = (UINT32)a->size;
    } else {
        copy(joined, current, slash);
        joined[slash] = '/';
        copy(joined + slash + 1, target, (UINT32)a->size);
        at = slash + 1 + (UINT32)a->size;
    }
    if (a->remaining) {
        joined[at++] = '/';
        copy(joined + at, remaining, a->remaining);
        at += a->remaining;
    }
    return normalize(joined, at, current, length) ? OK : ERR_INVALID_ARGS;
}

UINT32 io_open(UINT32 directory, UINT32 operation, const UINT8 *path,
               UINT32 length, UINT32 rights, UINT32 *channel,
               IO_ATTRIBUTES *attributes)
{
    const IO_ATTRIBUTES *a =
        (const IO_ATTRIBUTES *)(reply_bytes + sizeof(IO_REPLY));
    UINT32 reply_length, status;

    *channel = HANDLE_INVALID;
    if (length > IO_PATH_MAX)
        return ERR_INVALID_ARGS;
    status = open_once(directory, operation, path, length, rights, 0, channel,
                       &reply_length);
    if (status != OK)
        return status;
    if (a->kind == IO_KIND_LINK) {
        handle_close(*channel);
        *channel = HANDLE_INVALID;
        return ERR_NOT_SUPPORTED;
    }
    *attributes = *a;
    return OK;
}

UINT32 view_open(const VIEW *view, const char *path, UINT32 rights,
                 UINT32 *channel, IO_ATTRIBUTES *attributes)
{
    return view_create(view, path, rights, 0, channel, attributes);
}

UINT32 view_create(const VIEW *view, const char *path, UINT32 rights,
                   UINT32 flags, UINT32 *channel, IO_ATTRIBUTES *attributes)
{
    const VIEW_ENTRY *e;
    const IO_ATTRIBUTES *a =
        (const IO_ATTRIBUTES *)(reply_bytes + sizeof(IO_REPLY));
    UINT32 length, hops, rest, status, mine, reply_length;

    *channel = HANDLE_INVALID;
    if (!normalize((const UINT8 *)path, text_length(path), current, &length))
        return ERR_INVALID_ARGS;
    for (hops = 0; hops <= SYMLINK_MAX_HOPS; hops++) {
        e = entry_for(view, current, length);
        if (!e)
            return ERR_NOT_FOUND;
        rest = e->length < length ? e->length + 1 : length;
        status = open_once(e->directory, IO_OPEN, current + rest,
                           length - rest, rights, flags, &mine,
                           &reply_length);
        if (status != OK)
            return status;
        if (a->kind != IO_KIND_LINK) {
            *attributes = *a;
            *channel = mine;
            return OK;
        }
        handle_close(mine);
        status = follow(&length, rest, reply_length);
        if (status != OK)
            return status;
    }
    return ERR_OUT_OF_RANGE;
}

/* ------------------------------------------------------------------------
   Listings. */

typedef struct {
    const UINT8 *name;
    UINT32 length;
} NAME;

/* The next component of each prefix nested below the normal form path,
   once each, in bytewise order. */
static UINT32 nested(const VIEW *view, const UINT8 *path, UINT32 length,
                     NAME *names)
{
    UINT32 count = 0, i, j, end;

    for (i = 0; i < view->count; i++) {
        const VIEW_ENTRY *e = &view->entries[i];
        NAME name;

        if (e->length <= length || !covers(path, length, e->prefix, e->length))
            continue;
        for (end = length + 1; end < e->length && e->prefix[end] != '/'; end++)
            ;
        name.name = e->prefix + length + 1;
        name.length = end - length - 1;
        for (j = 0; j < count && compare(names[j].name, names[j].length,
                                         name.name, name.length) < 0;
             j++)
            ;
        if (j < count && compare(names[j].name, names[j].length, name.name,
                                 name.length) == 0)
            continue;
        for (end = count; end > j; end--)
            names[end] = names[end - 1];
        names[j] = name;
        count++;
    }
    return count;
}

UINT32 view_list(const VIEW *view, const char *path,
                 void (*each)(void *context, const UINT8 *name,
                              UINT32 length, UINT32 kind),
                 void *context)
{
    NAME names[LAUNCH_VIEW_MAX];
    IO_ATTRIBUTES attributes;
    IO_REQUEST request;
    const IO_DIRECTORY_ENTRY *entry;
    UINT32 length, count, next = 0, channel, status, reply_length, at;
    UINT32 taken = 0;
    UINT64 cookie = 0;

    if (!normalize((const UINT8 *)path, text_length(path), current, &length))
        return ERR_INVALID_ARGS;
    count = nested(view, current, length, names);
    if (!entry_for(view, current, length)) {
        if (!count)
            return ERR_NOT_FOUND;
        for (; next < count; next++)
            each(context, names[next].name, names[next].length,
                 IO_KIND_DIRECTORY);
        return OK;
    }
    status = view_open(view, path, IO_RIGHT_LIST, &channel, &attributes);
    if (status != OK)
        return status;
    if (attributes.kind != IO_KIND_DIRECTORY) {
        handle_close(channel);
        return ERR_NOT_SUPPORTED;
    }
    for (;;) {
        request.operation = IO_READ_DIR;
        request.offset = cookie;
        request.length = LIST_BATCH;
        request.rights = 0;
        request.flags = 0;
        request.reserved = 0;
        status = io_call(channel, &request, sizeof request, reply_bytes,
                         sizeof reply_bytes, &reply_length, 0);
        if (status != OK || reply_length == sizeof(IO_REPLY))
            break;
        cookie = ((const IO_REPLY *)reply_bytes)->information;
        for (at = sizeof(IO_REPLY); at < reply_length;
             at += sizeof *entry + entry->length) {
            const UINT8 *name;

            entry = (const IO_DIRECTORY_ENTRY *)(reply_bytes + at);
            if (reply_length - at < sizeof *entry ||
                reply_length - at - sizeof *entry < entry->length) {
                status = ERR_BAD_STATE;
                break;
            }
            if (taken++ == LIST_ENTRIES_MAX) {
                status = ERR_OUT_OF_RANGE;
                break;
            }
            name = reply_bytes + at + sizeof *entry;
            for (; next < count && compare(names[next].name,
                                           names[next].length, name,
                                           entry->length) < 0;
                 next++)
                each(context, names[next].name, names[next].length,
                     IO_KIND_DIRECTORY);
            if (next < count && compare(names[next].name, names[next].length,
                                        name, entry->length) == 0)
                continue;
            each(context, name, entry->length, entry->kind);
        }
        if (status != OK)
            break;
    }
    handle_close(channel);
    for (; status == OK && next < count; next++)
        each(context, names[next].name, names[next].length,
             IO_KIND_DIRECTORY);
    return status;
}
