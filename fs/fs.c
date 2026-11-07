/* The file system service (docs/spec/storage.md F6, F7, F10, F18):
   serves the data volume of a pool on a block device, through the
   directory protocol (directory_protocol.h) with its write operations,
   committing every change copy-on-write. The pool's format is
   docs/spec/pool-format.md's.

   The device is a block driver's client endpoint (block_protocol.h),
   and the service's own section, which the driver pins, is both its
   block cache and the buffers every transfer names, so a block moves by
   DMA between the device and the cache page that holds it.

   Every block read from the device is checked against the SHA-256 its
   pointer carries before it is used; a block with two copies, which
   every block but a file's data has, is read from its second copy when
   its first fails, and a block with no copy that passes fails its read
   with ERR_DATA_INTEGRITY, never with its bytes (F7).

   Changes collect in the cache as dirty blocks. A dirty block is never
   evicted, and every dirty block's parent is dirty too, up to the node
   table's record of its object and from there to the root, so that a
   commit writes exactly the dirty blocks, bottom up, each to free space
   with its pointer stored in its parent (F6, step 1). A pointer in the
   cache always names a block of the last commit, or no block: no block
   is written outside a commit. A commit then flushes the device, writes
   the next superblock slot in each ring and flushes again (steps 2 to
   4); a block it releases is freed only once that last flush has
   answered, so the tree it replaces stays whole until the new one is.

   A commit comes with every IO_FLUSH, when the dirty blocks grow past
   DIRTY_LIMIT before a change starts, and when the last channel closes;
   never inside a change, so a commit holds every change whole. A change
   checks what can refuse it, the room it needs and the blocks it reads
   in part, before it modifies the cache; a step that fails after, which
   takes a metadata block failing both copies or the device failing,
   stops the service, as a commit that fails does. Its clients then see
   their channels close, and the pool holds the last commit or the one
   under way, whole.

   Each channel belongs to a client (F17): every open through the
   bootstrap root starts one, which is how the first process hands out
   /data and /users; any other open belongs to the client of the
   channel it went through. A client holds at most
   SERVICE_CLIENT_CHANNELS channels.

   One thread waits on every channel it serves and serves each message
   in turn; it ends once no channel is left, having committed. */

#include "block_protocol.h"
#include "directory_protocol.h"
#include "log.h"
#include "name.h"
#include "native.h"
#include "pool.h"
#include "services.h"

#define CHANNELS_MAX WAIT_MANY_MAX

/* The rights of the bootstrap root, the most any channel carries: the
   service maps nothing executable and publishes nothing. */
#define VOLUME_RIGHTS (IO_RIGHT_READ | IO_RIGHT_WRITE | IO_RIGHT_LIST)

/* The section shared with the driver: one page per cache block, then
   SPARE_PAGES pages for the transfers outside the cache: the GPT,
   superblocks and the root block in IO_PAGE, and while the pool opens,
   one indirect block per level from WALK_PAGE. */
#define SHARED_BASE 0x0000000200000000ULL
#define SHARED_SIZE SECTION_MAX_SIZE
#define SHARED_PAGES (SHARED_SIZE / PAGE_SIZE)
#define SPARE_PAGES 8
#define CACHE_BLOCKS (SHARED_PAGES - SPARE_PAGES)
#define IO_PAGE CACHE_BLOCKS
#define WALK_PAGE (IO_PAGE + 1)

_Static_assert(POOL_BLOCK_SIZE == PAGE_SIZE, "a pool block is a page");
_Static_assert(1 + POOL_LEVELS_MAX <= SPARE_PAGES,
               "a walk has a page for each level below the root");

/* The largest pool the service opens: its two block maps, one bit a
   block, are BITMAP_BYTES each. */
#define POOL_BLOCKS_MAX (1ULL << 20)
#define BITS_PER_BYTE 8
#define BITMAP_BYTES (POOL_BLOCKS_MAX / BITS_PER_BYTE)

/* The most node slots, and so files and directories, a volume holds. */
#define NODES_MAX 65536
#define NODE_BITMAP_BYTES (NODES_MAX / BITS_PER_BYTE)

/* A directory's entries are at most DIRECTORY_MAX bytes, which the
   service reads whole into its scratch buffer for each lookup and
   change. */
#define DIRECTORY_MAX 65536
#define DIRECTORY_BLOCKS_MAX (DIRECTORY_MAX / POOL_BLOCK_SIZE + 1)

/* The most a file holds: the blocks POOL_LEVELS_MAX levels address. */
#define FANOUT_SQUARED ((UINT64)POOL_FANOUT * POOL_FANOUT)
#define FILE_BLOCKS_MAX (FANOUT_SQUARED * FANOUT_SQUARED)
#define FILE_SIZE_MAX (FILE_BLOCKS_MAX * POOL_BLOCK_SIZE)
_Static_assert(POOL_LEVELS_MAX == 5, "FILE_BLOCKS_MAX is POOL_FANOUT^4");

/* What one change can dirty at most: a rename's two directory edits of
   DIRECTORY_BLOCKS_MAX blocks each, every edit's and record's path to
   its root, POOL_LEVELS_MAX blocks each, and a node table that grows.
   A dirty block needs at most POOL_COPIES blocks at the commit. A change
   starts only with CHANGE_DIRTY_MAX blocks of the cache left to dirty
   and LOOKUP_BUFFERS more for the reads it makes; once the dirty blocks
   pass DIRTY_LIMIT, the next change commits first. */
#define CHANGE_DIRTY_MAX (2 * DIRECTORY_BLOCKS_MAX + 8 * POOL_LEVELS_MAX)
#define LOOKUP_BUFFERS 16
#define DIRTY_LIMIT (CACHE_BLOCKS - CHANGE_DIRTY_MAX - LOOKUP_BUFFERS)
_Static_assert(DIRTY_LIMIT > CACHE_BLOCKS / 2, "the cache mostly batches");

/* The blocks a change may not take, so that a remove, which frees blocks
   but writes metadata to do so, still commits on a full pool (F6). */
#define POOL_RESERVE_BLOCKS 64

/* The node table, an object with no slot of its own; and no slot. */
#define OBJECT_TABLE 0xffffffffu
#define NO_SLOT 0xffffffffu
_Static_assert(NODES_MAX < NO_SLOT, "no slot is a node's");

#define BUFFER_FREE 0
#define BUFFER_CLEAN 1
#define BUFFER_DIRTY 2

/* How get_buffer fills a block not in the cache: read it, or leave it
   zero for a caller that writes all of it. */
#define FILL_READ 0
#define FILL_NONE 1

/* Transactions a program assigns itself have bit 31 clear (ipc.md I3). */
#define TRANSACTION_MASK 0x7fffffffu

/* The GPT the service finds its pool in (UEFI 2.11 5.3.2, 5.3.3): the
   header's fields, the CRC over the header with its own CRC field taken
   as zero, and the entries' fields. */
#define GPT_HEADER_LBA 1
#define GPT_SIGNATURE "EFI PART"
#define GPT_SIGNATURE_SIZE 8
#define GPT_HEADER_SIZE_FIELD 12
#define GPT_HEADER_CRC 16
#define GPT_HEADER_CRC_SIZE 4
#define GPT_MY_LBA 24
#define GPT_FIRST_USABLE 40
#define GPT_LAST_USABLE 48
#define GPT_HEADER_MIN 92
#define GPT_ENTRIES_CRC 88
#define GPT_ENTRIES_LBA 72
#define GPT_ENTRY_COUNT 80
#define GPT_ENTRY_SIZE 84
#define GPT_ENTRY_MIN 128
#define GPT_ENTRY_FIRST 32
#define GPT_ENTRY_LAST 40
#define GPT_ENTRIES_MAX 128

_Static_assert(POOL_KIND_DIRECTORY == IO_KIND_DIRECTORY &&
                   POOL_KIND_FILE == IO_KIND_FILE,
               "a record's kind is the protocol's");

typedef struct {
    UINT32 object;
    UINT32 level;
    UINT64 index;
    UINT32 state;
    UINT64 used;
} BUFFER;

/* Whether opening a directory through a channel starts a client. */
typedef enum {
    OPENS_JOIN = 1,
    OPENS_START_CLIENTS
} OPENS;

typedef struct {
    UINT32 handle;
    UINT32 client;
    UINT32 rights;
    UINT32 slot;
    UINT32 generation;
    UINT32 kind;
    OPENS opens;
} CHANNEL;

/* A directory entry, as it lies at offset at of the scratch buffer. */
typedef struct {
    UINT32 at;
    UINT32 bytes;
    UINT32 slot;
    UINT32 kind;
    const UINT8 *name;
    UINT32 length;
} ENTRY;

static UINT32 self = HANDLE_INVALID;
static UINT32 block_channel = HANDLE_INVALID;
static UINT32 last_transaction;
static UINT8 *shared;

/* The pool: its first sector and blocks; the newest commit and its root;
   the blocks in use, of the last commit and allocated since, and those
   released since, which stay in use until the commit is durable. */
static UINT64 pool_first_lba, pool_blocks, pool_middle;
static UINT64 committed_txg;
static POOL_POINTER committed_root;
static POOL_ROOT volume;
static int root_dirty;
static UINT8 used_map[BITMAP_BYTES];
static UINT8 freeing_map[BITMAP_BYTES];
static UINT64 used_blocks, freeing_blocks;
static UINT64 cursor[POOL_COPIES];
static UINT8 node_map[NODE_BITMAP_BYTES];

static BUFFER buffers[CACHE_BLOCKS];
static UINT64 tick;
static UINT32 dirty_count;

static UINT8 directory[DIRECTORY_MAX];
static UINT32 directory_size;

static CHANNEL channels[CHANNELS_MAX];
static UINT32 channel_count;
static UINT32 client_channels[SERVICE_CLIENTS];

