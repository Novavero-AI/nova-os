/* The store image reader (reader.h). Every offset and length the image
   gives is checked against the bounds it must lie in before it is used,
   so a malformed image fails a check instead of sending a read outside
   it. */

#include "reader.h"

#define LENGTH_FIELD 8

static UINT32 read32(const UINT8 *p)
{
    return (UINT32)p[0] | (UINT32)p[1] << 8 | (UINT32)p[2] << 16 |
           (UINT32)p[3] << 24;
}

static UINT64 read64(const UINT8 *p)
{
    return read32(p) | (UINT64)read32(p + 4) << 32;
}

static void write32(UINT8 *p, UINT32 v)
{
    p[0] = (UINT8)v;
    p[1] = (UINT8)(v >> 8);
    p[2] = (UINT8)(v >> 16);
    p[3] = (UINT8)(v >> 24);
}

static void write64(UINT8 *p, UINT64 v)
{
    write32(p, (UINT32)v);
    write32(p + 4, (UINT32)(v >> 32));
}

/* Whether length bytes at offset lie within limit bytes, without the sum
   overflowing. */
static int within(UINT64 offset, UINT64 length, UINT64 limit)
{
    return offset <= limit && length <= limit - offset;
}

static int compare(const UINT8 *a, UINT64 a_length, const UINT8 *b,
                   UINT64 b_length)
{
    UINT64 i, n = a_length < b_length ? a_length : b_length;

    for (i = 0; i < n; i++)
        if (a[i] != b[i])
            return a[i] < b[i] ? -1 : 1;
    return (a_length > b_length) - (a_length < b_length);
}

static int same(const UINT8 *a, const UINT8 *b, UINT64 length)
{
    return compare(a, length, b, length) == 0;
}

static void copy(UINT8 *to, const UINT8 *from, UINT64 count)
{
    UINT64 i;

    for (i = 0; i < count; i++)
        to[i] = from[i];
}

static void sha256(const UINT8 *data, UINT64 size,
                   UINT8 digest[SHA256_DIGEST_SIZE])
{
    SHA256_CONTEXT context;

    sha256_init(&context);
    sha256_update(&context, data, size);
    sha256_final(&context, digest);
}

/* A name a directory may hold: not empty, ".", or "..", with no '/' or
   NUL (storage.md, fact 4). */
static int entry_name(const UINT8 *name, UINT64 length, UINT64 limit)
{
    UINT64 i;

    if (length == 0 || length > limit ||
        (length == 1 && name[0] == '.') ||
        (length == 2 && name[0] == '.' && name[1] == '.'))
        return 0;
    for (i = 0; i < length; i++)
        if (name[i] == '/' || name[i] == 0)
            return 0;
    return 1;
}

/* ------------------------------------------------------------------------
   The header and the registrations. */

static int check_registration(const STORE_IMAGE *image, UINT32 index,
                              const STORE_REGISTRATION *previous)
{
    const UINT8 *entry =
        image->registrations + (UINT64)index * STORE_REGISTRATION_SIZE;
    UINT64 name = read32(entry + STORE_REGISTRATION_NAME);
    UINT64 length = read32(entry + STORE_REGISTRATION_NAME_LENGTH);
    UINT64 references = read32(entry + STORE_REGISTRATION_REFERENCES);
    UINT64 count = read32(entry + STORE_REGISTRATION_REFERENCE_COUNT);
    UINT64 i;

    if (!within(name, length, image->registrations_size) ||
        !entry_name(image->registrations + name, length,
                    STORE_BASE_NAME_MAX) ||
        (previous && compare(previous->name, previous->name_length,
                             image->registrations + name, length) >= 0) ||
        !within(references, count * STORE_REFERENCE_SIZE,
                image->registrations_size))
        return 0;
    for (i = 0; i < count; i++)
        if (read32(image->registrations + references +
                   i * STORE_REFERENCE_SIZE) >= image->registration_count)
            return 0;
    return 1;
}

