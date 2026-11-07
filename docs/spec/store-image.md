# Store image

The store image is the read-only, single-file form of the store's
content that [storage.md F16](storage.md#f16-the-first-milestone)
boots from: nova-boot loads `\nova\store.img` whole
([boot-protocol.md](boot-protocol.md#store-image)), the kernel passes it
to the first process as a section it may only read, and the store
service serves `/store` from it. It holds the records of the store
volume ([F8](storage.md#f8-the-store-volume)), registrations, nodes and
blobs, in the order of [F11](storage.md#f11-reproducible-images), with
no pool around them. `tools/mkstore.c` writes it, `store/reader.c` reads
it, and `include/store_image.h` gives its offsets and constants.

"FIPS 180-4" is the Secure Hash Standard pinned in
[README.md](README.md); every digest is a SHA-256, 32 bytes.

## Conventions

- Integers are little-endian and unaligned; a reader reads each field
  byte by byte.
- An offset in the header or in the blob table counts from the image's
  first byte; one in the registration or node table counts from that
  table's first byte.
- The image is a whole number of 4,096-byte blocks, and its header's
  `image_size` is its length.
- A name is a store path's base name, or a directory entry's name: not
  empty, `.` or `..`, with no `/` or NUL, UTF-8 (RFC 3629), at most 255
  bytes (`STORE_ENTRY_NAME_MAX`, POSIX's `NAME_MAX`), and a base name at
  most 244 (`STORE_BASE_NAME_MAX`: 32 characters of hash, `-`, and a name
  of at most 211, upstream Nix's `StorePath::MaxPathLen`). A link's
  target is 1 to 4,095 bytes with no NUL (`STORE_TARGET_MAX`, `PATH_MAX`
  less its terminator).

## Header

At offset 0, 360 bytes:

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | `magic`, `NOVASTOR` |
| 8 | 4 | `version`, 1 |
| 12 | 4 | Registrations |
| 16 | 4 | Nodes |
| 20 | 4 | Blobs |
| 24 | 8 | `image_size` |
| 32 | 8 | The registration table's offset |
| 40 | 8 | Its size |
| 48 | 8 | The node table's offset |
| 56 | 8 | Its size |
| 64 | 8 | The blob table's offset |
| 72 | 8 | Its size |
| 80 | 32 | The digest of the registration table, all its bytes |
| 112 | 4 | The boot path's base name's length |
| 116 | 244 | The boot path's base name, zero after it |

The boot path is the store path holding the programs the first process
starts: `bin/store.exe`, the store service, and `bin/namespace.exe`, the
namespace server.

## Registrations

One 104-byte entry per store path, in bytewise order of base name, then
the bytes the entries point into:

| Offset | Size | Field |
|---|---|---|
| 0 | 32 | The root node's digest |
| 32 | 32 | The NAR hash |
| 64 | 8 | The NAR's size |
| 72 | 4, 4 | The base name's offset and length |
| 80 | 4, 4 | The references' offset and count |
| 88 | 4, 4 | The deriver's base name's offset and length, 0 for none |
| 96 | 4, 4 | The content address's offset and length, 0 for none |

A reference is the 4-byte index of a registration in this table, so the
image's paths are closed under references
([storage.md, fact 5](storage.md#what-the-store-holds)); a path's
references are in ascending order. The content address is upstream's
text form (`fixed:r:sha256:` and the NAR hash in nix-base32), and there
is no registration time ([F8](storage.md#f8-the-store-volume)).

## Nodes

One 40-byte index entry per node in bytewise order of digest, then the
encodings:

| Offset | Size | Field |
|---|---|---|
| 0 | 32 | The node's digest |
| 32 | 4 | Its encoding's offset |
| 36 | 4 | Its encoding's length |

A node's digest is the SHA-256 of its encoding, whose first byte is its
kind:

| Kind | Then |
|---|---|
| 1, directory | The entry count (4), then each entry in bytewise order of name: the name's length (4), the name, the entry's node digest (32) |
| 2, file | The executable flag, 0 or 1 (1); the length (8); the blob's digest (32) |
| 3, link | The target's length (4), the target |

A directory's entries fill its encoding exactly. Identical subtrees, in
one path or several, are one node.

## Blobs

One 56-byte entry per blob in bytewise order of digest:

| Offset | Size | Field |
|---|---|---|
| 0 | 32 | The blob's digest |
| 32 | 8 | Its length |
| 40 | 8 | Its hash tree's offset |
| 48 | 8 | Its data's offset, a multiple of 4,096 |

A blob is the bytes of one or more files with the same contents; a
file's executable flag is in its node, not its blob. Its hash tree is
over blocks of 4,096 bytes:

1. Level 0 is the blob's bytes in blocks of 4,096, the last shorter if
   the length is not a multiple of 4,096; an empty blob has one block of
   no bytes.
2. Level k + 1 is the digests of level k's blocks, one after another, in
   blocks of 4,096 bytes (128 digests).
3. A block's digest is the SHA-256 of 16 bytes that bind its place,
   then its bytes: its offset within its level (8), the level (4) and
   the bytes hashed (4).
4. The top is the first level with one block, whose digest is the top
   digest.
5. The blob's digest is the SHA-256 of its length (8) and the top
   digest.

The hash tree in the image is every level of more than one digest, from
level 0's up, one after another; a blob of one block has none, its top
digest being its block's. Binding a block's offset, level and length
gives each block one place in the tree, as Fuchsia's Merkle roots do
([storage.md F8](storage.md#f8-the-store-volume)).

## Order

The header, then from offset 360 the registration table, the node
table and the blob table, each starting at a multiple of 8, then the
blobs' hash trees, then from the next block boundary each blob's data,
a whole number of blocks with zeros after its bytes. Nodes' encodings,
hash trees and data are in the order each first occurs: the paths in
base-name order, each path's nodes in NAR order, a file's blob with its
node ([F11](storage.md#f11-reproducible-images)). Nothing in the image
records a time or the order its inputs were given in, so the same paths
give the same bytes.

## Reading

A reader checks, before it uses what each names:

- the header's magic and version, and that every table lies within the
  image and holds its count of entries;
- the registration table against the header's digest, and in it that
  each base name is not empty, `.` or `..`, holds no `/` or NUL, is at
  most 244 bytes and follows the one before it in bytewise order, and
  that each reference names a registration;
- that the node table holds every node a registration or a directory
  names, each node's encoding against its digest, and its form: a
  directory's entries filling it, each name as a base name above but at
  most 255 bytes, in strictly increasing order, a file's encoding
  exactly 42 bytes, a link's target 1 to 4,095 bytes with no NUL;
- a blob's length against its file node's, its tree and data within the
  image, and its tree against its digest;
- every block a read touches against its level-0 digest.

A check that fails is an integrity error: the read that needs the
record fails, and nothing else does
([F9](storage.md#f9-reading-and-verifying-the-store)). UTF-8 is
mkstore's to check as it writes the image. No digest
covers the header; once the pool exists, the store service checks a boot
image's paths against their registrations
([F14](storage.md#f14-disks-and-the-system-recipe)).
