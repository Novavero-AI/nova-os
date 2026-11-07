/* The view's server and init's output region (view.h). Every position
   and length a request carries is checked against the file, the reply
   buffer and the region before it is used, so no request reaches init's
   memory outside the file it names.

   The output region holds the seed chain's outputs one after another
   from OUTPUT_BASE. Each is written only by the process that creates
   it, and only while it is the last in the region, so it grows in place
   as sections are mapped at the region's end. process_map has no
   inverse yet, so the region only grows. */

#include "view.h"
#include "file_protocol.h"
#include "nt32.h"

/* The most files a process holds open at once. */
#define OPEN_FILES_MAX 8

#define REGION_SIZE (SCRATCH_BASE - OUTPUT_BASE)

/* The most bytes one output holds. init never unmaps the output region,
   so this bounds what one step can make it map; the chain's largest
   output, M2-0-0.M1, is 1.7 MB. */
#define OUTPUT_MAX_SIZE (8 * (UINT64)SECTION_MAX_SIZE)

/* The rights the server needs on an open's endpoint: to read requests,
   write replies and wait for both. */
#define ENDPOINT_RIGHTS (CHANNEL_RIGHT_READ | CHANNEL_RIGHT_WRITE | RIGHT_WAIT)

typedef struct {
    UINT32 channel;
    VIEW_FILE *file;
    UINT64 position;
    UINT32 access;
} OPEN_FILE;

static UINT8 message[CHANNEL_MAX_BYTES];
static UINT8 reply[sizeof(FILE_REPLY) + FILE_TRANSFER_MAX];
static OPEN_FILE opens[OPEN_FILES_MAX];
static UINT32 open_count;

/* The bytes of the region the sealed outputs take, and the bytes
   mapped. */
static UINT64 region_used, region_mapped;

/* A reply carries no handles. */
static const HANDLE_TRANSFER no_handles[1];

/* ------------------------------------------------------------------------
   The output region. */

UINT8 *output_next(void)
{
    return (UINT8 *)(OUTPUT_BASE + region_used);
}

void output_seal(UINT64 size)
{
    region_used += size;
}

/* Map sections at the region's end until it holds end bytes, end at
   most REGION_SIZE. */
static int region_reserve(UINT64 end)
{
    UINT32 section;
    UINT8 *mapped;

    while (region_mapped < end) {
        if (REGION_SIZE - region_mapped < SECTION_MAX_SIZE ||
            section_create(SECTION_MAX_SIZE, 0, &section) != OK)
            return 0;
        mapped = map_into_init_at(section, OUTPUT_BASE + region_mapped,
                                  SECTION_MAX_SIZE, MAP_READ | MAP_WRITE);
        handle_close(section);
        if (!mapped)
            return 0;
        region_mapped += SECTION_MAX_SIZE;
    }
    return 1;
}

/* ------------------------------------------------------------------------
   Opens. */

static int lower(int c)
{
    return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c;
}

/* Whether the length bytes at wanted are name, regardless of ASCII case
   if any_case. */
static int name_is(const char *name, const UINT8 *wanted, UINT32 length,
                   int any_case)
{
    UINT32 i;

    for (i = 0; i < length; i++) {
        if (!name[i])
            return 0;
        if (any_case ? lower(name[i]) != lower(wanted[i])
                     : (UINT8)name[i] != wanted[i])
            return 0;
    }
    return !name[length];
}

static VIEW_FILE *find(VIEW_FILE *files, UINT32 count, const UINT8 *name,
                       UINT32 length, int any_case)
{
    UINT32 i;

    for (i = 0; i < count; i++)
        if (name_is(files[i].name, name, length, any_case))
            return &files[i];
    return 0;
}

/* The outcome of opening file with the disposition and access in flags,
   NT's dispositions applied to a file the process reads, which exists,
   and to the one it writes, which exists once created. */
static UINT32 open_file(VIEW_FILE *file, UINT32 flags, UINT64 *information)
{
    UINT32 disposition = flags & FILE_FLAGS_DISPOSITION;
    int truncate = disposition == FILE_SUPERSEDE ||
                   disposition == FILE_OVERWRITE ||
                   disposition == FILE_OVERWRITE_IF;

    if (!file)
        return FILE_NOT_FOUND;
    if (disposition > FILE_OVERWRITE_IF)
        return FILE_INVALID;
    if (!file->output && ((flags & FILE_ACCESS_WRITE) || truncate ||
                          disposition == FILE_CREATE))
        return disposition == FILE_CREATE ? FILE_EXISTS : FILE_DENIED;
    if (file->exists) {
        if (disposition == FILE_CREATE)
            return FILE_EXISTS;
        if (truncate)
            file->size = 0;
        *information = disposition == FILE_SUPERSEDE   ? FILE_SUPERSEDED
                       : truncate                       ? FILE_OVERWRITTEN
                                                        : FILE_OPENED;
        return FILE_OK;
    }
    if (disposition == FILE_OPEN || disposition == FILE_OVERWRITE)
        return FILE_NOT_FOUND;
    file->exists = 1;
    file->size = 0;
    *information = FILE_CREATED;
    return FILE_OK;
}