UINT32 store_image_open(STORE_IMAGE *image, const UINT8 *bytes, UINT64 size)
{
    UINT8 digest[SHA256_DIGEST_SIZE];
    STORE_REGISTRATION previous;
    UINT64 registrations, nodes, nodes_size, blobs, blobs_size;
    UINT32 i;

    if (size < STORE_HEADER_SIZE ||
        !same(bytes + STORE_HEADER_MAGIC, (const UINT8 *)STORE_MAGIC,
              STORE_MAGIC_SIZE) ||
        read32(bytes + STORE_HEADER_VERSION) != STORE_VERSION)
        return ERR_INVALID_ARGS;
    image->bytes = bytes;
    image->size = size;
    image->registration_count =
        read32(bytes + STORE_HEADER_REGISTRATION_COUNT);
    image->node_count = read32(bytes + STORE_HEADER_NODE_COUNT);
    image->blob_count = read32(bytes + STORE_HEADER_BLOB_COUNT);
    registrations = read64(bytes + STORE_HEADER_REGISTRATIONS);
    image->registrations_size =
        read64(bytes + STORE_HEADER_REGISTRATIONS_SIZE);
    nodes = read64(bytes + STORE_HEADER_NODES);
    nodes_size = read64(bytes + STORE_HEADER_NODES_SIZE);
    blobs = read64(bytes + STORE_HEADER_BLOBS);
    blobs_size = read64(bytes + STORE_HEADER_BLOBS_SIZE);
    image->boot_name_length = read32(bytes + STORE_HEADER_BOOT_NAME_LENGTH);
    image->boot_name = bytes + STORE_HEADER_BOOT_NAME;
    if (read64(bytes + STORE_HEADER_IMAGE_SIZE) != size ||
        !within(registrations, image->registrations_size, size) ||
        !within(nodes, nodes_size, size) || !within(blobs, blobs_size, size) ||
        image->registrations_size / STORE_REGISTRATION_SIZE <
            image->registration_count ||
        nodes_size / STORE_NODE_ENTRY_SIZE < image->node_count ||
        blobs_size / STORE_BLOB_ENTRY_SIZE < image->blob_count ||
        image->boot_name_length > STORE_BASE_NAME_MAX)
        return ERR_DATA_INTEGRITY;
    image->registrations = bytes + registrations;
    image->nodes = bytes + nodes;
    image->nodes_size = nodes_size;
    image->blobs = bytes + blobs;
    sha256(image->registrations, image->registrations_size, digest);
    if (!same(digest, bytes + STORE_HEADER_REGISTRATIONS_DIGEST,
              SHA256_DIGEST_SIZE))
        return ERR_DATA_INTEGRITY;
    for (i = 0; i < image->registration_count; i++) {
        if (!check_registration(image, i, i ? &previous : 0))
            return ERR_DATA_INTEGRITY;
        store_registration_get(image, i, &previous);
    }
    return OK;
}

void store_registration_get(const STORE_IMAGE *image, UINT32 index,
                            STORE_REGISTRATION *registration)
{
    const UINT8 *entry =
        image->registrations + (UINT64)index * STORE_REGISTRATION_SIZE;

    registration->name =
        image->registrations + read32(entry + STORE_REGISTRATION_NAME);
    registration->name_length = read32(entry + STORE_REGISTRATION_NAME_LENGTH);
    registration->root = entry + STORE_REGISTRATION_ROOT;
    registration->references =
        image->registrations + read32(entry + STORE_REGISTRATION_REFERENCES);
    registration->reference_count =
        read32(entry + STORE_REGISTRATION_REFERENCE_COUNT);
}

UINT32 store_reference(const STORE_REGISTRATION *registration, UINT32 i)
{
    return read32(registration->references + (UINT64)i * STORE_REFERENCE_SIZE);
}

