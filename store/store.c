/* The store service (docs/spec/storage.md F12, F16): serves /store, read
   only, from the store image's section, which the first process passes
   it (services.h). It serves the directory protocol
   (directory_protocol.h) on three kinds of directory: the root, whose
   entries are every path the image holds; a closure directory, whose
   entries are one path's closure (F5); and the directories inside a
   path. Files are served from the image, each block checked against its
   blob's hash tree as it is read (F9), and IO_GET_SECTION copies a blob,
   checked the same way, into a section of the service's own, which a
   program may map but never write, so no program receives the image's
   section, which holds every path (F16). It never follows a symbolic
   link: an open that reaches one stops there (F3).

   Each channel belongs to a client (F17): the control channel and the
   root directory each start one, as does each closure directory, and
   every other channel belongs to the client of the channel it was opened
   through. A client holds at most SERVICE_CLIENT_CHANNELS channels and
   the service keeps at most CLIENT_SECTIONS sections for it, so no
   client takes what another holds; the clients are the control channel,
   the root and up to six programs' closure directories. The root is
   one client, which every process that opens /store through the
   namespace server shares, and the control channel's client holds each
   control channel opened through it, which an open of the empty path
   on a control channel makes (services.h).

   One thread waits on every channel it serves and serves each message
   in turn; it ends once no channel is left. */

#include "directory_protocol.h"
#include "log.h"
#include "name.h"
#include "native.h"
#include "reader.h"
#include "services.h"

/* Every channel served is waited on in one call. */
#define CHANNELS_MAX WAIT_MANY_MAX

/* The sections the service keeps for a client: the CLIENT_SECTIONS it
   filled for the client most recently used, so that it keeps at most
   SERVICE_CLIENTS * CLIENT_SECTIONS sections, 32 MiB at
   SECTION_MAX_SIZE, and one client's fills never push out another's
   (F17). A section one client keeps serves every client that asks for
   its blob. The service maps a section writable only while it fills
   it, and the handle it keeps carries neither WRITE nor PIN, so once
   the section is handed out nothing can write it (F9, F15). A blob that
   fails its check keeps an entry with no section, which refuses it at
   once while the entry lasts. */
#define CLIENT_SECTIONS 2
#define KEPT_SECTION_RIGHTS                                               \
    (SECTION_RIGHT_READ | SECTION_RIGHT_EXECUTE | RIGHT_DUPLICATE |       \
     RIGHT_TRANSFER)

/* The most paths an image holds for the service to compute closures
   over. */
#define STORE_PATHS_MAX 4096
#define BITS_PER_BYTE 8

/* Where the image is mapped in the service, and where a blob's section
   is mapped while the service fills it, one at a time. */
#define IMAGE_BASE 0x0000000100000000ULL
#define FILL_BASE 0x0000000200000000ULL

/* The rights each kind of directory may carry: the store is read only,
   and only the namespace server's directories publish. */
#define STORE_RIGHTS (IO_RIGHT_READ | IO_RIGHT_EXECUTE | IO_RIGHT_LIST)
#define CONTROL_RIGHTS STORE_RIGHT_CLOSURE

typedef enum {
    CHANNEL_CONTROL = 1,
    CHANNEL_ROOT,    /* every path */
    CHANNEL_CLOSURE, /* one path's closure */
    CHANNEL_NODE     /* a node inside a path */
} CHANNEL_KIND;

typedef struct {
    UINT32 handle;
    UINT32 client;
    CHANNEL_KIND kind;
    UINT32 rights;
    UINT32 path; /* a closure's path's registration */
    UINT8 node[STORE_DIGEST_SIZE];
} CHANNEL;

typedef struct {
    UINT8 blob[STORE_DIGEST_SIZE];
    UINT32 section; /* HANDLE_INVALID for a blob that failed its check */
} SECTION_SLOT;

typedef struct {
    UINT32 channels; /* open; 0 for a free client */
    UINT32 section_count;
    SECTION_SLOT sections[CLIENT_SECTIONS]; /* the most recently used first */
} CLIENT;