static void answer(UINT32 channel, UINT32 transaction, UINT32 status,
                   UINT64 information, UINT32 data)
{
    FILE_REPLY *header = (FILE_REPLY *)reply;

    header->transaction = transaction;
    header->status = status;
    header->information = information;
    channel_write(channel, reply, sizeof(FILE_REPLY) + data, no_handles, 0);
}

/* A message's bytes into message, closing any handles it carries but
   the one an open passes, a channel endpoint with ENDPOINT_RIGHTS, which
   goes to *handle; HANDLE_INVALID there if there is none. Returns the
   byte count, or -1 if nothing could be read. */
static INT64 take(UINT32 channel, UINT32 *handle)
{
    RECEIVED_HANDLE handles[CHANNEL_MAX_HANDLES];
    UINT32 bytes, count, i;

    *handle = HANDLE_INVALID;
    if (channel_read(channel, message, sizeof message, handles,
                     CHANNEL_MAX_HANDLES, &bytes, &count) != OK)
        return -1;
    for (i = 0; i < count; i++) {
        if (i == 0 && handles[0].type == TYPE_CHANNEL &&
            (handles[0].rights & ENDPOINT_RIGHTS) == ENDPOINT_RIGHTS)
            *handle = handles[0].handle;
        else
            handle_close(handles[i].handle);
    }
    return bytes;
}

/* An open whose request is malformed is answered with transaction 0,
   which no request of the client's carries. Returns 0 if the directory
   holds a message that cannot be read, which would stay queued. */
static int serve_open(UINT32 directory, VIEW_FILE *files, UINT32 count)
{
    const FILE_REQUEST *request = (const FILE_REQUEST *)message;
    UINT64 information = 0;
    UINT32 endpoint, status, transaction = 0;
    VIEW_FILE *file = 0;
    INT64 bytes = take(directory, &endpoint);

    if (bytes < 0)
        return 0;
    if (endpoint == HANDLE_INVALID)
        return 1;
    if ((UINT64)bytes < sizeof(FILE_REQUEST) ||
        request->operation != FILE_OPERATION_OPEN ||
        request->length != bytes - sizeof(FILE_REQUEST)) {
        status = FILE_INVALID;
    } else {
        transaction = request->transaction;
        file = find(files, count, message + sizeof(FILE_REQUEST),
                    request->length,
                    (request->flags & FILE_NAME_ANY_CASE) != 0);
        status = open_count == OPEN_FILES_MAX
                     ? FILE_FULL
                     : open_file(file, request->flags, &information);
    }
    answer(endpoint, transaction, status, information, 0);
    if (status != FILE_OK) {
        handle_close(endpoint);
        return 1;
    }
    opens[open_count].channel = endpoint;
    opens[open_count].file = file;
    opens[open_count].position = 0;
    opens[open_count].access =
        request->flags & (FILE_ACCESS_READ | FILE_ACCESS_WRITE);
    open_count++;
    return 1;
}

/* ------------------------------------------------------------------------
   Requests on an open file. Each returns a reply's status, and sets its
   information and the bytes of data that follow it. */

static UINT32 file_read(OPEN_FILE *open, UINT64 at, UINT64 length,
                        UINT64 *information, UINT32 *data)
{
    VIEW_FILE *file = open->file;
    UINT64 count = 0, i;

    if (!(open->access & FILE_ACCESS_READ))
        return FILE_DENIED;
    if (at < file->size)
        count = file->size - at;
    if (count > length)
        count = length;
    if (count > FILE_TRANSFER_MAX)
        count = FILE_TRANSFER_MAX;
    for (i = 0; i < count; i++)
        reply[sizeof(FILE_REPLY) + i] = file->data[at + i];
    open->position = at + count;
    *information = count;
    *data = (UINT32)count;
    return FILE_OK;
}

/* The bytes follow the request. A write that starts past the end fills
   the gap with zeros; one of no bytes changes nothing. */
static UINT32 file_write(OPEN_FILE *open, UINT64 at, UINT64 length,
                         UINT64 carried, UINT64 *information)
{
    VIEW_FILE *file = open->file;
    UINT64 start, room, i;

    if (!(open->access & FILE_ACCESS_WRITE) || !file->output)
        return FILE_DENIED;
    if (length != carried)
        return FILE_INVALID;
    *information = 0;
    if (length == 0)
        return FILE_OK;
    start = (UINT64)(file->output - (UINT8 *)OUTPUT_BASE);
    room = REGION_SIZE - start < OUTPUT_MAX_SIZE ? REGION_SIZE - start
                                                 : OUTPUT_MAX_SIZE;
    if (at > room || length > room - at ||
        !region_reserve(start + at + length))
        return FILE_FULL;
    for (i = file->size; i < at; i++)
        file->output[i] = 0;
    for (i = 0; i < length; i++)
        file->output[at + i] = message[sizeof(FILE_REQUEST) + i];
    if (at + length > file->size)
        file->size = at + length;
    open->position = at + length;
    *information = length;
    return FILE_OK;
}