int store_registration_find(const STORE_IMAGE *image, const UINT8 *name,
                            UINT32 length, UINT32 *index)
{
    STORE_REGISTRATION registration;
    UINT32 low = 0, high = image->registration_count;
    int order;

    while (low < high) {
        UINT32 middle = low + (high - low) / 2;

        store_registration_get(image, middle, &registration);
        order = compare(name, length, registration.name,
                        registration.name_length);
        if (order == 0) {
            *index = middle;
            return 1;
        }
        if (order < 0)
            high = middle;
        else
            low = middle + 1;
    }
    return 0;
}

/* ------------------------------------------------------------------------
   Nodes. */

/* A directory's encoding: entries that fill it exactly, each name one a
   directory may hold, in strictly increasing bytewise order. */
static int check_directory(STORE_NODE *node, const UINT8 *encoding,
                           UINT64 length)
{
    const UINT8 *previous = 0, *at;
    UINT64 previous_length = 0, left, name_length, i;

    if (length < STORE_DIRECTORY_ENTRIES)
        return 0;
    node->size = read32(encoding + STORE_DIRECTORY_COUNT);
    node->body = encoding + STORE_DIRECTORY_ENTRIES;
    node->body_size = length - STORE_DIRECTORY_ENTRIES;
    at = node->body;
    left = node->body_size;
    for (i = 0; i < node->size; i++) {
        if (left < STORE_ENTRY_NAME)
            return 0;
        name_length = read32(at);
        if (!within(STORE_ENTRY_NAME, name_length + STORE_DIGEST_SIZE, left) ||
            !entry_name(at + STORE_ENTRY_NAME, name_length,
                        STORE_ENTRY_NAME_MAX) ||
            (previous && compare(previous, previous_length,
                                 at + STORE_ENTRY_NAME, name_length) >= 0))
            return 0;
        previous = at + STORE_ENTRY_NAME;
        previous_length = name_length;
        at += STORE_ENTRY_NAME + name_length + STORE_DIGEST_SIZE;
        left -= STORE_ENTRY_NAME + name_length + STORE_DIGEST_SIZE;
    }
    return left == 0;
}

static int parse_node(STORE_NODE *node, const UINT8 *encoding, UINT64 length)
{
    UINT64 i;

    if (length == 0)
        return 0;
    node->kind = encoding[STORE_NODE_KIND];
    node->executable = 0;
    node->blob = 0;
    switch (node->kind) {
    case STORE_NODE_DIRECTORY:
        return check_directory(node, encoding, length);
    case STORE_NODE_FILE:
        if (length != STORE_FILE_SIZE || encoding[STORE_FILE_EXECUTABLE] > 1)
            return 0;
        node->executable = encoding[STORE_FILE_EXECUTABLE];
        node->size = read64(encoding + STORE_FILE_LENGTH);
        node->blob = encoding + STORE_FILE_BLOB;
        node->body = 0;
        node->body_size = 0;
        return 1;
    case STORE_NODE_LINK:
        if (length < STORE_LINK_TARGET)
            return 0;
        node->size = read32(encoding + STORE_LINK_LENGTH);
        node->body = encoding + STORE_LINK_TARGET;
        node->body_size = length - STORE_LINK_TARGET;
        if (node->size == 0 || node->size > STORE_TARGET_MAX ||
            node->size != node->body_size)
            return 0;
        for (i = 0; i < node->size; i++)
            if (node->body[i] == 0)
                return 0;
        return 1;
    }
    return 0;
}

UINT32 store_node_get(const STORE_IMAGE *image, const UINT8 *digest,
                      STORE_NODE *node)
{
    UINT8 computed[SHA256_DIGEST_SIZE];
    UINT32 low = 0, high = image->node_count;

    while (low < high) {
        UINT32 middle = low + (high - low) / 2;
        const UINT8 *entry =
            image->nodes + (UINT64)middle * STORE_NODE_ENTRY_SIZE;
        int order = compare(digest, STORE_DIGEST_SIZE,
                            entry + STORE_NODE_ENTRY_DIGEST, STORE_DIGEST_SIZE);
        UINT64 offset, length;

        if (order < 0) {
            high = middle;
            continue;
        }
        if (order > 0) {
            low = middle + 1;
            continue;
        }
        offset = read32(entry + STORE_NODE_ENTRY_OFFSET);
        length = read32(entry + STORE_NODE_ENTRY_LENGTH);
        if (!within(offset, length, image->nodes_size))
            return ERR_DATA_INTEGRITY;
        sha256(image->nodes + offset, length, computed);
        if (!same(computed, digest, SHA256_DIGEST_SIZE) ||
            !parse_node(node, image->nodes + offset, length))
            return ERR_DATA_INTEGRITY;
        node->digest = entry + STORE_NODE_ENTRY_DIGEST;
        return OK;
    }
    return ERR_DATA_INTEGRITY;
}

