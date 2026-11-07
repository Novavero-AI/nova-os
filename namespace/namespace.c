/* The namespace server (docs/spec/objects.md O8, storage.md F4): serves
   the root of the one namespace, a directory whose entries are the
   names file systems are published under. An open below a published
   name goes, with the rest of the path and the client's endpoint, to
   the server behind the name, which answers and serves the endpoint
   from then on, so file traffic does not pass through here (O8, rule
   3). Publishing binds a new name to a directory handle and never hides
   one already bound; replacing swaps the handle behind a name in one
   step; unpublishing removes it (F4). The server holds no per-process
   state: a process's view names the directories it reaches (F5).

   The root carries WRITE so that an open below a publication can ask
   for it, rights narrowing at every open (F1); the server behind the
   name grants at most what its published directory carries. The root
   itself is written only by publishing.

   Each root channel belongs to a client (F17). The root channel of the
   bootstrap message, its starter's, starts one, and so does each open
   of the root through it, which is how the starter hands the root to
   another process; an open of the root through any other channel
   belongs to that channel's client. A client holds at most
   SERVICE_CLIENT_CHANNELS root channels, so none takes what another
   holds.

   One thread waits on every root channel it serves; it ends once none
   is left open, and its publications close with it. */

#include "directory_protocol.h"
#include "log.h"
#include "name.h"
#include "native.h"
#include "services.h"

/* Every channel served is waited on in one call. */
#define CHANNELS_MAX WAIT_MANY_MAX

/* The most names published at once. */
#define PUBLICATIONS_MAX 16

#define ROOT_RIGHTS                                                       \
    (IO_RIGHT_READ | IO_RIGHT_WRITE | IO_RIGHT_EXECUTE | IO_RIGHT_LIST |   \
     IO_RIGHT_PUBLISH)

/* Whether opening the root through a channel starts a client. */
typedef enum {
    OPENS_JOIN = 1,
    OPENS_START_CLIENTS
} OPENS;

typedef struct {
    UINT32 handle;
    UINT32 client;
    UINT32 rights;
    OPENS opens;
} CHANNEL;

/* A name and the directory published under it, kept in bytewise order
   of name. */
typedef struct {
    UINT8 name[NAME_MAX_BYTES];
    UINT32 length;
    UINT32 directory;
} PUBLICATION;

static CHANNEL channels[CHANNELS_MAX];
static UINT32 channel_count;
static UINT32 client_channels[SERVICE_CLIENTS]; /* 0 for a free client */
static PUBLICATION publications[PUBLICATIONS_MAX];
static UINT32 publication_count;

static UINT8 message[CHANNEL_MAX_BYTES];
static UINT8 reply[CHANNEL_MAX_BYTES];
static RECEIVED_HANDLE received[CHANNEL_MAX_HANDLES];
static UINT32 received_count;

/* A message with no handles still names an array in user memory, which
   channel_write checks whatever the count (syscalls.md S6). */
static const HANDLE_TRANSFER no_handles[1];

