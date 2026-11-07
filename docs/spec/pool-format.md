# Pool format

The on-disk form of a pool of the native file system, version 1: the
partition a data disk holds, its superblock rings, and the copy-on-write
tree of one data volume under them
([storage.md F6, F7, F10, F18](storage.md#f18-the-writable-milestone)).
`tools/mkdata.c` writes an empty one, `fs/fs.c` opens and commits it,
`fs/pool.c` encodes its records for both, and `include/pool_format.h`
gives its offsets and constants.

"UEFI 5.3.3" is a section of UEFI 2.11 and "FIPS 180-4" the Secure Hash
Standard, as pinned in [README.md](README.md); every digest is a SHA-256,
32 bytes.

## Conventions

- Integers are little-endian and unaligned; a reader reads each field
  byte by byte.
- A block is 4,096 bytes. A block number counts from the pool's first
  block.
- A field this document does not name is zero when written and ignored
  when read.

## The disk

A data disk is partitioned with a GPT, as
[disk-layout.md](disk-layout.md) lays out the boot disk: a protective MBR
in LBA 0, the primary header and its 128-entry array from LBA 1, and the
backup array and header in the last 33 sectors. Its one partition has:

- `PartitionTypeGUID` B1768F24-225A-436D-AABD-669B874BAC7F, made at
  random for the pool (UEFI 5.3.3);
- `StartingLBA` 2048, and an `EndingLBA` that leaves the partition a
  whole number of blocks;
- `PartitionName` "nova-os pool".

mkdata's `DiskGUID` and `UniquePartitionGUID` are constants, as mkimage's
are, under the rule disk-layout.md leaves open ("Identifiers"). A disk
whose primary GPT header fails its signature, its CRC or its own LBA, or
whose partition entry array fails its CRC, is refused (UEFI 5.3.2), as is
one whose pool entry lies outside the header's usable LBAs or overlaps
another used entry (5.3.1); the backup table is not read.

The pool is the partition's blocks. Its first 4 and last 4 blocks are the
two superblock rings; the rest is the data area, which holds every other
block. Block 0 lies in the first ring, so a block number of 0 names no
block.

## Superblocks

Commit `t` writes its superblock to slot `t` mod 4 of each ring, the
first 128 bytes of the slot's block, the rest zero:

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | `magic`, `NOVAPOOL` |
| 8 | 4 | `version`, 1 |
| 12 | 4 | The block size, 4,096 |
| 16 | 8 | The pool's blocks |
| 24 | 8 | `t`, the commit's transaction group |
| 32 | 64 | The root block's pointer |
| 96 | 32 | The digest of bytes 0 to 95 |

A slot is valid if its magic, version, block size, block count and digest
check and `t` mod 4 is its slot. Opening a pool takes the valid slot of
either ring with the highest `t`. The slot a commit writes is never the
one the last commit wrote, so a superblock torn by a crash leaves the one
before it whole.

## Block pointers

64 bytes:

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | The block's first copy |
| 8 | 8 | Its second copy, 0 if it has none |
| 16 | 8 | The transaction group that wrote it |
| 24 | 8 | Reserved |
| 32 | 32 | The digest of the block's 4,096 bytes |

A pointer whose first copy is 0 is a hole, reads as 4,096 zeros, and is
all zero. A block read is checked against its pointer's digest; a copy
that fails is not used, the second copy is tried, and a block with no
copy that passes is an integrity error
([storage.md F7](storage.md#f7-checksums)). Metadata has two copies, the
first placed from the data area's start and the second from the pool's
middle; a file's data has one. Metadata is the root block, the node
table's blocks, every directory's blocks and every indirect block.

## Objects

An object is a tree of blocks under one pointer and a level count: with
0 levels it has no block and its pointer is a hole; with 1 the pointer
names its block 0; with `n` above 1 the pointer names an indirect block of
64 pointers, each covering 64^(`n` - 2) of its blocks, block `i` under
pointer `i` / 64^(`n` - 2). Every level is at most 5, so an object holds
at most 64^4 blocks, 64 GiB. A hole anywhere reads as zeros, and an
object's bytes past its size are zero.

## The root block

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | The transaction group that wrote it, the superblock's `t` |
| 8 | 8 | The node table's slots, a multiple of 32 |
| 16 | 1 | The node table's levels, 1 to 5 |
| 24 | 64 | The node table's pointer |

## The node table

An object whose blocks each hold 32 node records of 128 bytes; slot `s`
is record `s` mod 32 of block `s` / 32. Slot 0 is the volume's root
directory.

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | The kind: 0 free, 1 directory, 2 file |
| 1 | 1 | Flags: 1, the file is executable |
| 2 | 1 | The node's object's levels |
| 4 | 4 | The generation |
| 8 | 8 | A file's size in bytes, a directory's entries' bytes |
| 16 | 64 | The node's object's pointer |

A free slot has no object and keeps its generation; a slot taken for a
new node gets the next generation, so a node is named within the volume
by its slot and generation (F2's node ID). A directory's entries are at
most 65,536 bytes.

## Directories

A directory's object holds its entries in bytewise order of name, packed,
each:

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | The node's slot |
| 4 | 1 | Its kind, as its record's |
| 5 | 1 | The name's length, 1 to 255 |
| 6 | length | The name: UTF-8 (RFC 3629), not `.` or `..`, with no `/` or NUL |

## A commit

A commit `t` writes every block that changed since commit `t` - 1 to free
blocks, each pointer naming it with `t`, from the leaves up through each
object, then the node table, then a new root block; flushes the device;
writes superblock `t` in both rings; and flushes again
([storage.md F6](storage.md#f6-pools-volumes-and-the-commit)). A block
the commit releases is not written again until the second flush has
completed. The pool holds no map of free blocks: opening it marks every
block the newest commit's tree names, reading and checking every
metadata block, and every other block of the data area is free.

## An empty pool

mkdata writes transaction group 1: a node table of one block whose slot 0
is an empty root directory of generation 1, at the data area's first
block and the pool's middle; the root block, naming 32 slots, at the
blocks after those; and superblock 1 in slot 1 of each ring.