void store_entries(const STORE_NODE *directory, STORE_ENTRIES *entries)
{
    entries->next = directory->body;
    entries->end = directory->body + directory->body_size;
}

/* store_node_get checked that the entries fill the encoding exactly. */
int store_entries_next(STORE_ENTRIES *entries, const UINT8 **name,
                       UINT32 *length, const UINT8 **digest)
{
    if (entries->next == entries->end)
        return 0;
    *length = read32(entries->next);
    *name = entries->next + STORE_ENTRY_NAME;
    *digest = *name + *length;
    entries->next = *digest + STORE_DIGEST_SIZE;
    return 1;
}

int store_entry_find(const STORE_NODE *directory, const UINT8 *name,
                     UINT32 length, const UINT8 **digest)
{
    STORE_ENTRIES entries;
    const UINT8 *entry_name_bytes;
    UINT32 entry_length;
    int order;

    store_entries(directory, &entries);
    while (store_entries_next(&entries, &entry_name_bytes, &entry_length,
                              digest)) {
        order = compare(name, length, entry_name_bytes, entry_length);
        if (order == 0)
            return 1;
        if (order < 0)
            return 0;
    }
    return 0;
}

/* ------------------------------------------------------------------------
   Blobs and their hash trees (store-image.md). */

static UINT64 blocks_of(UINT64 bytes)
{
    return bytes ? (bytes - 1) / STORE_BLOCK_SIZE + 1 : 1;
}

/* The bytes of every level of a tree over blocks blocks that holds more
   than one digest. */
static UINT64 tree_size(UINT64 blocks)
{
    UINT64 size = 0;

    while (blocks > 1) {
        size += blocks * STORE_DIGEST_SIZE;
        blocks = blocks_of(blocks * STORE_DIGEST_SIZE);
    }
    return size;
}

static void block_digest(UINT32 level, UINT64 offset, const UINT8 *bytes,
                         UINT64 length, UINT8 digest[STORE_DIGEST_SIZE])
{
    UINT8 prefix[STORE_TREE_PREFIX_SIZE];
    SHA256_CONTEXT context;

    write64(prefix, offset);
    write32(prefix + LENGTH_FIELD, level);
    write32(prefix + LENGTH_FIELD + 4, (UINT32)length);
    sha256_init(&context);
    sha256_update(&context, prefix, sizeof prefix);
    sha256_update(&context, bytes, length);
    sha256_final(&context, digest);
}

/* The digest of block index of the level of size bytes at bytes. */
static void level_block(UINT32 level, const UINT8 *bytes, UINT64 size,
                        UINT64 index, UINT8 digest[STORE_DIGEST_SIZE])
{
    UINT64 offset = index * STORE_BLOCK_SIZE;
    UINT64 length = size - offset < STORE_BLOCK_SIZE ? size - offset
                                                     : STORE_BLOCK_SIZE;

    block_digest(level, offset, bytes + offset, length, digest);
}

/* Each stored level against the one above it, up to the top digest, and
   the top and the length against the blob's digest. A blob of one block
   has no stored level: its top is its block's digest. */