static UINT32 file_position(OPEN_FILE *open, UINT64 offset,
                            UINT64 *information)
{
    if (!(open->access & (FILE_ACCESS_READ | FILE_ACCESS_WRITE)))
        return FILE_DENIED;
    if (offset != FILE_OFFSET_CURRENT)
        open->position = offset;
    *information = open->position;
    return FILE_OK;
}

static UINT32 file_size(const OPEN_FILE *open, UINT64 *information)
{
    *information = open->file->size;
    return FILE_OK;
}

/* Serve one message on an open file's channel. Returns 0 if the open
   ends: its message cannot be read, which would leave it queued, or is
   no request of the protocol, which a reply sent back on its own
   channel would not be either. */
static int serve_file(OPEN_FILE *open)
{
    const FILE_REQUEST *request = (const FILE_REQUEST *)message;
    UINT32 endpoint, status, data = 0;
    INT64 bytes = take(open->channel, &endpoint);
    UINT64 information = 0, at;

    if (endpoint != HANDLE_INVALID)
        handle_close(endpoint);
    if (bytes < (INT64)sizeof(FILE_REQUEST) ||
        request->operation < FILE_OPERATION_READ ||
        request->operation > FILE_OPERATION_SIZE)
        return 0;
    at = request->offset == FILE_OFFSET_CURRENT ? open->position
                                                : request->offset;
    if (request->offset != FILE_OFFSET_CURRENT &&
        request->offset >= FILE_OFFSET_LIMIT)
        status = FILE_INVALID;
    else if (request->operation == FILE_OPERATION_READ)
        status = file_read(open, at, request->length, &information, &data);
    else if (request->operation == FILE_OPERATION_WRITE)
        status = file_write(open, at, request->length,
                            (UINT64)bytes - sizeof(FILE_REQUEST),
                            &information);
    else if (request->operation == FILE_OPERATION_POSITION)
        status = file_position(open, request->offset, &information);
    else
        status = file_size(open, &information);
    if (status != FILE_OK) {
        information = 0;
        data = 0;
    }
    answer(open->channel, request->transaction, status, information, data);
    return 1;
}

/* ------------------------------------------------------------------------
   The loop. The wait lists the process, the directory and the open
   files, in that order; a channel whose peer has closed leaves it,
   since its PEER_CLOSED stays active, as does one serve_file ends, and
   the directory is closed once serve_open cannot read it, so that the
   process sees its peer closed rather than waiting on a full queue. The
   open files are served before the directory, whose new opens join the
   end of the list, so each index still names the file it was waited on
   as. */

#define WAIT_PROCESS 0
#define WAIT_DIRECTORY 1

void view_serve(UINT32 directory, UINT32 process, VIEW_FILE *files,
                UINT32 count)
{
    WAIT_ENTRY items[WAIT_DIRECTORY + 1 + OPEN_FILES_MAX];
    int directory_open = 1, keep;
    UINT32 waits, first, waited, i;

    for (;;) {
        items[WAIT_PROCESS].handle = process;
        items[WAIT_PROCESS].signals = PROCESS_TERMINATED;
        items[WAIT_DIRECTORY].handle = directory;
        items[WAIT_DIRECTORY].signals = CHANNEL_READABLE | CHANNEL_PEER_CLOSED;
        first = waits = directory_open ? WAIT_DIRECTORY + 1 : WAIT_PROCESS + 1;
        for (i = 0; i < open_count; i++) {
            items[waits].handle = opens[i].channel;
            items[waits++].signals = CHANNEL_READABLE | CHANNEL_PEER_CLOSED;
        }
        waited = open_count;
        if (object_wait_many(items, waits, DEADLINE_INFINITE) != OK ||
            (items[WAIT_PROCESS].observed & PROCESS_TERMINATED))
            break;
        for (i = waited; i-- > 0;) {
            UINT32 observed = items[first + i].observed;

            if (observed & CHANNEL_READABLE)
                keep = serve_file(&opens[i]);
            else
                keep = !(observed & CHANNEL_PEER_CLOSED);
            if (!keep) {
                handle_close(opens[i].channel);
                opens[i] = opens[--open_count];
            }
        }
        if (directory_open) {
            if (items[WAIT_DIRECTORY].observed & CHANNEL_READABLE)
                keep = serve_open(directory, files, count);
            else
                keep = !(items[WAIT_DIRECTORY].observed & CHANNEL_PEER_CLOSED);
            if (!keep) {
                handle_close(directory);
                directory_open = 0;
            }
        }
    }
    for (i = 0; i < open_count; i++)
        handle_close(opens[i].channel);
    open_count = 0;
    if (directory_open)
        handle_close(directory);
}
