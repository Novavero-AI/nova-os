/* The pool format's records as values (include/pool_format.h), shared by
   the file system service and tools/mkdata.c, so that the two writers of
   the format agree (docs/spec/storage.md F11). Freestanding: it calls
   nothing but SHA-256. */

#ifndef NOVA_POOL_H
#define NOVA_POOL_H

#include "pool_format.h"
#include "types.h"

typedef struct {
    UINT64 address[POOL_COPIES]; /* 0 where there is no copy */
    UINT64 birth;
    UINT8 digest[POOL_DIGEST_SIZE];
} POOL_POINTER;

typedef struct {
    UINT8 kind;
    UINT8 flags;
    UINT8 levels;
    UINT32 generation;
    UINT64 size;
    POOL_POINTER root;
} POOL_RECORD;

/* The root block's fields: the node table is an object like any other,
   whose record the root block holds in place of a node table slot. */
typedef struct {
    UINT64 txg;
    UINT64 slots;
    UINT8 levels;
    POOL_POINTER table;
} POOL_ROOT;

UINT32 pool_get32(const UINT8 *p);
UINT64 pool_get64(const UINT8 *p);
void pool_put32(UINT8 *p, UINT32 value);
void pool_put64(UINT8 *p, UINT64 value);

/* UEFI's CRC32 (polynomial 0x04C11DB7, UEFI 2.11 4.2.1) of count bytes
   at bytes following those crc was computed over, 0 for none, so that a
   CRC over pieces equals the CRC over all of them. */
UINT32 pool_crc32(UINT32 crc, const UINT8 *bytes, UINT64 count);

/* The SHA-256 of size bytes. */
void pool_digest(const UINT8 *bytes, UINT64 size,
                 UINT8 digest[POOL_DIGEST_SIZE]);

int pool_same_digest(const UINT8 *a, const UINT8 *b);

/* Zero count bytes. */
void pool_zero(UINT8 *bytes, UINT64 count);

/* A pointer whose first copy is 0: no block, which reads as zeros. */
int pool_is_hole(const POOL_POINTER *pointer);
void pool_hole(POOL_POINTER *pointer);

void pool_pointer_read(const UINT8 *p, POOL_POINTER *pointer);
void pool_pointer_write(UINT8 *p, const POOL_POINTER *pointer);

void pool_record_read(const UINT8 *p, POOL_RECORD *record);
void pool_record_write(UINT8 *p, const POOL_RECORD *record);

/* The root block's fields from block, and block, POOL_BLOCK_SIZE bytes,
   holding root and zeros. */
void pool_root_read(const UINT8 *block, POOL_ROOT *root);
void pool_root_write(UINT8 *block, const POOL_ROOT *root);

/* block, POOL_BLOCK_SIZE bytes, as the superblock of commit txg in a pool
   of blocks blocks, whose root block root names. */
void pool_superblock_write(UINT8 *block, UINT64 blocks, UINT64 txg,
                           const POOL_POINTER *root);

/* Whether block holds a superblock of this format for a pool of blocks
   blocks, its digest checked; if so, its commit and root pointer. */
int pool_superblock_read(const UINT8 *block, UINT64 blocks, UINT64 *txg,
                         POOL_POINTER *root);

#endif
