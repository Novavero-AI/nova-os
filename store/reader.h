/* Reading a store image (store_image.h) in place: its registrations,
   its nodes, each checked against its digest whenever it is read, and
   its blobs, each block checked against the blob's hash tree whenever it
   is read (docs/spec/storage.md F9). The store service serves /store
   with it, and the first process finds the store service with it
   (F16). Nothing here writes the image, which lies in memory no one
   writes once it is checked, so a node or block checked stays as
   checked. */

#ifndef NOVA_STORE_READER_H
#define NOVA_STORE_READER_H

#include "native_abi.h"
#include "sha256.h"
#include "store_image.h"

typedef struct {
    const UINT8 *bytes;
    UINT64 size;
    UINT32 registration_count;
    UINT32 node_count;
    UINT32 blob_count;
    const UINT8 *registrations;
    UINT64 registrations_size;
    const UINT8 *nodes;
    UINT64 nodes_size;
    const UINT8 *blobs;
    const UINT8 *boot_name;
    UINT32 boot_name_length;
} STORE_IMAGE;

typedef struct {
    const UINT8 *name;
    UINT32 name_length;
    const UINT8 *root; /* the root node's digest */
    const UINT8 *references; /* reference_count 4-byte indices */
    UINT32 reference_count;
} STORE_REGISTRATION;

typedef struct {
    UINT32 kind;       /* STORE_NODE_DIRECTORY, _FILE or _LINK */
    UINT32 executable; /* a file's flag */
    UINT64 size;       /* a file's bytes, a link's target's, a directory's
                          entries */
    const UINT8 *digest;
    const UINT8 *blob; /* a file's blob's digest */
    const UINT8 *body; /* a directory's entries, a link's target */
    UINT64 body_size;
} STORE_NODE;

/* A directory's entries, from the one at next. */
typedef struct {
    const UINT8 *next;
    const UINT8 *end;
} STORE_ENTRIES;

typedef struct {
    const UINT8 *digest;
    UINT64 length;
    UINT64 blocks;
    const UINT8 *tree;
    const UINT8 *data;
    UINT8 top[STORE_DIGEST_SIZE]; /* the tree's single top digest */
} STORE_BLOB;

/* Check size bytes at bytes as a store image: its header, the bounds of
   its tables, and its registration table against the header's digest,
   including every name's order and every reference's index. Returns OK,
   ERR_INVALID_ARGS for an image of another format or version, or
   ERR_DATA_INTEGRITY. */
UINT32 store_image_open(STORE_IMAGE *image, const UINT8 *bytes, UINT64 size);

/* The registration of the path whose base name is the length bytes at
   name, by its index in base-name order. Returns 1 if there is one. */
int store_registration_find(const STORE_IMAGE *image, const UINT8 *name,
                            UINT32 length, UINT32 *index);

/* Registration index, below the image's count. */
void store_registration_get(const STORE_IMAGE *image, UINT32 index,
                            STORE_REGISTRATION *registration);

/* The index of a registration's reference i, below its count. */
UINT32 store_reference(const STORE_REGISTRATION *registration, UINT32 i);

/* The node whose digest is digest, read from the node table and checked
   against it and against its encoding's rules. Every digest asked for is
   one the image names, so a node the table lacks is damage too. Returns
   OK, or ERR_DATA_INTEGRITY if the table lacks it or it fails a check. */
UINT32 store_node_get(const STORE_IMAGE *image, const UINT8 *digest,
                      STORE_NODE *node);

/* A directory node's entries in bytewise order of name. */
void store_entries(const STORE_NODE *directory, STORE_ENTRIES *entries);

/* The next entry's name and node digest. Returns 0 past the last. */
int store_entries_next(STORE_ENTRIES *entries, const UINT8 **name,
                       UINT32 *length, const UINT8 **digest);

/* The digest of the entry of directory named by the length bytes at
   name. Returns 1 if there is one. */
int store_entry_find(const STORE_NODE *directory, const UINT8 *name,
                     UINT32 length, const UINT8 **digest);

/* A file node's blob, its hash tree checked against the blob's digest.
   Returns OK, or ERR_DATA_INTEGRITY if the blob is missing, out of the
   image's bounds, of another length, or fails the check. */
UINT32 store_blob_get(const STORE_IMAGE *image, const STORE_NODE *file,
                      STORE_BLOB *blob);

/* Copy count bytes from offset of blob to out, checking every block they
   touch against the blob's hash tree first. offset + count is at most
   the blob's length. Returns OK, or ERR_DATA_INTEGRITY, having copied
   the blocks before the first that fails. */
UINT32 store_blob_read(const STORE_BLOB *blob, UINT64 offset, UINT8 *out,
                       UINT64 count);

/* Check every block of blob, as reading it whole does. */
UINT32 store_blob_check(const STORE_BLOB *blob);

#endif
