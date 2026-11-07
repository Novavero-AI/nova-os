/* The pool format, version 1 (docs/spec/pool-format.md): a data disk's
   pool partition as tools/mkdata.c writes it and the file system service
   (fs/fs.c) reads and commits it (docs/spec/storage.md F6, F7, F10).
   Every integer is little-endian, and the offsets below are byte offsets
   within the record they name; fs/pool.c reads and writes each field
   byte by byte, so no record needs alignment. */

#ifndef NOVA_POOL_FORMAT_H
#define NOVA_POOL_FORMAT_H

/* The partition's type GUID, made once at random, in GPT's on-disk byte
   order (its first three fields little-endian, UEFI 2.11 appendix A):
   B1768F24-225A-436D-AABD-669B874BAC7F. */
#define POOL_PARTITION_TYPE                                              \
    {0x24, 0x8f, 0x76, 0xb1, 0x5a, 0x22, 0x6d, 0x43,                     \
     0xaa, 0xbd, 0x66, 0x9b, 0x87, 0x4b, 0xac, 0x7f}
#define POOL_GUID_SIZE 16

/* The pool is a sequence of POOL_BLOCK_SIZE blocks from the partition's
   first sector, as many as the partition holds whole. The superblock
   rings take its first and last POOL_RING_SLOTS blocks; the rest is the
   data area. Block 0 is a ring slot, so a block address of 0 names no
   block. */
#define POOL_BLOCK_SIZE 4096
#define POOL_SECTOR_SIZE 512
#define POOL_SECTORS_PER_BLOCK (POOL_BLOCK_SIZE / POOL_SECTOR_SIZE)
#define POOL_RING_SLOTS 4
#define POOL_DIGEST_SIZE 32

/* The fewest blocks a pool holds: both rings and room for a committed
   empty volume twice over. */
#define POOL_BLOCKS_MIN 64

/* A superblock, at the start of its slot's block, the rest of which is
   zero. Commit t writes slot t % POOL_RING_SLOTS of each ring; its digest
   covers the bytes before it. */
#define POOL_MAGIC "NOVAPOOL"
#define POOL_MAGIC_SIZE 8
#define POOL_VERSION 1
#define POOL_SUPER_MAGIC 0
#define POOL_SUPER_VERSION 8
#define POOL_SUPER_BLOCK_SIZE 12
#define POOL_SUPER_BLOCKS 16
#define POOL_SUPER_TXG 24
#define POOL_SUPER_ROOT 32
#define POOL_SUPER_DIGEST 96
#define POOL_SUPER_SIZE 128

/* A block pointer: the block's first copy and its second, 0 where it has
   none; the transaction group that wrote it; and the SHA-256 of its
   POOL_BLOCK_SIZE bytes. A pointer whose first copy is 0 is a hole,
   which reads as zeros, and is all zero. */
#define POOL_POINTER_FIRST 0
#define POOL_POINTER_SECOND 8
#define POOL_POINTER_BIRTH 16
#define POOL_POINTER_RESERVED 24
#define POOL_POINTER_DIGEST 32
#define POOL_POINTER_SIZE 64
#define POOL_COPIES 2

/* An object's blocks hang from one pointer as a tree of POOL_LEVELS_MAX
   levels at most: with levels 0 it has none, with 1 the pointer names
   its block 0, and with n above 1 an indirect block of POOL_FANOUT
   pointers whose children cover POOL_FANOUT^(n - 2) blocks each. */
#define POOL_FANOUT (POOL_BLOCK_SIZE / POOL_POINTER_SIZE)
#define POOL_LEVELS_MAX 5

/* The root block: the transaction group that wrote it, the node table's
   slots, and the node table's levels and pointer. The rest is zero. */
#define POOL_ROOT_TXG 0
#define POOL_ROOT_SLOTS 8
#define POOL_ROOT_TABLE_LEVELS 16
#define POOL_ROOT_TABLE 24
#define POOL_ROOT_SIZE (POOL_ROOT_TABLE + POOL_POINTER_SIZE)

/* A node record, POOL_RECORD_SIZE bytes, POOL_RECORDS_PER_BLOCK to a
   node table block; slot n is record n of the table. A free slot keeps
   its generation. Slot 0 is the root directory. */
#define POOL_RECORD_KIND 0
#define POOL_RECORD_FLAGS 1
#define POOL_RECORD_LEVELS 2
#define POOL_RECORD_GENERATION 4
#define POOL_RECORD_SIZE_FIELD 8
#define POOL_RECORD_ROOT 16
#define POOL_RECORD_SIZE 128
#define POOL_RECORDS_PER_BLOCK (POOL_BLOCK_SIZE / POOL_RECORD_SIZE)
#define POOL_ROOT_SLOT 0

/* A record's kind, the directory protocol's (directory_protocol.h), and
   its flag. */
#define POOL_KIND_FREE 0
#define POOL_KIND_DIRECTORY 1
#define POOL_KIND_FILE 2
#define POOL_FLAG_EXECUTABLE 0x1

/* A directory's bytes are its entries in bytewise order of name, each
   its node's slot, kind and name's length, then the name. */
#define POOL_ENTRY_SLOT 0
#define POOL_ENTRY_KIND 4
#define POOL_ENTRY_LENGTH 5
#define POOL_ENTRY_NAME 6
#define POOL_NAME_MAX 255

#endif