static STORE_IMAGE image;
static CHANNEL channels[CHANNELS_MAX];
static UINT32 channel_count;
static CLIENT clients[SERVICE_CLIENTS];
static UINT32 self = HANDLE_INVALID;

static UINT8 message[CHANNEL_MAX_BYTES];
static UINT8 reply[CHANNEL_MAX_BYTES];
static RECEIVED_HANDLE received[CHANNEL_MAX_HANDLES];
static UINT32 received_count;
static UINT8 closure[STORE_PATHS_MAX / BITS_PER_BYTE];

/* A message with no handles still names an array in user memory, which
   channel_write checks whatever the count (syscalls.md S6). */
static const HANDLE_TRANSFER no_handles[1];
static UINT32 pending[STORE_PATHS_MAX];

static void copy(UINT8 *to, const UINT8 *from, UINT64 count)
{
    UINT64 i;

    for (i = 0; i < count; i++)
        to[i] = from[i];
}

static int same(const UINT8 *a, const UINT8 *b, UINT64 count)
{
    UINT64 i;

    for (i = 0; i < count; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}

/* ------------------------------------------------------------------------
   Channels and replies. */

/* Close every handle the message being served carried but the one at
   keep, or every one for keep CHANNEL_MAX_HANDLES. */
static void close_received(UINT32 keep)
{
    UINT32 i;

    for (i = 0; i < received_count; i++)
        if (i != keep)
            handle_close(received[i].handle);
    received_count = 0;
}

/* Serve handle as a channel of client. The channels open are the sum of
   the clients', so they never pass CHANNELS_MAX. */
static UINT32 add_channel(UINT32 handle, UINT32 client, CHANNEL_KIND kind,
                          UINT32 rights, UINT32 path, const UINT8 *node)
{
    CHANNEL *c;

    if (clients[client].channels == SERVICE_CLIENT_CHANNELS)
        return ERR_NO_RESOURCES;
    clients[client].channels++;
    c = &channels[channel_count++];
    c->handle = handle;
    c->client = client;
    c->kind = kind;
    c->rights = rights;
    c->path = path;
    if (node)
        copy(c->node, node, STORE_DIGEST_SIZE);
    return OK;
}

/* Serve handle as the first channel of a new client, in a client that
   has none open. */
static UINT32 add_client(UINT32 handle, CHANNEL_KIND kind, UINT32 rights,
                         UINT32 path)
{
    UINT32 i;

    for (i = 0; i < SERVICE_CLIENTS; i++)
        if (clients[i].channels == 0)
            return add_channel(handle, i, kind, rights, path, 0);
    return ERR_NO_RESOURCES;
}

/* Close the channel at index, whose place the last channel takes. A
   client left with no channel ends, closing the sections kept for it. */
static void remove_channel(UINT32 index)
{
    CLIENT *client = &clients[channels[index].client];
    UINT32 i;

    handle_close(channels[index].handle);
    channels[index] = channels[--channel_count];
    if (--client->channels)
        return;
    for (i = 0; i < client->section_count; i++)
        if (client->sections[i].section != HANDLE_INVALID)
            handle_close(client->sections[i].section);
    client->section_count = 0;
}

/* Write a reply of the header and data bytes from reply, with handles. A
   client that leaves its queue full or closes it loses the reply. */
static void send(UINT32 channel, UINT32 transaction, UINT32 status,
                 UINT64 information, UINT32 data,
                 const HANDLE_TRANSFER *handles, UINT32 handle_count)
{
    IO_REPLY *header = (IO_REPLY *)reply;

    header->transaction = transaction;
    header->status = status;
    header->information = information;
    if (channel_write(channel, reply, sizeof(IO_REPLY) + data,
                      handle_count ? handles : no_handles,
                      handle_count) != OK) {
        UINT32 i;

        for (i = 0; i < handle_count; i++)
            handle_close(handles[i].handle);
    }
}

/* The attributes of a node, at reply's data. */
static UINT32 node_attributes(const STORE_NODE *node, UINT32 rights)
{
    IO_ATTRIBUTES *a = (IO_ATTRIBUTES *)(reply + sizeof(IO_REPLY));

    a->kind = node->kind == STORE_NODE_DIRECTORY ? IO_KIND_DIRECTORY
              : node->kind == STORE_NODE_FILE    ? IO_KIND_FILE
                                                 : IO_KIND_LINK;
    a->flags = IO_HAS_DIGEST | (node->executable ? IO_EXECUTABLE : 0);
    a->size = node->kind == STORE_NODE_DIRECTORY ? 0 : node->size;
    copy(a->digest, node->kind == STORE_NODE_FILE ? node->blob : node->digest,
         IO_DIGEST_SIZE);
    a->rights = rights;
    a->remaining = 0;
    return sizeof *a;
}

/* The attributes of the root or a closure directory, which are no node
   of the image and have no digest. */
static UINT32 listing_attributes(UINT32 rights)
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

/* ------------------------------------------------------------------------
   Closures. */

static int member(UINT32 index)
{
    return (closure[index / BITS_PER_BYTE] >> (index % BITS_PER_BYTE)) & 1;
}

/* The closure of path, by its references (storage.md, fact 5), into
   closure. */
static void compute_closure(UINT32 path)
{
    STORE_REGISTRATION r;
    UINT32 count = 0, i, next;

    for (i = 0; i < sizeof closure; i++)
        closure[i] = 0;
    closure[path / BITS_PER_BYTE] |= (UINT8)(1 << (path % BITS_PER_BYTE));
    pending[count++] = path;
    while (count) {
        store_registration_get(&image, pending[--count], &r);
        for (i = 0; i < r.reference_count; i++) {
            next = store_reference(&r, i);
            if (!member(next)) {
                closure[next / BITS_PER_BYTE] |=
                    (UINT8)(1 << (next % BITS_PER_BYTE));
                pending[count++] = next;
            }
        }
    }
}

/* ------------------------------------------------------------------------
   Opens. */

/* The first component of the length bytes at path, and where the rest
   starts, past its '/'. */
static UINT32 component(const UINT8 *path, UINT32 length, UINT32 *rest)
{
    UINT32 n = 0;

    while (n < length && path[n] != '/')
        n++;
    *rest = n < length ? n + 1 : n;
    return n;
}

/* Walk path from node, a component at a time, to the node it names or
   the first link on the way, which *node then is, with *rest where the
   part of path after it starts. */
static UINT32 walk(STORE_NODE *node, const UINT8 *path, UINT32 length,
                   UINT32 *rest)
{
    const UINT8 *child;
    UINT32 at = 0, next, n, status;

    while (at < length && node->kind != STORE_NODE_LINK) {
        n = component(path + at, length - at, &next);
        if (node->kind != STORE_NODE_DIRECTORY ||
            !store_entry_find(node, path + at, n, &child))
            return ERR_NOT_FOUND;
        status = store_node_get(&image, child, node);
        if (status != OK)
            return status;
        at += next;
    }
    *rest = at;
    return OK;
}

/* Every component of a path is a name, so it holds no empty, "." or
   ".." component; the empty path is the directory itself. A path is at
   most IO_PATH_MAX bytes, which bounds a link's reply. */
static int path_valid(const UINT8 *path, UINT32 length)
{
    UINT32 at = 0, next, n;

    if (length > IO_PATH_MAX)
        return 0;
    while (at < length) {
        n = component(path + at, length - at, &next);
        if (!name_valid(path + at, n) || (next > n && at + next == length))
            return 0;
        at += next;
    }
    return 1;
}

/* Answer an open of the length bytes at path, below directory c, on
   endpoint, and serve the endpoint as what it opened. */
static void open_path(const CHANNEL *c, const UINT8 *path, UINT32 length,
                      UINT32 rights, UINT32 transaction, UINT32 endpoint)
{
    STORE_REGISTRATION r;
    STORE_NODE node;
    UINT32 index, n, rest = 0, status = OK, data = 0;

    if (rights & ~c->rights)
        status = ERR_ACCESS_DENIED;
    else if (!path_valid(path, length))
        status = ERR_INVALID_ARGS;
    if (status == OK && c->kind == CHANNEL_NODE)
        status = store_node_get(&image, c->node, &node);
    if (status == OK && length == 0) {
        status = add_channel(endpoint, c->client, c->kind, rights, c->path,
                             c->node);
        if (status == OK) {
            data = c->kind == CHANNEL_NODE ? node_attributes(&node, rights)
                                           : listing_attributes(rights);
            send(endpoint, transaction, OK, 0, data, 0, 0);
            return;
        }
    }
    if (status == OK && c->kind == CHANNEL_NODE) {
        status = walk(&node, path, length, &rest);
    } else if (status == OK) {
        n = component(path, length, &rest);
        if (!store_registration_find(&image, path, n, &index))
            status = ERR_NOT_FOUND;
        if (status == OK && c->kind == CHANNEL_CLOSURE) {
            compute_closure(c->path);
            if (!member(index))
                status = ERR_NOT_FOUND;
        }
        if (status == OK) {
            store_registration_get(&image, index, &r);
            status = store_node_get(&image, r.root, &node);
        }
        if (status == OK) {
            n = rest;
            status = walk(&node, path + n, length - n, &rest);
            rest += n;
        }
    }
    if (status == OK && node.kind == STORE_NODE_LINK) {
        IO_ATTRIBUTES *a = (IO_ATTRIBUTES *)(reply + sizeof(IO_REPLY));

        data = node_attributes(&node, rights);
        a->remaining = length - rest;
        copy(reply + sizeof(IO_REPLY) + data, node.body, node.size);
        copy(reply + sizeof(IO_REPLY) + data + node.size, path + rest,
             length - rest);
        send(endpoint, transaction, OK, 0,
             data + (UINT32)node.size + (length - rest), 0, 0);
        handle_close(endpoint);
        return;
    }
    if (status == OK)
        status = add_channel(endpoint, c->client, CHANNEL_NODE, rights,
                             c->path, node.digest);
    if (status != OK) {
        if (status == ERR_DATA_INTEGRITY) {
            say("nova-store: open refused path=");
            say_value_bytes(path, length);
            say(" reason=integrity\r\n");
        }
        send(endpoint, transaction, status, 0, 0, 0, 0);
        handle_close(endpoint);
        return;
    }
    send(endpoint, transaction, OK, 0, node_attributes(&node, rights), 0, 0);
}

/* Answer an open of the closure directory of the path whose base name
   is the length bytes at name, on endpoint, which starts a client. */
static void open_closure(const UINT8 *name, UINT32 length, UINT32 rights,
                         UINT32 transaction, UINT32 endpoint)
{
    UINT32 index, status = OK;

    if (rights & ~STORE_RIGHTS)
        status = ERR_ACCESS_DENIED;
    else if (!name_valid(name, length) ||
             !store_registration_find(&image, name, length, &index))
        status = ERR_NOT_FOUND;
    if (status == OK)
        status = add_client(endpoint, CHANNEL_CLOSURE, rights, index);
    if (status != OK) {
        send(endpoint, transaction, status, 0, 0, 0, 0);
        handle_close(endpoint);
        return;
    }
    send(endpoint, transaction, OK, 0, listing_attributes(rights), 0, 0);
}

/* Another control channel, at endpoint, with rights, at most c's, in
   c's client (services.h). */
static void open_control(const CHANNEL *c, UINT32 rights, UINT32 transaction,
                         UINT32 endpoint)
{
    IO_ATTRIBUTES *a = (IO_ATTRIBUTES *)(reply + sizeof(IO_REPLY));
    UINT32 i, status = rights & ~c->rights ? ERR_ACCESS_DENIED : OK;

    if (status == OK)
        status = add_channel(endpoint, c->client, CHANNEL_CONTROL, rights, 0,
                             0);
    if (status != OK) {
        send(endpoint, transaction, status, 0, 0, 0, 0);
        handle_close(endpoint);
        return;
    }
    a->kind = IO_KIND_SERVICE;
    a->flags = 0;
    a->size = 0;
    for (i = 0; i < IO_DIGEST_SIZE; i++)
        a->digest[i] = 0;
    a->rights = rights;
    a->remaining = 0;
    send(endpoint, transaction, OK, 0, sizeof *a, 0, 0);
}

/* ------------------------------------------------------------------------
   Listings. */

/* Add an entry to the reply at *at, if it fits. */
static int add_entry(UINT32 *at, const UINT8 *name, UINT32 length,
                     UINT32 kind)
{
    IO_DIRECTORY_ENTRY *entry = (IO_DIRECTORY_ENTRY *)(reply + *at);

    if (sizeof reply - *at < sizeof *entry + length)
        return 0;
    entry->length = (UINT16)length;
    entry->kind = (UINT8)kind;
    entry->reserved = 0;
    copy(reply + *at + sizeof *entry, name, length);
    *at += sizeof *entry + length;
    return 1;
}

static UINT32 io_kind(UINT32 kind)
{
    return kind == STORE_NODE_DIRECTORY ? IO_KIND_DIRECTORY
           : kind == STORE_NODE_FILE    ? IO_KIND_FILE
                                        : IO_KIND_LINK;
}

/* Entries from cookie, up to count, of the root or a closure directory:
   registrations in base-name order, a closure's members only. */
static UINT32 list_paths(const CHANNEL *c, UINT64 cookie, UINT32 count,
                         UINT64 *next, UINT32 *at)
{
    STORE_REGISTRATION r;
    STORE_NODE root;
    UINT64 i;
    UINT32 added = 0, status;

    if (c->kind == CHANNEL_CLOSURE)
        compute_closure(c->path);
    for (i = cookie; i < image.registration_count && added < count; i++) {
        if (c->kind == CHANNEL_CLOSURE && !member((UINT32)i))
            continue;
        store_registration_get(&image, (UINT32)i, &r);
        status = store_node_get(&image, r.root, &root);
        if (status != OK)
            return status;
        if (!add_entry(at, r.name, r.name_length, io_kind(root.kind)))
            break;
        added++;
    }
    *next = i;
    return OK;
}

static UINT32 list_node(const CHANNEL *c, UINT64 cookie, UINT32 count,
                        UINT64 *next, UINT32 *at)
{
    STORE_NODE directory, child;
    STORE_ENTRIES entries;
    const UINT8 *name, *digest;
    UINT32 length, added = 0, status;
    UINT64 i = 0;

    status = store_node_get(&image, c->node, &directory);
    if (status != OK)
        return status;
    if (directory.kind != STORE_NODE_DIRECTORY)
        return ERR_NOT_SUPPORTED;
    store_entries(&directory, &entries);
    for (; added < count && store_entries_next(&entries, &name, &length,
                                               &digest);
         i++) {
        if (i < cookie)
            continue;
        status = store_node_get(&image, digest, &child);
        if (status != OK)
            return status;
        if (!add_entry(at, name, length, io_kind(child.kind)))
            break;
        added++;
    }
    *next = i;
    return OK;
}

/* ------------------------------------------------------------------------
   Files. */

static UINT32 file_of(const CHANNEL *c, STORE_NODE *node, STORE_BLOB *blob)
{
    UINT32 status = store_node_get(&image, c->node, node);

    if (status == OK && node->kind != STORE_NODE_FILE)
        status = ERR_NOT_SUPPORTED;
    if (status == OK)
        status = store_blob_get(&image, node, blob);
    return status;
}

static UINT32 read_file(const CHANNEL *c, UINT64 offset, UINT32 length,
                        UINT64 *count)
{
    STORE_NODE node;
    STORE_BLOB blob;
    UINT32 status;

    *count = 0;
    if (!(c->rights & IO_RIGHT_READ))
        return ERR_ACCESS_DENIED;
    status = file_of(c, &node, &blob);
    if (status != OK)
        return status;
    if (length > IO_TRANSFER_MAX)
        length = IO_TRANSFER_MAX;
    if (offset < blob.length)
        *count = blob.length - offset < length ? blob.length - offset : length;
    return store_blob_read(&blob, offset, reply + sizeof(IO_REPLY), *count);
}

/* A new section holding blob's bytes, and the service's handle to it,
   with KEPT_SECTION_RIGHTS. The section is filled through a mapping
   made with its first handle, which closes before the fill, and the
   mapping is removed before the section is kept, so that the service
   gives up its own write access (F9); a fill whose mapping is not
   removed keeps nothing. Returns ERR_DATA_INTEGRITY if a block fails
   its check. */
static UINT32 fill(const STORE_BLOB *blob, UINT32 *section)
{
    UINT64 size = blob->length ? (blob->length + PAGE_SIZE - 1) / PAGE_SIZE *
                                     PAGE_SIZE
                               : PAGE_SIZE;
    UINT32 full, status, unmapped;

    *section = HANDLE_INVALID;
    status = section_create(size, 0, &full);
    if (status != OK)
        return status;
    status = handle_duplicate(full, KEPT_SECTION_RIGHTS, section);
    if (status == OK)
        status =
            process_map(self, full, 0, FILL_BASE, size, MAP_READ | MAP_WRITE);
    handle_close(full);
    if (status == OK) {
        status = store_blob_read(blob, 0, (UINT8 *)FILL_BASE, blob->length);
        unmapped = process_unmap(self, FILL_BASE, size);
        if (status == OK)
            status = unmapped;
    }
    if (status != OK && *section != HANDLE_INVALID) {
        handle_close(*section);
        *section = HANDLE_INVALID;
    }
    return status;
}

/* Make the section at index of client's the most recently used, first,
   and return it. */
static const SECTION_SLOT *touch(CLIENT *client, UINT32 index)
{
    SECTION_SLOT slot = client->sections[index];

    for (; index > 0; index--)
        client->sections[index] = client->sections[index - 1];
    client->sections[0] = slot;
    return &client->sections[0];
}

/* Keep section, which holds blob or is HANDLE_INVALID for a blob that
   failed its check, as client's most recently used; its least recently
   used falls out past CLIENT_SECTIONS, its handle closed. */
static void keep_section(CLIENT *client, const UINT8 *blob, UINT32 section)
{
    SECTION_SLOT *last;

    if (client->section_count == CLIENT_SECTIONS) {
        last = &client->sections[CLIENT_SECTIONS - 1];
        if (last->section != HANDLE_INVALID)
            handle_close(last->section);
    } else {
        last = &client->sections[client->section_count++];
    }
    copy(last->blob, blob, STORE_DIGEST_SIZE);
    last->section = section;
    touch(client, client->section_count - 1);
}

/* The section of blob for client: one any client keeps, or one filled
   now and kept for client. */
static UINT32 blob_section(UINT32 client, const STORE_BLOB *blob,
                           UINT32 *section)
{
    UINT32 k, i, status;

    for (k = 0; k < SERVICE_CLIENTS; k++)
        for (i = 0; i < clients[k].section_count; i++)
            if (same(blob->digest, clients[k].sections[i].blob,
                     STORE_DIGEST_SIZE)) {
                *section = touch(&clients[k], i)->section;
                return *section == HANDLE_INVALID ? ERR_DATA_INTEGRITY : OK;
            }
    if (blob->length > SECTION_MAX_SIZE)
        return ERR_OUT_OF_RANGE;
    status = fill(blob, section);
    if (status == OK || status == ERR_DATA_INTEGRITY)
        keep_section(&clients[client], blob->digest, *section);
    return status;
}

/* A handle to the file's section with the rights wanted, never WRITE. */
static UINT32 get_section(const CHANNEL *c, UINT32 rights, UINT64 *size,
                          HANDLE_TRANSFER *out)
{
    STORE_NODE node;
    STORE_BLOB blob;
    UINT32 section, status;

    if (!(rights & IO_RIGHT_READ) ||
        (rights & ~(IO_RIGHT_READ | IO_RIGHT_EXECUTE)))
        return ERR_INVALID_ARGS;
    if (rights & ~c->rights)
        return ERR_ACCESS_DENIED;
    status = file_of(c, &node, &blob);
    if (status == OK)
        status = blob_section(c->client, &blob, &section);
    if (status == OK)
        status = handle_duplicate(
            section,
            SECTION_RIGHT_READ | RIGHT_TRANSFER |
                (rights & IO_RIGHT_EXECUTE ? SECTION_RIGHT_EXECUTE : 0),
            &out->handle);
    out->rights = RIGHTS_SAME;
    if (status == OK)
        *size = blob.length;
    return status;
}

/* ------------------------------------------------------------------------
   Requests. */

/* The endpoint an open carries, as its first and only handle, or
   HANDLE_INVALID. */
static UINT32 endpoint_of(void)
{
    if (received_count == 1 && received[0].type == TYPE_CHANNEL &&
        (received[0].rights & IO_ENDPOINT_RIGHTS) == IO_ENDPOINT_RIGHTS)
        return received[0].handle;
    return HANDLE_INVALID;
}

/* Serve the request in message, of bytes bytes, on channel index.
   Returns 0 if the channel ends: a message too short to be a request,
   which is answered by closing the channel. */
static int serve_request(UINT32 index, UINT32 bytes)
{
    const IO_REQUEST *request = (const IO_REQUEST *)message;
    const UINT8 *data = message + sizeof(IO_REQUEST);
    CHANNEL c = channels[index];
    HANDLE_TRANSFER handle;
    UINT64 information = 0;
    UINT32 status, endpoint, at = sizeof(IO_REPLY);

    if (bytes < sizeof(IO_REQUEST)) {
        close_received(CHANNEL_MAX_HANDLES);
        return 0;
    }
    if (request->operation == IO_OPEN ||
        request->operation == STORE_OPEN_CLOSURE) {
        endpoint = request->length == bytes - sizeof(IO_REQUEST)
                       ? endpoint_of()
                       : HANDLE_INVALID;
        if (endpoint == HANDLE_INVALID) {
            close_received(CHANNEL_MAX_HANDLES);
            send(c.handle, request->transaction, ERR_INVALID_ARGS, 0, 0, 0, 0);
            return 1;
        }
        received_count = 0;
        if (request->flags || request->reserved)
            status = ERR_INVALID_ARGS;
        else if (request->operation == IO_OPEN && c.kind == CHANNEL_CONTROL)
            status = request->length ? ERR_NOT_SUPPORTED : OK;
        else if (request->operation == STORE_OPEN_CLOSURE &&
                 c.kind != CHANNEL_CONTROL)
            status = ERR_NOT_SUPPORTED;
        else if (request->operation == IO_OPEN)
            status = OK;
        else
            status = c.rights & STORE_RIGHT_CLOSURE ? OK : ERR_ACCESS_DENIED;
        if (status != OK) {
            send(endpoint, request->transaction, status, 0, 0, 0, 0);
            handle_close(endpoint);
        } else if (c.kind == CHANNEL_CONTROL &&
                   request->operation == IO_OPEN) {
            open_control(&c, request->rights, request->transaction,
                         endpoint);
        } else if (request->operation == IO_OPEN) {
            open_path(&c, data, request->length, request->rights,
                      request->transaction, endpoint);
        } else {
            open_closure(data, request->length, request->rights,
                         request->transaction, endpoint);
        }
        return 1;
    }
    close_received(CHANNEL_MAX_HANDLES);
    if (request->flags || request->reserved) {
        send(c.handle, request->transaction, ERR_INVALID_ARGS, 0, 0, 0, 0);
        return 1;
    }
    if (c.kind == CHANNEL_CONTROL) {
        send(c.handle, request->transaction, ERR_NOT_SUPPORTED, 0, 0, 0, 0);
        return 1;
    }
    switch (request->operation) {
    case IO_READ_DIR:
        if (!(c.rights & IO_RIGHT_LIST))
            status = ERR_ACCESS_DENIED;
        else if (c.kind == CHANNEL_NODE)
            status = list_node(&c, request->offset, request->length,
                               &information, &at);
        else
            status = list_paths(&c, request->offset, request->length,
                                &information, &at);
        send(c.handle, request->transaction, status,
             status == OK ? information : 0,
             status == OK ? at - (UINT32)sizeof(IO_REPLY) : 0, 0, 0);
        return 1;
    case IO_GET_ATTRIBUTES: {
        STORE_NODE node;
        UINT32 data_size;

        status = OK;
        if (c.kind == CHANNEL_NODE) {
            status = store_node_get(&image, c.node, &node);
            data_size = status == OK ? node_attributes(&node, c.rights) : 0;
        } else {
            data_size = listing_attributes(c.rights);
        }
        send(c.handle, request->transaction, status, 0, data_size, 0, 0);
        return 1;
    }
    case IO_READ:
        status = c.kind == CHANNEL_NODE
                     ? read_file(&c, request->offset, request->length,
                                 &information)
                     : ERR_NOT_SUPPORTED;
        if (status == ERR_DATA_INTEGRITY)
            say("nova-store: read refused reason=integrity\r\n");
        send(c.handle, request->transaction, status,
             status == OK ? information : 0,
             status == OK ? (UINT32)information : 0, 0, 0);
        return 1;
    case IO_GET_SECTION:
        status = c.kind == CHANNEL_NODE
                     ? get_section(&c, request->rights, &information, &handle)
                     : ERR_NOT_SUPPORTED;
        if (status == ERR_DATA_INTEGRITY)
            say("nova-store: section refused reason=integrity\r\n");
        send(c.handle, request->transaction, status,
             status == OK ? information : 0, 0, &handle, status == OK);
        return 1;
    }
    send(c.handle, request->transaction, ERR_NOT_SUPPORTED, 0, 0, 0, 0);
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
   The service. */

static void read_bootstrap(UINT32 bootstrap)
{
    RECEIVED_HANDLE handles[STORE_BOOTSTRAP_HANDLES];
    STORE_BOOTSTRAP boot;
    UINT32 bytes, count;
    UINT64 mapped;

    if (channel_read(bootstrap, &boot, sizeof boot, handles,
                     STORE_BOOTSTRAP_HANDLES, &bytes, &count) != OK ||
        count != STORE_BOOTSTRAP_HANDLES || bytes != sizeof boot ||
        handles[STORE_BOOTSTRAP_IMAGE].type != TYPE_SECTION ||
        handles[STORE_BOOTSTRAP_SELF].type != TYPE_PROCESS ||
        handles[STORE_BOOTSTRAP_CONTROL].type != TYPE_CHANNEL ||
        handles[STORE_BOOTSTRAP_ROOT].type != TYPE_CHANNEL)
        process_exit(SERVICE_EXIT_FAILED);
    handle_close(bootstrap);
    say_open(handles[STORE_BOOTSTRAP_DEBUG_LOG].handle);
    self = handles[STORE_BOOTSTRAP_SELF].handle;
    mapped = (boot.image_size + PAGE_SIZE - 1) / PAGE_SIZE * PAGE_SIZE;
    if (!mapped ||
        process_map(self, handles[STORE_BOOTSTRAP_IMAGE].handle, 0,
                    IMAGE_BASE, mapped, MAP_READ) != OK ||
        store_image_open(&image, (const UINT8 *)IMAGE_BASE,
                         boot.image_size) != OK ||
        image.registration_count > STORE_PATHS_MAX) {
        say("nova-store: image open failed "
            "reason=\"not one this service reads\"\r\n");
        process_exit(SERVICE_EXIT_FAILED);
    }
    handle_close(handles[STORE_BOOTSTRAP_IMAGE].handle);
    add_client(handles[STORE_BOOTSTRAP_CONTROL].handle, CHANNEL_CONTROL,
               CONTROL_RIGHTS, 0);
    add_client(handles[STORE_BOOTSTRAP_ROOT].handle, CHANNEL_ROOT,
               STORE_RIGHTS, 0);
    say("nova-store: serving paths=");
    say_number(image.registration_count);
    say(" nodes=");
    say_number(image.node_count);
    say(" blobs=");
    say_number(image.blob_count);
    say("\r\n");
}

/* Wait on every channel; serve those readable, from the last, so that
   removing one, which moves the last into its place, or adding one at
   the end, leaves each index below it naming the channel it was waited
   on as; close those whose peer has closed and whose queue is empty. */
_Noreturn void store_main(UINT32 bootstrap, UINT64 argument)
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
    say("nova-store: stopped\r\n");
    process_exit(SERVICE_EXIT_DONE);
}
