/* The pool format's records (pool.h). Each field is read and written byte
   by byte, little-endian, at the offsets pool_format.h gives. */

#include "pool.h"
#include "sha256.h"

#define BYTE_BITS 8

/* CRC32 takes each byte least significant bit first, so the register
   shifts right against the bit-reversed polynomial; it starts as all
   ones and the result is inverted, which pool_crc32 undoes to go on. */
#define CRC32_REVERSED 0xedb88320u
#define CRC32_ALL_ONES 0xffffffffu

UINT32 pool_get32(const UINT8 *p)
{
    return (UINT32)p[0] | (UINT32)p[1] << 8 | (UINT32)p[2] << 16 |
           (UINT32)p[3] << 24;
}

UINT64 pool_get64(const UINT8 *p)
{
    return pool_get32(p) | (UINT64)pool_get32(p + 4) << 32;
}

void pool_put32(UINT8 *p, UINT32 value)
{
    UINT32 i;

    for (i = 0; i < 4; i++)
        p[i] = (UINT8)(value >> (BYTE_BITS * i));
}

void pool_put64(UINT8 *p, UINT64 value)
{
    pool_put32(p, (UINT32)value);
    pool_put32(p + 4, (UINT32)(value >> 32));
}

UINT32 pool_crc32(UINT32 crc, const UINT8 *bytes, UINT64 count)
{
    UINT32 r = ~crc, bit;

    for (; count > 0; count--) {
        r ^= *bytes++;
        for (bit = 0; bit < BYTE_BITS; bit++)
            r = (r & 1) ? (r >> 1) ^ CRC32_REVERSED : r >> 1;
    }
    return r ^ CRC32_ALL_ONES;
}

void pool_digest(const UINT8 *bytes, UINT64 size,
                 UINT8 digest[POOL_DIGEST_SIZE])
{
    SHA256_CONTEXT context;

    sha256_init(&context);
    sha256_update(&context, bytes, size);
    sha256_final(&context, digest);
}

int pool_same_digest(const UINT8 *a, const UINT8 *b)
{
    UINT32 i;

    for (i = 0; i < POOL_DIGEST_SIZE; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}

void pool_zero(UINT8 *bytes, UINT64 count)
{
    UINT64 i;

    for (i = 0; i < count; i++)
        bytes[i] = 0;
}

int pool_is_hole(const POOL_POINTER *pointer)
{
    return pointer->address[0] == 0;
}

void pool_hole(POOL_POINTER *pointer)
{
    UINT32 i;

    for (i = 0; i < POOL_COPIES; i++)
        pointer->address[i] = 0;
    pointer->birth = 0;
    pool_zero(pointer->digest, POOL_DIGEST_SIZE);
}

void pool_pointer_read(const UINT8 *p, POOL_POINTER *pointer)
{
    UINT32 i;

    pointer->address[0] = pool_get64(p + POOL_POINTER_FIRST);
    pointer->address[1] = pool_get64(p + POOL_POINTER_SECOND);
    pointer->birth = pool_get64(p + POOL_POINTER_BIRTH);
    for (i = 0; i < POOL_DIGEST_SIZE; i++)
        pointer->digest[i] = p[POOL_POINTER_DIGEST + i];
}

void pool_pointer_write(UINT8 *p, const POOL_POINTER *pointer)
{
    UINT32 i;

    pool_put64(p + POOL_POINTER_FIRST, pointer->address[0]);
    pool_put64(p + POOL_POINTER_SECOND, pointer->address[1]);
    pool_put64(p + POOL_POINTER_BIRTH, pointer->birth);
    pool_put64(p + POOL_POINTER_RESERVED, 0);
    for (i = 0; i < POOL_DIGEST_SIZE; i++)
        p[POOL_POINTER_DIGEST + i] = pointer->digest[i];
}

void pool_record_read(const UINT8 *p, POOL_RECORD *record)
{
    record->kind = p[POOL_RECORD_KIND];
    record->flags = p[POOL_RECORD_FLAGS];
    record->levels = p[POOL_RECORD_LEVELS];
    record->generation = pool_get32(p + POOL_RECORD_GENERATION);
    record->size = pool_get64(p + POOL_RECORD_SIZE_FIELD);
    pool_pointer_read(p + POOL_RECORD_ROOT, &record->root);
}

void pool_record_write(UINT8 *p, const POOL_RECORD *record)
{
    pool_zero(p, POOL_RECORD_SIZE);
    p[POOL_RECORD_KIND] = record->kind;
    p[POOL_RECORD_FLAGS] = record->flags;
    p[POOL_RECORD_LEVELS] = record->levels;
    pool_put32(p + POOL_RECORD_GENERATION, record->generation);
    pool_put64(p + POOL_RECORD_SIZE_FIELD, record->size);
    pool_pointer_write(p + POOL_RECORD_ROOT, &record->root);
}

void pool_root_read(const UINT8 *block, POOL_ROOT *root)
{
    root->txg = pool_get64(block + POOL_ROOT_TXG);
    root->slots = pool_get64(block + POOL_ROOT_SLOTS);
    root->levels = block[POOL_ROOT_TABLE_LEVELS];
    pool_pointer_read(block + POOL_ROOT_TABLE, &root->table);
}

void pool_root_write(UINT8 *block, const POOL_ROOT *root)
{
    pool_zero(block, POOL_BLOCK_SIZE);
    pool_put64(block + POOL_ROOT_TXG, root->txg);
    pool_put64(block + POOL_ROOT_SLOTS, root->slots);
    block[POOL_ROOT_TABLE_LEVELS] = root->levels;
    pool_pointer_write(block + POOL_ROOT_TABLE, &root->table);
}

void pool_superblock_write(UINT8 *block, UINT64 blocks, UINT64 txg,
                           const POOL_POINTER *root)
{
    UINT32 i;

    pool_zero(block, POOL_BLOCK_SIZE);
    for (i = 0; i < POOL_MAGIC_SIZE; i++)
        block[POOL_SUPER_MAGIC + i] = (UINT8)POOL_MAGIC[i];
    pool_put32(block + POOL_SUPER_VERSION, POOL_VERSION);
    pool_put32(block + POOL_SUPER_BLOCK_SIZE, POOL_BLOCK_SIZE);
    pool_put64(block + POOL_SUPER_BLOCKS, blocks);
    pool_put64(block + POOL_SUPER_TXG, txg);
    pool_pointer_write(block + POOL_SUPER_ROOT, root);
    pool_digest(block, POOL_SUPER_DIGEST, block + POOL_SUPER_DIGEST);
}

int pool_superblock_read(const UINT8 *block, UINT64 blocks, UINT64 *txg,
                         POOL_POINTER *root)
{
    UINT8 digest[POOL_DIGEST_SIZE];
    UINT32 i;

    for (i = 0; i < POOL_MAGIC_SIZE; i++)
        if (block[POOL_SUPER_MAGIC + i] != (UINT8)POOL_MAGIC[i])
            return 0;
    pool_digest(block, POOL_SUPER_DIGEST, digest);
    if (!pool_same_digest(digest, block + POOL_SUPER_DIGEST) ||
        pool_get32(block + POOL_SUPER_VERSION) != POOL_VERSION ||
        pool_get32(block + POOL_SUPER_BLOCK_SIZE) != POOL_BLOCK_SIZE ||
        pool_get64(block + POOL_SUPER_BLOCKS) != blocks)
        return 0;
    *txg = pool_get64(block + POOL_SUPER_TXG);
    pool_pointer_read(block + POOL_SUPER_ROOT, root);
    return 1;
}