static void copy(UINT8 *to, const UINT8 *from, UINT32 count)
{
    UINT32 i;

    for (i = 0; i < count; i++)
        to[i] = from[i];
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

static void close_received(void)
{
    UINT32 i;

    for (i = 0; i < received_count; i++)
        handle_close(received[i].handle);
    received_count = 0;
}

/* The handle the request carries, as its only one, if it is a channel
   with every right in rights; HANDLE_INVALID otherwise, with every
   handle closed. */
static UINT32 take_channel(UINT32 rights)
{
    UINT32 handle;

    if (received_count != 1 || received[0].type != TYPE_CHANNEL ||
        (received[0].rights & rights) != rights) {
        close_received();
        return HANDLE_INVALID;
    }
    handle = received[0].handle;
    received_count = 0;
    return handle;
}

/* Serve handle as a root channel of client. The channels open are the
   sum of the clients', so they never pass CHANNELS_MAX. */
static UINT32 add_channel(UINT32 handle, UINT32 client, UINT32 rights,
                          OPENS opens)
{
    CHANNEL *c;

    if (client_channels[client] == SERVICE_CLIENT_CHANNELS)
        return ERR_NO_RESOURCES;
    client_channels[client]++;
    c = &channels[channel_count++];
    c->handle = handle;
    c->client = client;
    c->rights = rights;
    c->opens = opens;
    return OK;
}

/* Serve handle as the first channel of a new client, in a client that
   has none open. */
static UINT32 add_client(UINT32 handle, UINT32 rights, OPENS opens)
{
    UINT32 i;

    for (i = 0; i < SERVICE_CLIENTS; i++)
        if (client_channels[i] == 0)
            return add_channel(handle, i, rights, opens);
    return ERR_NO_RESOURCES;
}

/* Close the channel at index, whose place the last channel takes. */
static void remove_channel(UINT32 index)
{
    handle_close(channels[index].handle);
    client_channels[channels[index].client]--;
    channels[index] = channels[--channel_count];
}

static void send(UINT32 channel, UINT32 transaction, UINT32 status,
                 UINT64 information, UINT32 data)
{
    IO_REPLY *header = (IO_REPLY *)reply;

    header->transaction = transaction;
    header->status = status;
    header->information = information;
    channel_write(channel, reply, sizeof(IO_REPLY) + data, no_handles, 0);
}

/* The root's attributes, at reply's data. */
static UINT32 root_attributes(UINT32 rights)
{
    IO_ATTRIBUTES *a = (IO_ATTRIBUTES *)(reply + sizeof(IO_REPLY));
    UINT32 i;

    a->kind = IO_KIND_DIRECTORY;
    a->flags = 0;
    a->size = 0;
    for (i = 0; i < IO_DIGEST_SIZE; i++)
        a->digest[i] = 0;
    a->rights = rights;
    a->remaining = 0;
    return sizeof *a;
}

/* The index of the publication named by the length bytes at name, or of
   where it would go, and whether it is there. */
static int find(const UINT8 *name, UINT32 length, UINT32 *index)
{
    UINT32 i;
    int order;

    for (i = 0; i < publication_count; i++) {
        order = compare(name, length, publications[i].name,
                        publications[i].length);
        if (order <= 0) {
            *index = i;
            return order == 0;
        }
    }
    *index = publication_count;
    return 0;
}

/* ------------------------------------------------------------------------
   Requests. */

/* An open: of the root itself for the empty path; otherwise passed on,
   with its flags, below the name its first component publishes. A
   create needs WRITE on the channel it comes through, as on any
   directory. The server creates nothing, so an open with IO_OPEN_CREATE
   of the root itself, or of a name the root does not hold, is not
   supported. */
static void open_path(const CHANNEL *c, const IO_REQUEST *request,
                      const UINT8 *path)
{
    UINT32 endpoint = take_channel(IO_ENDPOINT_RIGHTS);
    UINT32 n = 0, rest, index, status = OK;
    HANDLE_TRANSFER transfer;
    IO_REQUEST *forward = (IO_REQUEST *)reply;

    if (endpoint == HANDLE_INVALID) {
        send(c->handle, request->transaction, ERR_INVALID_ARGS, 0, 0);
        return;
    }
    while (n < request->length && path[n] != '/')
        n++;
    rest = n < request->length ? n + 1 : n;
    if ((request->flags & ~IO_OPEN_FLAGS) || request->reserved ||
        ((request->flags & (IO_OPEN_EXCLUSIVE | IO_OPEN_DIRECTORY)) &&
         !(request->flags & IO_OPEN_CREATE)))
        status = ERR_INVALID_ARGS;
    else if ((request->rights & ~c->rights) ||
             ((request->flags & IO_OPEN_CREATE) &&
              !(c->rights & IO_RIGHT_WRITE)))
        status = ERR_ACCESS_DENIED;
    else if ((request->flags & IO_OPEN_CREATE) && request->length == 0)
        status = ERR_NOT_SUPPORTED;
    else if (request->length > IO_PATH_MAX ||
             (request->length && !name_valid(path, n)) ||
             (rest == request->length && rest > n))
        status = ERR_INVALID_ARGS;
    else if (request->length == 0) {
        status = c->opens == OPENS_START_CLIENTS
                     ? add_client(endpoint, request->rights, OPENS_JOIN)
                     : add_channel(endpoint, c->client, request->rights,
                                   OPENS_JOIN);
        if (status == OK) {
            send(endpoint, request->transaction, OK, 0,
                 root_attributes(request->rights));
            return;
        }
    } else if (!find(path, n, &index))
        status = (request->flags & IO_OPEN_CREATE) && rest == request->length
                     ? ERR_NOT_SUPPORTED
                     : ERR_NOT_FOUND;
    if (status == OK) {
        forward->transaction = request->transaction;
        forward->operation = IO_OPEN;
        forward->offset = 0;
        forward->length = request->length - rest;
        forward->rights = request->rights;
        forward->flags = request->flags;
        forward->reserved = 0;
        copy(reply + sizeof *forward, path + rest, request->length - rest);
        transfer.handle = endpoint;
        transfer.rights = RIGHTS_SAME;
        status = channel_write(publications[index].directory, reply,
                               sizeof *forward + forward->length, &transfer,
                               1);
        if (status == OK)
            return;
    }
    send(endpoint, request->transaction, status, 0, 0);
    handle_close(endpoint);
}

static UINT32 list(UINT64 cookie, UINT32 count, UINT64 *next, UINT32 *at)
{
    UINT64 i;
    UINT32 added = 0;

    for (i = cookie; i < publication_count && added < count; i++) {
        IO_DIRECTORY_ENTRY *entry = (IO_DIRECTORY_ENTRY *)(reply + *at);
        const PUBLICATION *p = &publications[i];

        if (sizeof reply - *at < sizeof *entry + p->length)
            break;
        entry->length = (UINT16)p->length;
        entry->kind = IO_KIND_DIRECTORY;
        entry->reserved = 0;
        copy(reply + *at + sizeof *entry, p->name, p->length);
        *at += sizeof *entry + p->length;
        added++;
    }
    *next = i;
    return OK;
}

/* publish, replace and unpublish (F4). */
static UINT32 publishing(const IO_REQUEST *request, const UINT8 *name)
{
    UINT32 directory = HANDLE_INVALID, index, i;
    int found;

    if (request->operation != IO_UNPUBLISH) {
        directory = take_channel(CHANNEL_RIGHT_WRITE);
        if (directory == HANDLE_INVALID)
            return ERR_INVALID_ARGS;
    }
    close_received();
    if (!name_valid(name, request->length)) {
        if (directory != HANDLE_INVALID)
            handle_close(directory);
        return ERR_INVALID_ARGS;
    }
    found = find(name, request->length, &index);
    if (request->operation == IO_PUBLISH) {
        if (found || publication_count == PUBLICATIONS_MAX) {
            handle_close(directory);
            return found ? ERR_ALREADY_EXISTS : ERR_NO_RESOURCES;
        }
        for (i = publication_count; i > index; i--)
            publications[i] = publications[i - 1];
        copy(publications[index].name, name, request->length);
        publications[index].length = request->length;
        publications[index].directory = directory;
        publication_count++;
        return OK;
    }
    if (!found) {
        if (directory != HANDLE_INVALID)
            handle_close(directory);
        return ERR_NOT_FOUND;
    }
    handle_close(publications[index].directory);
    if (request->operation == IO_REPLACE) {
        publications[index].directory = directory;
        return OK;
    }
    for (i = index; i + 1 < publication_count; i++)
        publications[i] = publications[i + 1];
    publication_count--;
    return OK;
}

/* Serve the request in message, of bytes bytes, on channel index.
   Returns 0 if the channel ends: a message too short to be a request,
   which is answered by closing the channel. */
static int serve_request(UINT32 index, UINT32 bytes)
{
    const IO_REQUEST *request = (const IO_REQUEST *)message;
    const UINT8 *data = message + sizeof(IO_REQUEST);
    CHANNEL c = channels[index];
    UINT64 next = 0;
    UINT32 status, at = sizeof(IO_REPLY);

    if (bytes < sizeof(IO_REQUEST)) {
        close_received();
        return 0;
    }
    if (request->operation != IO_READ_DIR &&
        request->length != bytes - sizeof(IO_REQUEST)) {
        close_received();
        send(c.handle, request->transaction, ERR_INVALID_ARGS, 0, 0);
        return 1;
    }
    if (request->operation == IO_OPEN) {
        open_path(&c, request, data);
        return 1;
    }
    if (request->flags || request->reserved) {
        close_received();
        send(c.handle, request->transaction, ERR_INVALID_ARGS, 0, 0);
        return 1;
    }
    switch (request->operation) {
    case IO_READ_DIR:
        close_received();
        status = c.rights & IO_RIGHT_LIST
                     ? list(request->offset, request->length, &next, &at)
                     : ERR_ACCESS_DENIED;
        send(c.handle, request->transaction, status, next,
             status == OK ? at - (UINT32)sizeof(IO_REPLY) : 0);
        return 1;
    case IO_GET_ATTRIBUTES:
        close_received();
        send(c.handle, request->transaction, OK, 0, root_attributes(c.rights));
        return 1;
    case IO_PUBLISH:
    case IO_REPLACE:
    case IO_UNPUBLISH:
        if (!(c.rights & IO_RIGHT_PUBLISH)) {
            close_received();
            send(c.handle, request->transaction, ERR_ACCESS_DENIED, 0, 0);
            return 1;
        }
        send(c.handle, request->transaction, publishing(request, data), 0, 0);
        return 1;
    }
    close_received();
    send(c.handle, request->transaction, ERR_NOT_SUPPORTED, 0, 0);
    return 1;
}

/* Serve one message on channel index. Returns 0 if the channel ends: its
   message cannot be read, which would leave it queued. */
static int serve(UINT32 index)
{
    UINT32 bytes;

    if (channel_read(channels[index].handle, message, sizeof message,
                     received, CHANNEL_MAX_HANDLES, &bytes,
                     &received_count) != OK)
        return 0;
    return serve_request(index, bytes);
}

/* ------------------------------------------------------------------------
   The server. */

static void read_bootstrap(UINT32 bootstrap)
{
    RECEIVED_HANDLE handles[NAMESPACE_BOOTSTRAP_HANDLES];
    UINT32 bytes, count;

    if (channel_read(bootstrap, message, sizeof message, handles,
                     NAMESPACE_BOOTSTRAP_HANDLES, &bytes, &count) != OK ||
        count != NAMESPACE_BOOTSTRAP_HANDLES ||
        handles[NAMESPACE_BOOTSTRAP_ROOT].type != TYPE_CHANNEL)
        process_exit(SERVICE_EXIT_FAILED);
    handle_close(bootstrap);
    say_open(handles[NAMESPACE_BOOTSTRAP_DEBUG_LOG].handle);
    add_client(handles[NAMESPACE_BOOTSTRAP_ROOT].handle, ROOT_RIGHTS,
               OPENS_START_CLIENTS);
    say("nova-namespace: serving the root directory\r\n");
}

/* As the store service's loop (store.c): channels served from the last,
   so that removing or adding one leaves each index below it naming the
   channel it was waited on as. */
_Noreturn void namespace_main(UINT32 bootstrap, UINT64 argument)
{
    WAIT_ENTRY items[CHANNELS_MAX];
    UINT32 waited, i, observed;
    int keep;

    (void)argument;
    read_bootstrap(bootstrap);
    while (channel_count) {
        for (i = 0; i < channel_count; i++) {
            items[i].handle = channels[i].handle;
            items[i].signals = CHANNEL_READABLE | CHANNEL_PEER_CLOSED;
        }
        waited = channel_count;
        if (object_wait_many(items, waited, DEADLINE_INFINITE) != OK)
            break;
        for (i = waited; i-- > 0;) {
            observed = items[i].observed;
            if (observed & CHANNEL_READABLE)
                keep = serve(i);
            else
                keep = !(observed & CHANNEL_PEER_CLOSED);
            if (!keep)
                remove_channel(i);
        }
    }
    for (i = 0; i < publication_count; i++)
        handle_close(publications[i].directory);
    say("nova-namespace: no channel left open, ending\r\n");
    process_exit(SERVICE_EXIT_DONE);
}
