/** @file
    The pool format, version 1 (docs/spec/pool-format.md): a data disk's
    pool partition as tools/mkdata.c writes it and the file system service
    (fs/fs.c) reads and commits it (docs/spec/storage.md F6, F7, F10).
    Every integer is little-endian, and the offsets below are byte offsets
    within the record they name; fs/pool.c reads and writes each field
    byte by byte, so no record needs alignment. */

#ifndef NOVA_POOL_FORMAT_H
#define NOVA_POOL_FORMAT_H

/** The partition's type GUID, made once at random, in GPT's on-disk byte
    order (its first three fields little-endian, UEFI 2.11 appendix A):
    B1768F24-225A-436D-AABD-669B874BAC7F. */
#define POOL_PARTITION_TYPE                                              \
    {0x24, 0x8f, 0x76, 0xb1, 0x5a, 0x22, 0x6d, 0x43,                     \
     0xaa, 0xbd, 0x66, 0x9b, 0x87, 0x4b, 0xac, 0x7f}
#define POOL_GUID_SIZE 16 /**< the bytes of a GUID */

/** @name Blocks
    The pool is a sequence of POOL_BLOCK_SIZE blocks from the partition's
    first sector, as many as the partition holds whole. The superblock
    rings take its first and last POOL_RING_SLOTS blocks; the rest is the
    data area. Block 0 is a ring slot, so a block address of 0 names no
    block.
    @{ */
#define POOL_BLOCK_SIZE 4096 /**< the bytes of a block */
#define POOL_SECTOR_SIZE 512 /**< the bytes of a sector */
/** the sectors of a block */
#define POOL_SECTORS_PER_BLOCK (POOL_BLOCK_SIZE / POOL_SECTOR_SIZE)
#define POOL_RING_SLOTS 4   /**< the blocks of each ring */
#define POOL_DIGEST_SIZE 32 /**< the bytes of a digest */
/** @} */

/** The fewest blocks a pool holds: both rings and room for a committed
    empty volume twice over. */
#define POOL_BLOCKS_MIN 64

/** @name Superblocks
    A superblock, at the start of its slot's block, the rest of which is
    zero. Commit t writes slot t % POOL_RING_SLOTS of each ring; its digest
    covers the bytes before it.
    @{ */
#define POOL_MAGIC "NOVAPOOL"    /**< the magic */
#define POOL_MAGIC_SIZE 8        /**< the magic's bytes */
#define POOL_VERSION 1           /**< the format's version */
#define POOL_SUPER_MAGIC 0       /**< the magic (8) */
#define POOL_SUPER_VERSION 8     /**< the version (4) */
#define POOL_SUPER_BLOCK_SIZE 12 /**< the block size (4) */
#define POOL_SUPER_BLOCKS 16     /**< the pool's blocks (8) */
#define POOL_SUPER_TXG 24        /**< t, the commit's transaction group (8) */
#define POOL_SUPER_ROOT 32       /**< the root block's pointer (64) */
#define POOL_SUPER_DIGEST 96     /**< the digest (32) */
#define POOL_SUPER_SIZE 128      /**< the bytes of a superblock */
/** @} */

/** @name Block pointers
    A block pointer: the block's first copy and its second, 0 where it has
    none; the transaction group that wrote it; and the SHA-256 of its
    POOL_BLOCK_SIZE bytes. A pointer whose first copy is 0 is a hole,
    which reads as zeros, and is all zero.
    @{ */
#define POOL_POINTER_FIRST 0     /**< the first copy (8) */
#define POOL_POINTER_SECOND 8    /**< the second copy (8) */
#define POOL_POINTER_BIRTH 16    /**< the group that wrote it (8) */
#define POOL_POINTER_RESERVED 24 /**< reserved, 0 (8) */
#define POOL_POINTER_DIGEST 32   /**< the digest (32) */
#define POOL_POINTER_SIZE 64     /**< the bytes of a pointer */
#define POOL_COPIES 2            /**< the copies a pointer names */
/** @} */

/** @name Objects
    An object's blocks hang from one pointer as a tree of POOL_LEVELS_MAX
    levels at most: with levels 0 it has none, with 1 the pointer names
    its block 0, and with n above 1 an indirect block of POOL_FANOUT
    pointers whose children cover POOL_FANOUT^(n - 2) blocks each.
    @{ */
/** the pointers of an indirect block */
#define POOL_FANOUT (POOL_BLOCK_SIZE / POOL_POINTER_SIZE)
#define POOL_LEVELS_MAX 5 /**< the most levels */
/** @} */

/** @name The root block
    The root block: the transaction group that wrote it, the node table's
    slots, and the node table's levels and pointer. The rest is zero.
    @{ */
#define POOL_ROOT_TXG 0           /**< the group that wrote it (8) */
#define POOL_ROOT_SLOTS 8         /**< the node table's slots (8) */
#define POOL_ROOT_TABLE_LEVELS 16 /**< the node table's levels (1) */
#define POOL_ROOT_TABLE 24        /**< the node table's pointer (64) */
/** the bytes of its fields */
#define POOL_ROOT_SIZE (POOL_ROOT_TABLE + POOL_POINTER_SIZE)
/** @} */

/** @name Node records
    A node record, POOL_RECORD_SIZE bytes, POOL_RECORDS_PER_BLOCK to a
    node table block; slot n is record n of the table. A free slot keeps
    its generation. Slot 0 is the root directory.
    @{ */
#define POOL_RECORD_KIND 0       /**< the kind (1) */
#define POOL_RECORD_FLAGS 1      /**< the flags (1) */
#define POOL_RECORD_LEVELS 2     /**< the levels of the node's object (1) */
#define POOL_RECORD_GENERATION 4 /**< the generation (4) */
/** a file's size, or a directory's entries' bytes (8) */
#define POOL_RECORD_SIZE_FIELD 8
#define POOL_RECORD_ROOT 16  /**< the node's object's pointer (64) */
#define POOL_RECORD_SIZE 128 /**< the bytes of a record */
/** the records of a node table block */
#define POOL_RECORDS_PER_BLOCK (POOL_BLOCK_SIZE / POOL_RECORD_SIZE)
#define POOL_ROOT_SLOT 0 /**< the root directory's slot */
/** @} */

/** @name Kinds and flags
    A record's kind, the directory protocol's (directory_protocol.h), and
    its flag.
    @{ */
#define POOL_KIND_FREE 0         /**< a free slot */
#define POOL_KIND_DIRECTORY 1    /**< a directory */
#define POOL_KIND_FILE 2         /**< a file */
#define POOL_FLAG_EXECUTABLE 0x1 /**< the file is executable */
/** @} */

/** @name Directory entries
    A directory's bytes are its entries in bytewise order of name, each
    its node's slot, kind and name's length, then the name.
    @{ */
#define POOL_ENTRY_SLOT 0   /**< the node's slot (4) */
#define POOL_ENTRY_KIND 4   /**< its kind (1) */
#define POOL_ENTRY_LENGTH 5 /**< the name's length (1) */
#define POOL_ENTRY_NAME 6   /**< the name */
#define POOL_NAME_MAX 255   /**< the longest name, in bytes */
/** @} */

#endif