static UINT8 message[CHANNEL_MAX_BYTES];
static UINT8 reply[CHANNEL_MAX_BYTES];
static RECEIVED_HANDLE received[CHANNEL_MAX_HANDLES];
static UINT32 received_count;

/* channel_write needs a valid array even for no handles. */
static const HANDLE_TRANSFER no_handles[1];

static UINT32 commit(void);

static void copy(UINT8 *to, const UINT8 *from, UINT64 count)
{
    UINT64 i;

    for (i = 0; i < count; i++)
        to[i] = from[i];
}

/* Move count bytes within one buffer, either way. */
static void move(UINT8 *to, const UINT8 *from, UINT64 count)
{
    UINT64 i;

    if (to < from)
        for (i = 0; i < count; i++)
            to[i] = from[i];
    else
        for (i = count; i-- > 0;)
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

static int bit(const UINT8 *map, UINT64 n)
{
    return map[n / BITS_PER_BYTE] >> (n % BITS_PER_BYTE) & 1;
}

static void set_bit(UINT8 *map, UINT64 n)
{
    map[n / BITS_PER_BYTE] |= (UINT8)(1 << (n % BITS_PER_BYTE));
}

static void clear_bit(UINT8 *map, UINT64 n)
{
    map[n / BITS_PER_BYTE] &= (UINT8)~(1 << (n % BITS_PER_BYTE));
}

static void say_status(const char *what, UINT32 status)
{
    say("nova-fs: ");
    say(what);
    say(", status ");
    say_number(status);
    say("\r\n");
}

_Noreturn static void stop(const char *why, UINT32 status)
{
    say_status(why, status);
    say("nova-fs: stopping\r\n");
    process_exit(SERVICE_EXIT_FAILED);
}

/* FANOUT^level: the blocks one block at level covers. */
static UINT64 span(UINT32 level)
{
    UINT64 n = 1;

    while (level--)
        n *= POOL_FANOUT;
    return n;
}

/* ------------------------------------------------------------------------
   The device. */

static UINT8 *page(UINT32 n)
{
    return shared + (UINT64)n * PAGE_SIZE;
}

/* One request to the driver, answered before the next is sent, so the
   device sees them in this order (block_protocol.h). A driver that has
   gone stops the service. */
static UINT32 block_call(UINT32 operation, UINT64 sector, UINT32 length,
                         UINT32 buffer, UINT64 *information)
{
    BLOCK_REQUEST request;
    BLOCK_REPLY answer;
    UINT32 status, observed, bytes, count;

    last_transaction = (last_transaction + 1) & TRANSACTION_MASK;
    if (!last_transaction)
        last_transaction = 1;
    request.transaction = last_transaction;
    request.operation = operation;
    request.sector = sector;
    request.length = length;
    request.buffer = buffer;
    status = channel_write(block_channel, &request, sizeof request,
                           no_handles, 0);
    if (status == OK)
        status = object_wait_one(block_channel,
                                 CHANNEL_READABLE | CHANNEL_PEER_CLOSED,
                                 DEADLINE_INFINITE, &observed);
    if (status == OK)
        status = channel_read(block_channel, &answer, sizeof answer,
                              received, 0, &bytes, &count);
    if (status != OK || bytes != sizeof answer || count != 0 ||
        answer.transaction != request.transaction)
        stop("the block driver did not answer", status);
    if (information)
        *information = answer.information;
    return answer.status;
}

static UINT64 sector_of(UINT64 block)
{
    return pool_first_lba + block * POOL_SECTORS_PER_BLOCK;
}

/* Whether block lies in the data area, outside both rings. */
static int in_data_area(UINT64 block)
{
    return block >= POOL_RING_SLOTS && block < pool_blocks - POOL_RING_SLOTS;
}

static UINT32 read_page(UINT64 block, UINT32 n)
{
    return block_call(BLOCK_READ, sector_of(block), POOL_BLOCK_SIZE,
                      n * PAGE_SIZE, 0);
}

static UINT32 write_page(UINT64 block, UINT32 n)
{
    return block_call(BLOCK_WRITE, sector_of(block), POOL_BLOCK_SIZE,
                      n * PAGE_SIZE, 0);
}

/* Read the block pointer names into page n, checked against its digest:
   its first copy, else its second. A copy outside the data area fails
   its check as one whose bytes do. */
static UINT32 read_checked(const POOL_POINTER *pointer, UINT32 n)
{
    UINT8 digest[POOL_DIGEST_SIZE];
    UINT32 i, status = ERR_DATA_INTEGRITY;

    for (i = 0; i < POOL_COPIES; i++) {
        if (!pointer->address[i])
            continue;
        if (!in_data_area(pointer->address[i])) {
            status = ERR_DATA_INTEGRITY;
        } else {
            status = read_page(pointer->address[i], n);
            if (status != OK)
                continue;
            pool_digest(page(n), POOL_BLOCK_SIZE, digest);
            if (pool_same_digest(digest, pointer->digest))
                return OK;
            status = ERR_DATA_INTEGRITY;
        }
        say("nova-fs: block ");
        say_number(pointer->address[i]);
        say(" fails its checksum\r\n");
    }
    return status;
}

/* ------------------------------------------------------------------------
   Space. A block is taken from the first free one at or after a cursor,
   the first copy's from the data area's start and the second's from the
   pool's middle, so a block's two copies lie apart (F7). */

static UINT64 free_blocks(void)
{
    return pool_blocks - 2 * POOL_RING_SLOTS - used_blocks;
}

static UINT32 allocate(UINT32 copy_index, UINT64 *block)
{
    UINT64 n, tries, data = pool_blocks - 2 * POOL_RING_SLOTS;

    n = cursor[copy_index];
    for (tries = 0; tries < data; tries++) {
        if (!in_data_area(n))
            n = POOL_RING_SLOTS;
        if (!bit(used_map, n)) {
            set_bit(used_map, n);
            used_blocks++;
            cursor[copy_index] = n + 1;
            *block = n;
            return OK;
        }
        n++;
    }
    return ERR_NO_RESOURCES;
}

/* Release pointer's blocks once the commit under way is durable. */
static void release(const POOL_POINTER *pointer)
{
    UINT32 i;

    for (i = 0; i < POOL_COPIES; i++)
        if (pointer->address[i] && !bit(freeing_map, pointer->address[i])) {
            set_bit(freeing_map, pointer->address[i]);
            freeing_blocks++;
        }
}

/* ------------------------------------------------------------------------
   The cache. A BUFFER's bytes stay where they are only while no other
   buffer is loaded, unless it is dirty. */

static UINT8 *data_of(UINT32 b)
{
    return page(b);
}

static int find_buffer(UINT32 object, UINT32 level, UINT64 index, UINT32 *b)
{
    UINT32 i;

    for (i = 0; i < CACHE_BLOCKS; i++)
        if (buffers[i].state != BUFFER_FREE && buffers[i].object == object &&
            buffers[i].level == level && buffers[i].index == index) {
            buffers[i].used = ++tick;
            *b = i;
            return 1;
        }
    return 0;
}

/* A free buffer, or the least recently used clean one. */
static UINT32 take_buffer(UINT32 *b)
{
    UINT32 i, best = CACHE_BLOCKS;

    for (i = 0; i < CACHE_BLOCKS; i++) {
        if (buffers[i].state == BUFFER_FREE) {
            *b = i;
            return OK;
        }
        if (buffers[i].state == BUFFER_CLEAN &&
            (best == CACHE_BLOCKS || buffers[i].used < buffers[best].used))
            best = i;
    }
    if (best == CACHE_BLOCKS)
        return ERR_NO_RESOURCES;
    *b = best;
    return OK;
}

static void drop_buffer(UINT32 b)
{
    if (buffers[b].state == BUFFER_DIRTY)
        dirty_count--;
    buffers[b].state = BUFFER_FREE;
}

/* Drop every cached block of object that covers only data blocks from
   first on. */
static void drop_from(UINT32 object, UINT64 first)
{
    UINT32 i;

    for (i = 0; i < CACHE_BLOCKS; i++)
        if (buffers[i].state != BUFFER_FREE && buffers[i].object == object &&
            buffers[i].index * span(buffers[i].level) >= first)
            drop_buffer(i);
}

static UINT32 get_buffer(UINT32 object, UINT32 level, UINT64 index,
                         UINT32 fill, UINT32 *b);

/* The record of slot, from its node table block. */
static UINT32 record_get(UINT32 slot, POOL_RECORD *record)
{
    UINT32 b, status = get_buffer(OBJECT_TABLE, 0,
                                  slot / POOL_RECORDS_PER_BLOCK, FILL_READ,
                                  &b);

    if (status == OK)
        pool_record_read(data_of(b) + slot % POOL_RECORDS_PER_BLOCK *
                                          POOL_RECORD_SIZE,
                         record);
    return status;
}

/* An object's levels and root pointer, in a record: the node table's
   from the root block. */
static UINT32 object_get(UINT32 object, POOL_RECORD *record)
{
    if (object != OBJECT_TABLE)
        return record_get(object, record);
    record->kind = POOL_KIND_FREE;
    record->flags = 0;
    record->levels = volume.levels;
    record->generation = 0;
    record->size = volume.slots * POOL_RECORD_SIZE;
    record->root = volume.table;
    return OK;
}

/* The pointer to block index at level of object, from its parent or its
   record. A block past the object's tree is a hole. */
static UINT32 pointer_of(UINT32 object, UINT32 level, UINT64 index,
                         POOL_POINTER *pointer)
{
    POOL_RECORD record;
    UINT32 b, status = object_get(object, &record);

    if (status != OK)
        return status;
    pool_hole(pointer);
    if (level + 1 > record.levels)
        return OK;
    if (level + 1 == record.levels) {
        if (index == 0)
            *pointer = record.root;
        return OK;
    }
    status = get_buffer(object, level + 1, index / POOL_FANOUT, FILL_READ, &b);
    if (status == OK)
        pool_pointer_read(data_of(b) +
                              index % POOL_FANOUT * POOL_POINTER_SIZE,
                          pointer);
    return status;
}

/* Block index at level of object, in the cache: read and checked, or
   zero for a hole or for FILL_NONE. */
static UINT32 get_buffer(UINT32 object, UINT32 level, UINT64 index,
                         UINT32 fill, UINT32 *b)
{
    POOL_POINTER pointer;
    UINT32 status;

    if (find_buffer(object, level, index, b))
        return OK;
    pool_hole(&pointer);
    status = fill == FILL_READ ? pointer_of(object, level, index, &pointer)
                               : OK;
    if (status == OK)
        status = take_buffer(b);
    if (status != OK)
        return status;
    buffers[*b].state = BUFFER_FREE;
    if (pool_is_hole(&pointer))
        pool_zero(data_of(*b), POOL_BLOCK_SIZE);
    else
        status = read_checked(&pointer, *b);
    if (status != OK)
        return status;
    buffers[*b].object = object;
    buffers[*b].level = level;
    buffers[*b].index = index;
    buffers[*b].state = BUFFER_CLEAN;
    buffers[*b].used = ++tick;
    return OK;
}

/* Mark b dirty, and its parent, up through the node table to the root,
   so that a dirty block's parent is always dirty. b is marked first, so
   loading its parents never evicts it. */
static UINT32 dirty(UINT32 b)
{
    POOL_RECORD record;
    UINT32 object = buffers[b].object, level = buffers[b].level, parent;
    UINT64 index = buffers[b].index;
    UINT32 status;

    if (buffers[b].state == BUFFER_DIRTY)
        return OK;
    buffers[b].state = BUFFER_DIRTY;
    dirty_count++;
    status = object_get(object, &record);
    if (status != OK)
        return status;
    if (level + 1 < record.levels)
        status = get_buffer(object, level + 1, index / POOL_FANOUT, FILL_READ,
                            &parent);
    else if (object == OBJECT_TABLE) {
        root_dirty = 1;
        return OK;
    } else
        status = get_buffer(OBJECT_TABLE, 0, object / POOL_RECORDS_PER_BLOCK,
                            FILL_READ, &parent);
    return status == OK ? dirty(parent) : status;
}

static UINT32 object_put(UINT32 object, const POOL_RECORD *record);

/* Give object at least levels levels: each new level is a dirty root
   whose first pointer is the old root. */
static UINT32 grow(UINT32 object, UINT32 levels)
{
    POOL_RECORD record;
    UINT32 b, status = object_get(object, &record);

    while (status == OK && record.levels < levels) {
        if (record.levels > 0) {
            if (!find_buffer(object, record.levels, 0, &b))
                status = take_buffer(&b);
            if (status != OK)
                return status;
            if (buffers[b].state == BUFFER_DIRTY)
                dirty_count--;
            pool_zero(data_of(b), POOL_BLOCK_SIZE);
            pool_pointer_write(data_of(b), &record.root);
            buffers[b].object = object;
            buffers[b].level = record.levels;
            buffers[b].index = 0;
            buffers[b].state = BUFFER_DIRTY;
            buffers[b].used = ++tick;
            dirty_count++;
            pool_hole(&record.root);
        }
        record.levels++;
        status = object_put(object, &record);
    }
    return status;
}

/* Levels enough to hold block index. */
static UINT32 levels_for(UINT64 index)
{
    UINT32 levels = 1;

    while (index >= span(levels - 1))
        levels++;
    return levels;
}

/* Block index of object, at level 0, dirty in the cache. */
static UINT32 block_for_write(UINT32 object, UINT64 index, UINT32 fill,
                              UINT32 *b)
{
    UINT32 status = grow(object, levels_for(index));

    if (status == OK)
        status = get_buffer(object, 0, index, fill, b);
    if (status == OK)
        status = dirty(*b);
    return status;
}

static UINT32 record_put(UINT32 slot, const POOL_RECORD *record)
{
    UINT32 b, status = block_for_write(OBJECT_TABLE,
                                       slot / POOL_RECORDS_PER_BLOCK,
                                       FILL_READ, &b);

    if (status == OK)
        pool_record_write(data_of(b) + slot % POOL_RECORDS_PER_BLOCK *
                                           POOL_RECORD_SIZE,
                          record);
    return status;
}

static UINT32 object_put(UINT32 object, const POOL_RECORD *record)
{
    if (object != OBJECT_TABLE)
        return record_put(object, record);
    volume.levels = record->levels;
    volume.table = record->root;
    root_dirty = 1;
    return OK;
}

/* ------------------------------------------------------------------------
   Objects: a file's bytes, a directory's entries. */

static UINT32 object_read(UINT32 object, UINT64 offset, UINT8 *to,
                          UINT64 length)
{
    UINT32 b, status;
    UINT64 n, within;

    while (length) {
        within = offset % POOL_BLOCK_SIZE;
        n = POOL_BLOCK_SIZE - within < length ? POOL_BLOCK_SIZE - within
                                              : length;
        status = get_buffer(object, 0, offset / POOL_BLOCK_SIZE, FILL_READ, &b);
        if (status != OK)
            return status;
        copy(to, data_of(b) + within, n);
        to += n;
        offset += n;
        length -= n;
    }
    return OK;
}

/* A change's step after it has begun to modify the cache. Every step
   that can fail for a reason other than a broken pool or device is
   checked before the change begins, so a step that fails here leaves a
   change half made in the cache, which no commit may hold: the service
   stops, its clients see their channels close, and the pool holds its
   last commit, as after a crash. */
static void changed(UINT32 status)
{
    if (status != OK)
        stop("a change failed after it began; the pool holds its last commit",
             status);
}

/* Read block index of object, which a change writes only in part, and
   hold it dirty, if it lies in the object's tree: a block that fails its
   check fails the change before anything has changed. Marking a block
   dirty changes no byte of it. */
static UINT32 hold_partial(UINT32 object, UINT64 index)
{
    POOL_RECORD record;
    UINT32 b, status = object_get(object, &record);

    if (status != OK || !record.levels || index >= span(record.levels - 1))
        return status;
    status = get_buffer(object, 0, index, FILL_READ, &b);
    if (status == OK)
        changed(dirty(b));
    return status;
}

/* Write length bytes at offset, at most FILE_SIZE_MAX in all, growing
   the object's size to cover them. A block written whole is not read;
   one written in part is read before anything changes, and its failure
   is the write's. */
static UINT32 object_write(UINT32 object, UINT64 offset, const UINT8 *from,
                           UINT64 length)
{
    POOL_RECORD record;
    UINT32 b, status = OK;
    UINT64 n, within, end = offset + length;
    UINT64 first = offset / POOL_BLOCK_SIZE;
    UINT64 last = (end - 1) / POOL_BLOCK_SIZE;

    if (!length)
        return OK;
    if (offset % POOL_BLOCK_SIZE || (first == last && end % POOL_BLOCK_SIZE))
        status = hold_partial(object, first);
    if (status == OK && last != first && end % POOL_BLOCK_SIZE)
        status = hold_partial(object, last);
    if (status != OK)
        return status;
    while (length) {
        within = offset % POOL_BLOCK_SIZE;
        n = POOL_BLOCK_SIZE - within < length ? POOL_BLOCK_SIZE - within
                                              : length;
        changed(block_for_write(object, offset / POOL_BLOCK_SIZE,
                                n == POOL_BLOCK_SIZE ? FILL_NONE : FILL_READ,
                                &b));
        copy(data_of(b) + within, from, n);
        from += n;
        offset += n;
        length -= n;
    }
    changed(object_get(object, &record));
    if (end > record.size) {
        record.size = end;
        changed(object_put(object, &record));
    }
    return OK;
}

/* Release every block of the tree under pointer, block index at level of
   object, and pointer's own. An indirect block a commit has yet to write
   has a hole for its pointer, and may still name blocks of the last
   commit, as a root that grow pushed down does. */
static UINT32 free_tree(UINT32 object, UINT32 level, UINT64 index,
                        const POOL_POINTER *pointer)
{
    POOL_POINTER child;
    UINT32 b, k, status;

    if (pool_is_hole(pointer) &&
        !(level > 0 && find_buffer(object, level, index, &b)))
        return OK;
    for (k = 0; level > 0 && k < POOL_FANOUT; k++) {
        status = get_buffer(object, level, index, FILL_READ, &b);
        if (status != OK)
            return status;
        pool_pointer_read(data_of(b) + k * POOL_POINTER_SIZE, &child);
        status = free_tree(object, level - 1, index * POOL_FANOUT + k, &child);
        if (status != OK)
            return status;
    }
    release(pointer);
    return OK;
}

/* Under block index at level of object, which covers data blocks below
   keep and possibly some from keep on, release every subtree that holds
   only blocks from keep on and clear its pointer. */
static UINT32 cut(UINT32 object, UINT32 level, UINT64 index, UINT64 keep)
{
    POOL_POINTER child;
    UINT64 child_span = span(level - 1), first;
    UINT32 b, k, status = get_buffer(object, level, index, FILL_READ, &b);

    if (status == OK)
        status = dirty(b);
    for (k = 0; status == OK && k < POOL_FANOUT; k++) {
        first = (index * POOL_FANOUT + k) * child_span;
        if (first >= keep) {
            pool_pointer_read(data_of(b) + k * POOL_POINTER_SIZE, &child);
            status = free_tree(object, level - 1, index * POOL_FANOUT + k,
                               &child);
            pool_hole(&child);
            pool_pointer_write(data_of(b) + k * POOL_POINTER_SIZE, &child);
        } else if (first + child_span > keep && level > 1) {
            status = cut(object, level - 1, index * POOL_FANOUT + k, keep);
        }
    }
    return status;
}

/* Set object's size, releasing the blocks past it and zeroing the rest
   of its last block, so that a block's bytes past the size are always
   zero and a later extension reads zeros there. A size past the blocks
   the object's tree holds is a hole to its end. The last block is read
   before anything changes, and its failure is the truncation's. */
static UINT32 object_truncate(UINT32 object, UINT64 size)
{
    POOL_RECORD record;
    UINT64 keep = (size + POOL_BLOCK_SIZE - 1) / POOL_BLOCK_SIZE;
    UINT32 b, status = object_get(object, &record);

    if (status != OK || size == record.size)
        return status;
    if (size < record.size && size % POOL_BLOCK_SIZE) {
        status = hold_partial(object, keep - 1);
        if (status != OK)
            return status;
    }
    if (size < record.size && keep == 0) {
        if (record.levels)
            changed(free_tree(object, record.levels - 1, 0, &record.root));
        drop_from(object, 0);
        record.levels = 0;
        pool_hole(&record.root);
    } else if (size < record.size) {
        if (record.levels > 1)
            changed(cut(object, record.levels - 1, 0, keep));
        drop_from(object, keep);
        if (size % POOL_BLOCK_SIZE && find_buffer(object, 0, keep - 1, &b))
            pool_zero(data_of(b) + size % POOL_BLOCK_SIZE,
                      POOL_BLOCK_SIZE - size % POOL_BLOCK_SIZE);
        changed(object_get(object, &record));
    }
    record.size = size;
    changed(object_put(object, &record));
    return OK;
}

/* ------------------------------------------------------------------------
   Nodes. */

/* The record of a live node at slot, whose generation is generation; a
   node removed since a channel opened it is not found. */
static UINT32 node_get(UINT32 slot, UINT32 generation, POOL_RECORD *record)
{
    UINT32 status;

    if (slot >= volume.slots || !bit(node_map, slot))
        return ERR_NOT_FOUND;
    status = record_get(slot, record);
    if (status == OK && record->generation != generation)
        status = ERR_NOT_FOUND;
    return status;
}

/* The first free slot, which is volume.slots when every slot is taken
   and the node table grows by a block for the next. */
static UINT32 free_slot(void)
{
    UINT32 n;

    for (n = 0; n < volume.slots && bit(node_map, n); n++)
        ;
    return n;
}

/* Whether a node can be created: a free slot, or room for the node
   table to grow. */
static int node_room(void)
{
    return free_slot() < volume.slots ||
           volume.slots + POOL_RECORDS_PER_BLOCK <= NODES_MAX;
}

/* A new empty node of kind in the first free slot, the node table
   growing by a block when it has none, as node_room allows. */
static UINT32 node_create(UINT32 kind, UINT32 *slot, UINT32 *generation)
{
    POOL_RECORD record;
    UINT32 n = free_slot(), status;

    if (n == volume.slots) {
        volume.slots += POOL_RECORDS_PER_BLOCK;
        root_dirty = 1;
    }
    status = record_get(n, &record);
    if (status != OK)
        return status;
    record.kind = (UINT8)kind;
    record.flags = 0;
    record.levels = 0;
    record.generation = record.generation + 1 ? record.generation + 1 : 1;
    record.size = 0;
    pool_hole(&record.root);
    status = record_put(n, &record);
    if (status != OK)
        return status;
    set_bit(node_map, n);
    *slot = n;
    *generation = record.generation;
    return OK;
}

/* Release a node's blocks and free its slot, keeping its generation. */
static UINT32 node_remove(UINT32 slot)
{
    POOL_RECORD record;
    UINT32 status = object_truncate(slot, 0);

    if (status == OK)
        status = record_get(slot, &record);
    if (status != OK)
        return status;
    record.kind = POOL_KIND_FREE;
    record.flags = 0;
    status = record_put(slot, &record);
    if (status == OK)
        clear_bit(node_map, slot);
    return status;
}

/* ------------------------------------------------------------------------
   Directories, read whole into the scratch buffer. */

static void entry_at(UINT32 at, ENTRY *entry)
{
    entry->at = at;
    entry->slot = pool_get32(directory + at + POOL_ENTRY_SLOT);
    entry->kind = directory[at + POOL_ENTRY_KIND];
    entry->length = directory[at + POOL_ENTRY_LENGTH];
    entry->name = directory + at + POOL_ENTRY_NAME;
    entry->bytes = POOL_ENTRY_NAME + entry->length;
}

/* Read directory slot's entries, each checked: whole, a valid name in
   bytewise order after the last, a live slot of the kind it says. */
static UINT32 directory_load(UINT32 slot)
{
    POOL_RECORD record;
    ENTRY entry, last;
    UINT32 at, status = record_get(slot, &record);

    if (status != OK)
        return status;
    if (record.kind != POOL_KIND_DIRECTORY || record.size > DIRECTORY_MAX)
        return ERR_DATA_INTEGRITY;
    status = object_read(slot, 0, directory, record.size);
    if (status != OK)
        return status;
    directory_size = (UINT32)record.size;
    for (at = 0; at < directory_size; at += entry.bytes) {
        if (directory_size - at < POOL_ENTRY_NAME)
            return ERR_DATA_INTEGRITY;
        entry_at(at, &entry);
        if (entry.bytes > directory_size - at ||
            !name_valid(entry.name, entry.length) ||
            (at && compare(last.name, last.length, entry.name,
                           entry.length) >= 0) ||
            entry.slot >= volume.slots || !bit(node_map, entry.slot) ||
            (entry.kind != POOL_KIND_DIRECTORY &&
             entry.kind != POOL_KIND_FILE))
            return ERR_DATA_INTEGRITY;
        last = entry;
    }
    return OK;
}

/* The entry named by the length bytes at name in the loaded directory, or
   where it would go; whether it is there. */
static int directory_find(const UINT8 *name, UINT32 length, ENTRY *entry)
{
    UINT32 at;
    int order;

    for (at = 0; at < directory_size; at += entry->bytes) {
        entry_at(at, entry);
        order = compare(name, length, entry->name, entry->length);
        if (order == 0)
            return 1;
        if (order < 0)
            break;
    }
    entry->at = at < directory_size ? at : directory_size;
    return 0;
}

/* Whether the loaded directory has room for an entry named by length
   bytes, with freed more bytes freed first. */
static int directory_room(UINT32 length, UINT32 freed)
{
    return POOL_ENTRY_NAME + length <= DIRECTORY_MAX - directory_size + freed;
}

/* Insert an entry for slot, of kind, named by the length bytes at name,
   at at in the loaded directory, slot's, which has room for it, and write
   it back from there. */
static UINT32 directory_insert(UINT32 directory_slot, UINT32 at,
                               const UINT8 *name, UINT32 length, UINT32 slot,
                               UINT32 kind)
{
    UINT32 bytes = POOL_ENTRY_NAME + length;

    move(directory + at + bytes, directory + at, directory_size - at);
    pool_put32(directory + at + POOL_ENTRY_SLOT, slot);
    directory[at + POOL_ENTRY_KIND] = (UINT8)kind;
    directory[at + POOL_ENTRY_LENGTH] = (UINT8)length;
    copy(directory + at + POOL_ENTRY_NAME, name, length);
    directory_size += bytes;
    return object_write(directory_slot, at, directory + at,
                        directory_size - at);
}

/* Remove the loaded directory's entry, slot's, and write it back. */
static UINT32 directory_delete(UINT32 directory_slot, const ENTRY *entry)
{
    UINT32 status = OK;

    move(directory + entry->at, directory + entry->at + entry->bytes,
         directory_size - entry->at - entry->bytes);
    directory_size -= entry->bytes;
    if (entry->at < directory_size)
        status = object_write(directory_slot, entry->at,
                              directory + entry->at,
                              directory_size - entry->at);
    return status == OK ? object_truncate(directory_slot, directory_size)
                        : status;
}

/* ------------------------------------------------------------------------
   Paths: a component at a time from a directory, each a name, as the
   protocol requires (directory_protocol.h). */

static UINT32 component(const UINT8 *path, UINT32 length, UINT32 *rest)
{
    UINT32 n = 0;

    while (n < length && path[n] != '/')
        n++;
    *rest = n < length ? n + 1 : n;
    return n;
}

/* Every component a name, and no trailing '/'; at most IO_PATH_MAX. */
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

/* The node at path below directory from, its slot and kind. */
static UINT32 resolve(UINT32 from, const UINT8 *path, UINT32 length,
                      UINT32 *slot, UINT32 *kind)
{
    ENTRY entry;
    UINT32 at = 0, next, n, status;

    *slot = from;
    *kind = POOL_KIND_DIRECTORY;
    while (at < length) {
        if (*kind != POOL_KIND_DIRECTORY)
            return ERR_NOT_FOUND;
        n = component(path + at, length - at, &next);
        status = directory_load(*slot);
        if (status != OK)
            return status;
        if (!directory_find(path + at, n, &entry))
            return ERR_NOT_FOUND;
        *slot = entry.slot;
        *kind = entry.kind;
        at += next;
    }
    return OK;
}

/* The directory holding path's last component, below from, and that
   component: path is not empty. */
static UINT32 resolve_parent(UINT32 from, const UINT8 *path, UINT32 length,
                             UINT32 *parent, UINT32 *name)
{
    UINT32 last = length, kind, status;

    while (last > 0 && path[last - 1] != '/')
        last--;
    *name = last;
    status = resolve(from, path, last ? last - 1 : 0, parent, &kind);
    if (status == OK && kind != POOL_KIND_DIRECTORY)
        status = ERR_NOT_FOUND;
    return status;
}

/* ------------------------------------------------------------------------
   Commits. */

/* How many blocks a block of object at level has: two for metadata,
   one for a file's data. */
static UINT32 copies_of(UINT32 object, UINT32 level, UINT32 *copies)
{
    POOL_RECORD record;
    UINT32 status;

    *copies = POOL_COPIES;
    if (object == OBJECT_TABLE || level > 0)
        return OK;
    status = record_get(object, &record);
    if (status == OK && record.kind == POOL_KIND_FILE)
        *copies = 1;
    return status;
}

/* The blocks a commit of the cache's dirty blocks takes: the most each
   dirty block needs, and the root block's two. */
static UINT64 commit_blocks(void)
{
    return (UINT64)dirty_count * POOL_COPIES + POOL_COPIES;
}

/* Write block b to new blocks, release the blocks its old pointer names
   and store its new pointer where the old one was: its parent, dirty
   and in the cache, or its object's record or the root block. */
static UINT32 write_dirty(UINT32 b, UINT64 txg)
{
    POOL_RECORD record;
    POOL_POINTER old, fresh;
    UINT32 object = buffers[b].object, level = buffers[b].level;
    UINT64 index = buffers[b].index;
    UINT32 copies, i, parent = CACHE_BLOCKS;
    UINT32 status = object_get(object, &record);

    if (status == OK)
        status = copies_of(object, level, &copies);
    if (status != OK)
        return status;
    if (level + 1 < record.levels) {
        if (!find_buffer(object, level + 1, index / POOL_FANOUT, &parent) ||
            buffers[parent].state != BUFFER_DIRTY)
            return ERR_BAD_STATE;
        pool_pointer_read(data_of(parent) +
                              index % POOL_FANOUT * POOL_POINTER_SIZE,
                          &old);
    } else if (level + 1 == record.levels && index == 0) {
        old = record.root;
    } else {
        return ERR_BAD_STATE;
    }
    pool_hole(&fresh);
    fresh.birth = txg;
    pool_digest(data_of(b), POOL_BLOCK_SIZE, fresh.digest);
    for (i = 0; status == OK && i < copies; i++) {
        status = allocate(i, &fresh.address[i]);
        if (status == OK)
            status = write_page(fresh.address[i], b);
    }
    if (status != OK)
        return status;
    release(&old);
    buffers[b].state = BUFFER_CLEAN;
    dirty_count--;
    if (parent != CACHE_BLOCKS) {
        pool_pointer_write(data_of(parent) +
                               index % POOL_FANOUT * POOL_POINTER_SIZE,
                           &fresh);
        return OK;
    }
    record.root = fresh;
    return object_put(object, &record);
}

/* Write every dirty block of the objects, or of the node table, level
   by level from the leaves, so each block's parent takes its pointer
   before the parent is written. */
static UINT32 write_level_by_level(int table, UINT64 txg)
{
    UINT32 level, i, status;

    for (level = 0; level < POOL_LEVELS_MAX; level++)
        for (i = 0; i < CACHE_BLOCKS; i++)
            if (buffers[i].state == BUFFER_DIRTY &&
                buffers[i].level == level &&
                (buffers[i].object == OBJECT_TABLE) == table) {
                status = write_dirty(i, txg);
                if (status != OK)
                    return status;
            }
    return OK;
}

/* The superblock of txg in slot txg % POOL_RING_SLOTS of both rings. */
static UINT32 write_superblocks(UINT64 txg, const POOL_POINTER *root)
{
    UINT32 status;
    UINT64 slot = txg % POOL_RING_SLOTS;

    pool_superblock_write(page(IO_PAGE), pool_blocks, txg, root);
    status = write_page(slot, IO_PAGE);
    if (status == OK)
        status = write_page(pool_blocks - POOL_RING_SLOTS + slot, IO_PAGE);
    return status;
}

/* F6's commit: the new blocks, a flush, the superblocks, a flush. Then
   the blocks the commit released are free. */
static UINT32 commit(void)
{
    POOL_POINTER root;
    UINT64 txg = committed_txg + 1, n;
    UINT32 i, status;

    if (!dirty_count && !root_dirty)
        return OK;
    status = write_level_by_level(0, txg);
    if (status == OK)
        status = write_level_by_level(1, txg);
    if (status == OK && dirty_count)
        status = ERR_BAD_STATE;
    if (status == OK) {
        release(&committed_root);
        volume.txg = txg;
        pool_root_write(page(IO_PAGE), &volume);
        pool_hole(&root);
        root.birth = txg;
        pool_digest(page(IO_PAGE), POOL_BLOCK_SIZE, root.digest);
        for (i = 0; status == OK && i < POOL_COPIES; i++) {
            status = allocate(i, &root.address[i]);
            if (status == OK)
                status = write_page(root.address[i], IO_PAGE);
        }
    }
    if (status == OK)
        status = block_call(BLOCK_FLUSH, 0, 0, 0, 0);
    if (status == OK)
        status = write_superblocks(txg, &root);
    if (status == OK)
        status = block_call(BLOCK_FLUSH, 0, 0, 0, 0);
    if (status != OK)
        stop("a commit failed", status);
    for (n = 0; freeing_blocks && n < pool_blocks; n++)
        if (bit(freeing_map, n)) {
            clear_bit(freeing_map, n);
            clear_bit(used_map, n);
            freeing_blocks--;
            used_blocks--;
        }
    committed_txg = txg;
    committed_root = root;
    root_dirty = 0;
    return OK;
}

/* ------------------------------------------------------------------------
   Opening the pool. */

/* Note block as in use by the newest commit; a block outside the data
   area, or one two pointers name, is a broken tree. */
static UINT32 mark(UINT64 block)
{
    if (!in_data_area(block) || bit(used_map, block))
        return ERR_DATA_INTEGRITY;
    set_bit(used_map, block);
    used_blocks++;
    return OK;
}

/* Mark the tree under pointer, a block at level whose leaves have
   leaf_copies copies, reading each indirect block into page n. */
static UINT32 mark_tree(const POOL_POINTER *pointer, UINT32 level,
                        UINT32 leaf_copies, UINT32 n)
{
    POOL_POINTER child;
    UINT32 copies = level ? POOL_COPIES : leaf_copies, i, k, status;

    if (pool_is_hole(pointer))
        return OK;
    if ((pointer->address[1] != 0) != (copies == POOL_COPIES) ||
        pointer->birth > committed_txg)
        return ERR_DATA_INTEGRITY;
    for (i = 0; i < copies; i++) {
        status = mark(pointer->address[i]);
        if (status != OK)
            return status;
    }
    if (!level)
        return OK;
    status = read_checked(pointer, n);
    for (k = 0; status == OK && k < POOL_FANOUT; k++) {
        pool_pointer_read(page(n) + k * POOL_POINTER_SIZE, &child);
        status = mark_tree(&child, level - 1, leaf_copies, n + 1);
    }
    return status;
}

/* A record is free with nothing under it, or a directory or file with
   at most POOL_LEVELS_MAX levels, nothing under it at 0, and a size an
   object may have; a size past what its levels hold is a hole there. */
static int record_valid(const POOL_RECORD *record)
{
    if (record->levels > POOL_LEVELS_MAX ||
        (record->levels == 0 && !pool_is_hole(&record->root)))
        return 0;
    if (record->kind == POOL_KIND_FREE)
        return record->levels == 0;
    return (record->kind == POOL_KIND_FILE && record->size <= FILE_SIZE_MAX) ||
           (record->kind == POOL_KIND_DIRECTORY &&
            record->size <= DIRECTORY_MAX);
}

/* The newest commit whose superblock is whole in either ring, and its
   root block. */
static UINT32 open_newest(void)
{
    POOL_POINTER root, best_root;
    UINT64 txg, best = 0, slot;
    UINT32 ring, status;

    for (ring = 0; ring < 2; ring++)
        for (slot = 0; slot < POOL_RING_SLOTS; slot++) {
            status = read_page(ring ? pool_blocks - POOL_RING_SLOTS + slot
                                    : slot,
                               IO_PAGE);
            if (status == OK &&
                pool_superblock_read(page(IO_PAGE), pool_blocks, &txg,
                                     &root) &&
                txg % POOL_RING_SLOTS == slot && txg > best) {
                best = txg;
                best_root = root;
            }
        }
    if (!best)
        return ERR_NOT_FOUND;
    committed_txg = best;
    committed_root = best_root;
    status = read_checked(&best_root, IO_PAGE);
    if (status != OK)
        return status;
    pool_root_read(page(IO_PAGE), &volume);
    if (volume.txg != best || best_root.birth != best ||
        !best_root.address[1] || volume.slots % POOL_RECORDS_PER_BLOCK ||
        volume.slots == 0 || volume.slots > NODES_MAX ||
        volume.levels == 0 || volume.levels > POOL_LEVELS_MAX ||
        volume.slots / POOL_RECORDS_PER_BLOCK > span(volume.levels - 1))
        return ERR_DATA_INTEGRITY;
    status = mark(best_root.address[0]);
    if (status == OK)
        status = mark(best_root.address[1]);
    return status;
}

/* Open the pool from its newest commit: every block that commit's tree
   names, every indirect block and node table block read and checked on
   the way, and every node's record. */
static UINT32 open_pool(void)
{
    POOL_RECORD record;
    UINT32 slot, b, status;

    status = open_newest();
    if (status == OK)
        status = mark_tree(&volume.table, volume.levels - 1, POOL_COPIES,
                           WALK_PAGE);
    for (slot = 0; status == OK && slot < volume.slots; slot++) {
        status = get_buffer(OBJECT_TABLE, 0, slot / POOL_RECORDS_PER_BLOCK,
                            FILL_READ, &b);
        if (status != OK)
            break;
        pool_record_read(data_of(b) + slot % POOL_RECORDS_PER_BLOCK *
                                          POOL_RECORD_SIZE,
                         &record);
        if (!record_valid(&record) ||
            (slot == POOL_ROOT_SLOT && record.kind != POOL_KIND_DIRECTORY))
            status = ERR_DATA_INTEGRITY;
        else if (record.kind != POOL_KIND_FREE) {
            set_bit(node_map, slot);
            if (record.levels)
                status = mark_tree(&record.root, record.levels - 1,
                                   record.kind == POOL_KIND_FILE
                                       ? 1
                                       : POOL_COPIES,
                                   WALK_PAGE);
        }
    }
    cursor[0] = POOL_RING_SLOTS;
    cursor[1] = pool_middle;
    return status;
}

/* Whether header, read from LBA 1, is a GPT header: its signature, its
   size, its CRC and its own LBA (UEFI 2.11 5.3.2). */
static int gpt_header_valid(const UINT8 *header)
{
    static const UINT8 zero[GPT_HEADER_CRC_SIZE];
    UINT32 size = pool_get32(header + GPT_HEADER_SIZE_FIELD), crc, i;

    for (i = 0; i < GPT_SIGNATURE_SIZE; i++)
        if (header[i] != (UINT8)GPT_SIGNATURE[i])
            return 0;
    if (size < GPT_HEADER_MIN || size > POOL_SECTOR_SIZE ||
        pool_get64(header + GPT_MY_LBA) != GPT_HEADER_LBA)
        return 0;
    crc = pool_crc32(0, header, GPT_HEADER_CRC);
    crc = pool_crc32(crc, zero, GPT_HEADER_CRC_SIZE);
    crc = pool_crc32(crc, header + GPT_HEADER_CRC + GPT_HEADER_CRC_SIZE,
                     size - GPT_HEADER_CRC - GPT_HEADER_CRC_SIZE);
    return crc == pool_get32(header + GPT_HEADER_CRC);
}

/* Whether the type GUID at entry is all zero: an unused entry (UEFI
   2.11 5.3.3). */
static int gpt_entry_unused(const UINT8 *entry)
{
    UINT32 i;

    for (i = 0; i < POOL_GUID_SIZE; i++)
        if (entry[i])
            return 0;
    return 1;
}

/* The used entries' extents, read in one pass with the array's CRC. */
static UINT64 used_first[GPT_ENTRIES_MAX], used_last[GPT_ENTRIES_MAX];

/* The pool partition, by its type GUID in the disk's primary GPT, whose
   header and entry array must pass their CRCs, and which must lie within
   the header's usable LBAs and overlap no other used entry (UEFI 2.11
   5.3.1); a disk that fails any of these is refused, so the pool's
   rings never overwrite the table or another partition. The backup
   table at the disk's end, which 5.3.2 has software read when the
   primary fails, is not read. */
static UINT32 find_partition(UINT64 capacity)
{
    static const UINT8 type[POOL_GUID_SIZE] = POOL_PARTITION_TYPE;
    const UINT8 *entry;
    UINT64 entries, first_usable, last_usable, first = 0, last = 0;
    UINT32 count, size, per_sector, expected, crc = 0, i, j, status;
    UINT32 used = 0, pool = 0;
    int found = 0;

    status = block_call(BLOCK_READ, GPT_HEADER_LBA, POOL_SECTOR_SIZE,
                        IO_PAGE * PAGE_SIZE, 0);
    if (status != OK)
        return status;
    if (!gpt_header_valid(page(IO_PAGE))) {
        say("nova-fs: the disk's GPT header fails its check\r\n");
        return ERR_DATA_INTEGRITY;
    }
    entries = pool_get64(page(IO_PAGE) + GPT_ENTRIES_LBA);
    count = pool_get32(page(IO_PAGE) + GPT_ENTRY_COUNT);
    size = pool_get32(page(IO_PAGE) + GPT_ENTRY_SIZE);
    expected = pool_get32(page(IO_PAGE) + GPT_ENTRIES_CRC);
    first_usable = pool_get64(page(IO_PAGE) + GPT_FIRST_USABLE);
    last_usable = pool_get64(page(IO_PAGE) + GPT_LAST_USABLE);
    if (size < GPT_ENTRY_MIN || size > POOL_SECTOR_SIZE ||
        POOL_SECTOR_SIZE % size || count > GPT_ENTRIES_MAX ||
        entries >= capacity ||
        ((UINT64)count * size + POOL_SECTOR_SIZE - 1) / POOL_SECTOR_SIZE >
            capacity - entries)
        return ERR_NOT_SUPPORTED;
    per_sector = POOL_SECTOR_SIZE / size;
    for (i = 0; i < count; i++) {
        if (i % per_sector == 0) {
            status = block_call(BLOCK_READ, entries + i / per_sector,
                                POOL_SECTOR_SIZE, IO_PAGE * PAGE_SIZE, 0);
            if (status != OK)
                return status;
        }
        entry = page(IO_PAGE) + i % per_sector * size;
        crc = pool_crc32(crc, entry, size);
        if (gpt_entry_unused(entry))
            continue;
        used_first[used] = pool_get64(entry + GPT_ENTRY_FIRST);
        used_last[used] = pool_get64(entry + GPT_ENTRY_LAST);
        for (j = 0; j < POOL_GUID_SIZE && entry[j] == type[j]; j++)
            ;
        if (j == POOL_GUID_SIZE && !found) {
            found = 1;
            pool = used;
            first = used_first[used];
            last = used_last[used];
        }
        used++;
    }
    if (crc != expected) {
        say("nova-fs: the disk's GPT entry array fails its check\r\n");
        return ERR_DATA_INTEGRITY;
    }
    if (!found)
        return ERR_NOT_FOUND;
    for (i = 0; i < used; i++)
        if (i != pool && used_first[i] <= last && first <= used_last[i])
            break;
    if (first < first_usable || first > last || last > last_usable ||
        last_usable >= capacity || i < used) {
        say("nova-fs: the disk's pool entry lies outside the usable LBAs or "
            "overlaps another\r\n");
        return ERR_DATA_INTEGRITY;
    }
    pool_first_lba = first;
    pool_blocks = (last - first + 1) / POOL_SECTORS_PER_BLOCK;
    pool_middle = pool_blocks / 2;
    if (pool_blocks < POOL_BLOCKS_MIN || pool_blocks > POOL_BLOCKS_MAX)
        return ERR_NOT_SUPPORTED;
    return OK;
}

/* ------------------------------------------------------------------------
   Changes. */

/* Ready the cache for a change: commit first if the dirty blocks have
   passed DIRTY_LIMIT, and refuse a change the pool has no room to
   commit; a remove may take the reserve. */
static UINT32 begin_change(int removes)
{
    UINT64 reserve = removes ? 0 : POOL_RESERVE_BLOCKS;
    UINT32 status = OK;

    if (dirty_count > DIRTY_LIMIT)
        status = commit();
    if (status == OK &&
        free_blocks() < commit_blocks() + 2 * CHANGE_DIRTY_MAX + reserve)
        status = ERR_NO_RESOURCES;
    return status;
}

/* ------------------------------------------------------------------------
   Channels and replies. */

static void close_received(void)
{
    UINT32 i;

    for (i = 0; i < received_count; i++)
        handle_close(received[i].handle);
    received_count = 0;
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

static UINT32 node_attributes(const POOL_RECORD *record, UINT32 rights)
{
    IO_ATTRIBUTES *a = (IO_ATTRIBUTES *)(reply + sizeof(IO_REPLY));

    a->kind = record->kind;
    a->flags = record->flags & POOL_FLAG_EXECUTABLE ? IO_EXECUTABLE : 0;
    a->size = record->kind == POOL_KIND_FILE ? record->size : 0;
    pool_zero(a->digest, IO_DIGEST_SIZE);
    a->rights = rights;
    a->remaining = 0;
    return sizeof *a;
}

/* The client an open through c joins, if it has room for one more
   channel. */
static UINT32 client_for(const CHANNEL *c, UINT32 *client)
{
    UINT32 i;

    if (c->opens == OPENS_JOIN) {
        *client = c->client;
        return client_channels[c->client] < SERVICE_CLIENT_CHANNELS
                   ? OK
                   : ERR_NO_RESOURCES;
    }
    for (i = 0; i < SERVICE_CLIENTS; i++)
        if (client_channels[i] == 0) {
            *client = i;
            return OK;
        }
    return ERR_NO_RESOURCES;
}

/* Serve handle as a channel of client. The channels open are the sum of
   the clients', so they never pass CHANNELS_MAX. */
static void add_channel(UINT32 handle, UINT32 client, UINT32 rights,
                        UINT32 slot, UINT32 generation, UINT32 kind,
                        OPENS opens)
{
    CHANNEL *c = &channels[channel_count++];

    client_channels[client]++;
    c->handle = handle;
    c->client = client;
    c->rights = rights;
    c->slot = slot;
    c->generation = generation;
    c->kind = kind;
    c->opens = opens;
}

static void remove_channel(UINT32 index)
{
    handle_close(channels[index].handle);
    client_channels[channels[index].client]--;
    channels[index] = channels[--channel_count];
}

/* ------------------------------------------------------------------------
   Requests. */

/* An open's node, which with IO_OPEN_CREATE is created when missing. */
static UINT32 open_node(const CHANNEL *c, const UINT8 *path, UINT32 length,
                        UINT32 flags, UINT32 *slot, UINT32 *generation)
{
    POOL_RECORD record;
    ENTRY entry;
    UINT32 kind = flags & IO_OPEN_DIRECTORY ? POOL_KIND_DIRECTORY
                                            : POOL_KIND_FILE;
    UINT32 parent, name, found_kind, status;

    if (!(flags & IO_OPEN_CREATE) || length == 0) {
        status = resolve(c->slot, path, length, slot, &found_kind);
        if (status == OK && (flags & IO_OPEN_CREATE) &&
            ((flags & IO_OPEN_EXCLUSIVE) || found_kind != kind))
            status = ERR_ALREADY_EXISTS;
    } else {
        status = resolve_parent(c->slot, path, length, &parent, &name);
        if (status == OK)
            status = directory_load(parent);
        if (status == OK &&
            directory_find(path + name, length - name, &entry)) {
            *slot = entry.slot;
            if ((flags & IO_OPEN_EXCLUSIVE) || entry.kind != kind)
                status = ERR_ALREADY_EXISTS;
        } else if (status == OK) {
            if (!node_room() || !directory_room(length - name, 0))
                status = ERR_NO_RESOURCES;
            if (status == OK)
                status = begin_change(0);
            if (status == OK) {
                changed(node_create(kind, slot, generation));
                changed(directory_load(parent));
                directory_find(path + name, length - name, &entry);
                changed(directory_insert(parent, entry.at, path + name,
                                         length - name, *slot, kind));
            }
        }
    }
    if (status == OK)
        status = record_get(*slot, &record);
    if (status == OK)
        *generation = record.generation;
    return status;
}

/* Answer an open of the length bytes at path below directory c on
   endpoint, and serve the endpoint as what it opened. */
static void open_path(const CHANNEL *c, const IO_REQUEST *request,
                      const UINT8 *path, UINT32 endpoint)
{
    POOL_RECORD record;
    UINT32 flags = request->flags, client, slot, generation;
    UINT32 status = OK;

    if ((flags & ~IO_OPEN_FLAGS) || request->reserved ||
        ((flags & (IO_OPEN_EXCLUSIVE | IO_OPEN_DIRECTORY)) &&
         !(flags & IO_OPEN_CREATE)))
        status = ERR_INVALID_ARGS;
    else if (c->kind != POOL_KIND_DIRECTORY)
        status = ERR_NOT_SUPPORTED;
    else if ((request->rights & ~c->rights) ||
             ((flags & IO_OPEN_CREATE) && !(c->rights & IO_RIGHT_WRITE)))
        status = ERR_ACCESS_DENIED;
    else if (!path_valid(path, request->length))
        status = ERR_INVALID_ARGS;
    if (status == OK)
        status = node_get(c->slot, c->generation, &record);
    if (status == OK)
        status = client_for(c, &client);
    if (status == OK)
        status = open_node(c, path, request->length, flags, &slot,
                           &generation);
    if (status == OK)
        status = record_get(slot, &record);
    if (status != OK) {
        send(endpoint, request->transaction, status, 0, 0);
        handle_close(endpoint);
        return;
    }
    add_channel(endpoint, client, request->rights, slot, generation,
                record.kind, OPENS_JOIN);
    send(endpoint, request->transaction, OK, 0,
         node_attributes(&record, request->rights));
}

/* Entries of the directory from the cookie'th on, up to count, as many
   as the reply holds; the cookie to go on from. */
static UINT32 read_dir(const CHANNEL *c, UINT64 cookie, UINT32 count,
                       UINT64 *next, UINT32 *at)
{
    IO_DIRECTORY_ENTRY *out;
    ENTRY entry;
    UINT32 offset, added = 0, status = directory_load(c->slot);
    UINT64 ordinal = 0;

    for (offset = 0; status == OK && offset < directory_size && added < count;
         offset += entry.bytes, ordinal++) {
        entry_at(offset, &entry);
        if (ordinal < cookie)
            continue;
        if (sizeof reply - *at < sizeof *out + entry.length)
            break;
        out = (IO_DIRECTORY_ENTRY *)(reply + *at);
        out->length = (UINT16)entry.length;
        out->kind = (UINT8)entry.kind;
        out->reserved = 0;
        copy(reply + *at + sizeof *out, entry.name, entry.length);
        *at += sizeof *out + entry.length;
        added++;
    }
    *next = ordinal;
    return status;
}

static UINT32 read_file(const CHANNEL *c, UINT64 offset, UINT32 length,
                        UINT64 *count)
{
    POOL_RECORD record;
    UINT32 status = node_get(c->slot, c->generation, &record);

    *count = 0;
    if (status != OK)
        return status;
    if (length > IO_TRANSFER_MAX)
        length = IO_TRANSFER_MAX;
    if (offset < record.size)
        *count = record.size - offset < length ? record.size - offset : length;
    status = object_read(c->slot, offset, reply + sizeof(IO_REPLY), *count);
    if (status == ERR_DATA_INTEGRITY)
        say("nova-fs: a read fails its integrity check\r\n");
    if (status != OK)
        *count = 0;
    return status;
}

static UINT32 write_file(const CHANNEL *c, UINT64 offset, const UINT8 *data,
                         UINT32 length)
{
    POOL_RECORD record;
    UINT32 status = node_get(c->slot, c->generation, &record);

    if (status != OK || length == 0)
        return status;
    if (length > IO_TRANSFER_MAX || offset > FILE_SIZE_MAX - length)
        return ERR_OUT_OF_RANGE;
    status = begin_change(0);
    return status == OK ? object_write(c->slot, offset, data, length)
                        : status;
}

static UINT32 truncate_file(const CHANNEL *c, UINT64 size)
{
    POOL_RECORD record;
    UINT32 status = node_get(c->slot, c->generation, &record);

    if (status != OK)
        return status;
    if (size > FILE_SIZE_MAX)
        return ERR_OUT_OF_RANGE;
    status = begin_change(size < record.size);
    return status == OK ? object_truncate(c->slot, size) : status;
}

static UINT32 remove_path(const CHANNEL *c, const UINT8 *path, UINT32 length)
{
    POOL_RECORD record;
    ENTRY entry;
    UINT32 parent, name, status;

    if (length == 0 || !path_valid(path, length))
        return ERR_INVALID_ARGS;
    status = resolve_parent(c->slot, path, length, &parent, &name);
    if (status == OK)
        status = directory_load(parent);
    if (status == OK && !directory_find(path + name, length - name, &entry))
        status = ERR_NOT_FOUND;
    if (status == OK)
        status = record_get(entry.slot, &record);
    if (status == OK && record.kind == POOL_KIND_DIRECTORY && record.size)
        status = ERR_BAD_STATE;
    if (status == OK)
        status = begin_change(1);
    if (status != OK)
        return status;
    changed(directory_load(parent));
    changed(directory_find(path + name, length - name, &entry)
                ? directory_delete(parent, &entry)
                : ERR_BAD_STATE);
    changed(node_remove(entry.slot));
    return OK;
}

/* Whether the length bytes at below lie inside the path above: above,
   then '/'. */
static int inside(const UINT8 *above, UINT32 above_length,
                  const UINT8 *below, UINT32 length)
{
    return length > above_length && below[above_length] == '/' &&
           compare(above, above_length, below, above_length) == 0;
}

/* The slot and kind of the node a rename moves, and of the one it
   replaces, if any, and that the destination's directory has room for
   a new entry: checked before anything changes. */
static UINT32 rename_check(const CHANNEL *c, const IO_REQUEST *request,
                           const UINT8 *from, UINT32 from_length,
                           const UINT8 *to, UINT32 to_length, UINT32 *slot,
                           UINT32 *kind, UINT32 *replaced)
{
    POOL_RECORD record;
    ENTRY entry;
    UINT32 parent, from_parent, name, status, freed;

    *replaced = NO_SLOT;
    status = resolve_parent(c->slot, from, from_length, &from_parent, &name);
    parent = from_parent;
    if (status == OK)
        status = directory_load(parent);
    if (status == OK &&
        !directory_find(from + name, from_length - name, &entry))
        status = ERR_NOT_FOUND;
    if (status != OK)
        return status;
    *slot = entry.slot;
    *kind = entry.kind;
    freed = entry.bytes;
    status = resolve_parent(c->slot, to, to_length, &parent, &name);
    if (status == OK)
        status = directory_load(parent);
    if (status != OK)
        return status;
    if (!directory_find(to + name, to_length - name, &entry))
        return directory_room(to_length - name,
                              parent == from_parent ? freed : 0)
                   ? OK
                   : ERR_NO_RESOURCES;
    status = record_get(entry.slot, &record);
    if (status == OK && (!(request->flags & IO_RENAME_REPLACE) ||
                         entry.kind != *kind ||
                         (*kind == POOL_KIND_DIRECTORY && record.size)))
        status = ERR_ALREADY_EXISTS;
    *replaced = entry.slot;
    return status;
}

static UINT32 rename_path(const CHANNEL *c, const IO_REQUEST *request,
                          const UINT8 *data)
{
    ENTRY entry;
    const UINT8 *from = data, *to;
    UINT32 from_length, to_length, from_parent, from_name, to_parent;
    UINT32 to_name, slot, kind, replaced, status;

    if (request->offset > request->length ||
        (request->flags & ~IO_RENAME_REPLACE))
        return ERR_INVALID_ARGS;
    from_length = (UINT32)request->offset;
    to = data + from_length;
    to_length = request->length - from_length;
    if (!from_length || !to_length || !path_valid(from, from_length) ||
        !path_valid(to, to_length) || inside(from, from_length, to, to_length))
        return ERR_INVALID_ARGS;
    if (from_length == to_length &&
        compare(from, from_length, to, to_length) == 0)
        return resolve(c->slot, from, from_length, &slot, &kind);
    status = rename_check(c, request, from, from_length, to, to_length, &slot,
                          &kind, &replaced);
    if (status == OK)
        status = resolve_parent(c->slot, from, from_length, &from_parent,
                                &from_name);
    if (status == OK)
        status = resolve_parent(c->slot, to, to_length, &to_parent, &to_name);
    if (status == OK)
        status = begin_change(0);
    if (status != OK)
        return status;
    changed(directory_load(from_parent));
    changed(directory_find(from + from_name, from_length - from_name, &entry)
                ? directory_delete(from_parent, &entry)
                : ERR_BAD_STATE);
    changed(directory_load(to_parent));
    if (directory_find(to + to_name, to_length - to_name, &entry)) {
        pool_put32(directory + entry.at + POOL_ENTRY_SLOT, slot);
        changed(object_write(to_parent, entry.at + POOL_ENTRY_SLOT,
                             directory + entry.at + POOL_ENTRY_SLOT,
                             sizeof(UINT32)));
    } else {
        changed(directory_insert(to_parent, entry.at, to + to_name,
                                 to_length - to_name, slot, kind));
    }
    if (replaced != NO_SLOT)
        changed(node_remove(replaced));
    return OK;
}

/* The endpoint an open carries, as its first and only handle, or
   HANDLE_INVALID. */
static UINT32 endpoint_of(void)
{
    if (received_count == 1 && received[0].type == TYPE_CHANNEL &&
        (received[0].rights & IO_ENDPOINT_RIGHTS) == IO_ENDPOINT_RIGHTS)
        return received[0].handle;
    return HANDLE_INVALID;
}

/* Whether operation carries its bytes after the request, as length. */
static int carries_bytes(UINT32 operation)
{
    return operation == IO_OPEN || operation == IO_WRITE ||
           operation == IO_REMOVE || operation == IO_RENAME;
}

/* Serve the request in message, of bytes bytes, on channel index.
   Returns 0 if the channel ends: a message too short to be a request,
   which is answered by closing the channel. */
static int serve_request(UINT32 index, UINT32 bytes)
{
    const IO_REQUEST *request = (const IO_REQUEST *)message;
    const UINT8 *data = message + sizeof(IO_REQUEST);
    CHANNEL c = channels[index];
    POOL_RECORD record;
    UINT64 information = 0;
    UINT32 status, endpoint, at = sizeof(IO_REPLY), size = 0;
    UINT32 needs = 0, kind = 0;

    if (bytes < sizeof(IO_REQUEST)) {
        close_received();
        return 0;
    }
    if (carries_bytes(request->operation)
            ? request->length != bytes - sizeof(IO_REQUEST)
            : bytes != sizeof(IO_REQUEST)) {
        close_received();
        send(c.handle, request->transaction, ERR_INVALID_ARGS, 0, 0);
        return 1;
    }
    if (request->operation == IO_OPEN) {
        endpoint = endpoint_of();
        if (endpoint == HANDLE_INVALID) {
            close_received();
            send(c.handle, request->transaction, ERR_INVALID_ARGS, 0, 0);
            return 1;
        }
        received_count = 0;
        open_path(&c, request, data, endpoint);
        return 1;
    }
    close_received();
    switch (request->operation) {
    case IO_READ_DIR:
        needs = IO_RIGHT_LIST;
        kind = POOL_KIND_DIRECTORY;
        break;
    case IO_READ:
        needs = IO_RIGHT_READ;
        kind = POOL_KIND_FILE;
        break;
    case IO_WRITE:
    case IO_TRUNCATE:
        needs = IO_RIGHT_WRITE;
        kind = POOL_KIND_FILE;
        break;
    case IO_REMOVE:
    case IO_RENAME:
        needs = IO_RIGHT_WRITE;
        kind = POOL_KIND_DIRECTORY;
        break;
    case IO_FLUSH:
        needs = IO_RIGHT_WRITE;
        break;
    case IO_GET_ATTRIBUTES:
        break;
    default:
        send(c.handle, request->transaction, ERR_NOT_SUPPORTED, 0, 0);
        return 1;
    }
    if ((request->flags && request->operation != IO_RENAME) ||
        request->reserved || request->rights)
        status = ERR_INVALID_ARGS;
    else if (kind && c.kind != kind)
        status = ERR_NOT_SUPPORTED;
    else if ((c.rights & needs) != needs)
        status = ERR_ACCESS_DENIED;
    else
        status = node_get(c.slot, c.generation, &record);
    if (status == OK) {
        switch (request->operation) {
        case IO_READ_DIR:
            status = read_dir(&c, request->offset, request->length,
                              &information, &at);
            size = status == OK ? at - (UINT32)sizeof(IO_REPLY) : 0;
            break;
        case IO_READ:
            status = read_file(&c, request->offset, request->length,
                               &information);
            size = (UINT32)information;
            break;
        case IO_WRITE:
            status = write_file(&c, request->offset, data, request->length);
            information = status == OK ? request->length : 0;
            break;
        case IO_TRUNCATE:
            status = request->length ? ERR_INVALID_ARGS
                                     : truncate_file(&c, request->offset);
            break;
        case IO_REMOVE:
            status = remove_path(&c, data, request->length);
            break;
        case IO_RENAME:
            status = rename_path(&c, request, data);
            break;
        case IO_FLUSH:
            status = request->length || request->offset ? ERR_INVALID_ARGS
                                                          : commit();
            break;
        case IO_GET_ATTRIBUTES:
            size = node_attributes(&record, c.rights);
            break;
        }
    }
    send(c.handle, request->transaction, status, status == OK ? information : 0,
         status == OK ? size : 0);
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

/* Map the shared section and attach it to the driver, which pins it. */
static void attach(UINT64 *capacity)
{
    HANDLE_TRANSFER transfer;
    BLOCK_REQUEST request;
    BLOCK_REPLY answer;
    UINT32 section, status, observed, bytes, count;

    status = section_create(SHARED_SIZE, 0, &section);
    if (status == OK)
        status = process_map(self, section, 0, SHARED_BASE, SHARED_SIZE,
                             MAP_READ | MAP_WRITE);
    if (status == OK)
        status = handle_duplicate(section, BLOCK_SECTION_RIGHTS,
                                  &transfer.handle);
    if (status != OK)
        stop("cannot make the shared section", status);
    handle_close(section);
    shared = (UINT8 *)SHARED_BASE;
    transfer.rights = RIGHTS_SAME;
    request.transaction = 0;
    request.operation = BLOCK_ATTACH;
    request.sector = 0;
    request.length = SHARED_SIZE;
    request.buffer = 0;
    status = channel_write(block_channel, &request, sizeof request, &transfer,
                           1);
    if (status == OK)
        status = object_wait_one(block_channel,
                                 CHANNEL_READABLE | CHANNEL_PEER_CLOSED,
                                 DEADLINE_INFINITE, &observed);
    if (status == OK)
        status = channel_read(block_channel, &answer, sizeof answer, received,
                              0, &bytes, &count);
    if (status == OK && (bytes != sizeof answer || count))
        status = ERR_BAD_STATE;
    if (status == OK)
        status = answer.status;
    if (status != OK)
        stop("the block driver did not take the shared section", status);
    *capacity = answer.information;
}

static void read_bootstrap(UINT32 bootstrap, UINT32 *root)
{
    RECEIVED_HANDLE handles[FS_BOOTSTRAP_HANDLES];
    UINT32 bytes, count;

    if (channel_read(bootstrap, message, sizeof message, handles,
                     FS_BOOTSTRAP_HANDLES, &bytes, &count) != OK ||
        count != FS_BOOTSTRAP_HANDLES || bytes != 0 ||
        handles[FS_BOOTSTRAP_DEBUG_LOG].type != TYPE_RESOURCE ||
        handles[FS_BOOTSTRAP_SELF].type != TYPE_PROCESS ||
        handles[FS_BOOTSTRAP_BLOCK].type != TYPE_CHANNEL ||
        handles[FS_BOOTSTRAP_ROOT].type != TYPE_CHANNEL)
        process_exit(SERVICE_EXIT_FAILED);
    handle_close(bootstrap);
    say_open(handles[FS_BOOTSTRAP_DEBUG_LOG].handle);
    self = handles[FS_BOOTSTRAP_SELF].handle;
    block_channel = handles[FS_BOOTSTRAP_BLOCK].handle;
    *root = handles[FS_BOOTSTRAP_ROOT].handle;
}

static void say_pool(void)
{
    say("nova-fs: pool at lba ");
    say_number(pool_first_lba);
    say(", ");
    say_number(pool_blocks);
    say(" blocks of ");
    say_number(POOL_BLOCK_SIZE);
    say(" bytes; newest commit ");
    say_number(committed_txg);
    say(", ");
    say_number(volume.slots);
    say(" node slots, ");
    say_number(used_blocks);
    say(" blocks in use\r\n");
}

/* As the store service's loop (store/store.c): channels served from the
   last, so that removing or adding one leaves each index below it naming
   the channel it was waited on as. */
_Noreturn void fs_main(UINT32 bootstrap, UINT64 argument)
{
    WAIT_ENTRY items[CHANNELS_MAX];
    POOL_RECORD record;
    UINT64 capacity;
    UINT32 root, waited, i, observed, status;
    int keep;

    (void)argument;
    read_bootstrap(bootstrap, &root);
    attach(&capacity);
    status = find_partition(capacity);
    if (status != OK)
        stop("the disk holds no pool partition this service opens", status);
    status = open_pool();
    if (status == OK)
        status = record_get(POOL_ROOT_SLOT, &record);
    if (status != OK)
        stop("the pool does not open", status);
    say_pool();
    add_channel(root, 0, VOLUME_RIGHTS, POOL_ROOT_SLOT, record.generation,
                POOL_KIND_DIRECTORY, OPENS_START_CLIENTS);
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
    commit();
    say("nova-fs: no channel left open, committed, ending\r\n");
    process_exit(SERVICE_EXIT_DONE);
}