static UINT32 check_tree(STORE_BLOB *blob)
{
    UINT8 digest[STORE_DIGEST_SIZE], root[LENGTH_FIELD + STORE_DIGEST_SIZE];
    const UINT8 *level = blob->tree;
    UINT64 count = blob->blocks, above, i;
    UINT32 height = 0;

    if (count == 1)
        level_block(0, blob->data, blob->length, 0, blob->top);
    while (count > 1) {
        above = blocks_of(count * STORE_DIGEST_SIZE);
        for (i = 0; i < above; i++) {
            level_block(height + 1, level, count * STORE_DIGEST_SIZE, i,
                        digest);
            if (above == 1)
                copy(blob->top, digest, STORE_DIGEST_SIZE);
            else if (!same(digest,
                           level + count * STORE_DIGEST_SIZE +
                               i * STORE_DIGEST_SIZE,
                           STORE_DIGEST_SIZE))
                return ERR_DATA_INTEGRITY;
        }
        level += count * STORE_DIGEST_SIZE;
        count = above;
        height++;
    }
    write64(root, blob->length);
    copy(root + LENGTH_FIELD, blob->top, STORE_DIGEST_SIZE);
    sha256(root, sizeof root, digest);
    return same(digest, blob->digest, STORE_DIGEST_SIZE) ? OK
                                                         : ERR_DATA_INTEGRITY;
}

UINT32 store_blob_get(const STORE_IMAGE *image, const STORE_NODE *file,
                      STORE_BLOB *blob)
{
    UINT32 low = 0, high = image->blob_count;

    while (low < high) {
        UINT32 middle = low + (high - low) / 2;
        const UINT8 *entry =
            image->blobs + (UINT64)middle * STORE_BLOB_ENTRY_SIZE;
        int order = compare(file->blob, STORE_DIGEST_SIZE,
                            entry + STORE_BLOB_DIGEST, STORE_DIGEST_SIZE);
        UINT64 tree, data;

        if (order < 0) {
            high = middle;
            continue;
        }
        if (order > 0) {
            low = middle + 1;
            continue;
        }
        blob->digest = entry + STORE_BLOB_DIGEST;
        blob->length = read64(entry + STORE_BLOB_LENGTH);
        blob->blocks = blocks_of(blob->length);
        tree = read64(entry + STORE_BLOB_TREE);
        data = read64(entry + STORE_BLOB_DATA);
        if (blob->length != file->size ||
            !within(tree, tree_size(blob->blocks), image->size) ||
            !within(data, blob->length, image->size))
            return ERR_DATA_INTEGRITY;
        blob->tree = image->bytes + tree;
        blob->data = image->bytes + data;
        return check_tree(blob);
    }
    return ERR_DATA_INTEGRITY;
}

UINT32 store_blob_read(const STORE_BLOB *blob, UINT64 offset, UINT8 *out,
                       UINT64 count)
{
    UINT8 digest[STORE_DIGEST_SIZE];
    UINT64 block, start, end;

    while (count) {
        block = offset / STORE_BLOCK_SIZE;
        level_block(0, blob->data, blob->length, block, digest);
        if (!same(digest,
                  blob->blocks == 1 ? blob->top
                                    : blob->tree + block * STORE_DIGEST_SIZE,
                  STORE_DIGEST_SIZE))
            return ERR_DATA_INTEGRITY;
        start = offset - block * STORE_BLOCK_SIZE;
        end = STORE_BLOCK_SIZE - start < count ? STORE_BLOCK_SIZE
                                                : start + count;
        copy(out, blob->data + offset, end - start);
        out += end - start;
        offset += end - start;
        count -= end - start;
    }
    return OK;
}

UINT32 store_blob_check(const STORE_BLOB *blob)
{
    UINT8 digest[STORE_DIGEST_SIZE];
    UINT64 block;

    for (block = 0; block < blob->blocks; block++) {
        level_block(0, blob->data, blob->length, block, digest);
        if (!same(digest,
                  blob->blocks == 1 ? blob->top
                                    : blob->tree + block * STORE_DIGEST_SIZE,
                  STORE_DIGEST_SIZE))
            return ERR_DATA_INTEGRITY;
    }
    return OK;
}
