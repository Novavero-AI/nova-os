/** @file
    The store image (docs/spec/store-image.md): the read-only, single-file
    form of the store's content that nova-boot loads and the store service
    serves /store from (docs/spec/storage.md F16). tools/mkstore.c writes
    it and store/reader.c reads it. Every integer is little-endian, and the
    offsets below are byte offsets within the record they name; each field
    is read byte by byte, so no record needs alignment. */

#ifndef NOVA_STORE_IMAGE_H
#define NOVA_STORE_IMAGE_H

/** @name Magic and version
    "NOVASTOR", the first 8 bytes, and the layout's version.
    @{ */
#define STORE_MAGIC "NOVASTOR" /**< the magic */
#define STORE_MAGIC_SIZE 8     /**< the magic's bytes */
#define STORE_VERSION 1        /**< the layout's version */
/** @} */

/** @name Blocks and digests
    A blob's bytes are hashed in blocks of STORE_BLOCK_SIZE, and the image
    is a whole number of them.
    @{ */
#define STORE_BLOCK_SIZE 4096 /**< the bytes of a hashed block */
#define STORE_DIGEST_SIZE 32  /**< the bytes of a digest */
/** @} */

/** @name Base names
    A store path's base name: 32 nix-base32 characters of hash, '-', and a
    name of at most 211 bytes (upstream's StorePath::MaxPathLen).
    @{ */
#define STORE_HASH_CHARS 32 /**< the hash's characters */
#define STORE_NAME_MAX 211  /**< the longest name, in bytes */
/** the longest base name, in bytes */
#define STORE_BASE_NAME_MAX (STORE_HASH_CHARS + 1 + STORE_NAME_MAX)
/** @} */

/** @name Names and targets
    The longest directory entry name and link target an image holds:
    NAME_MAX and PATH_MAX less its terminator, the most a POSIX file system
    that restores the path's NAR keeps.
    @{ */
#define STORE_ENTRY_NAME_MAX 255 /**< the longest entry name, in bytes */
#define STORE_TARGET_MAX 4095    /**< the longest link target, in bytes */
/** @} */

/** @name The header
    The header, at offset 0. Each table's offset is from the image's
    first byte; boot_name is the base name of the store path holding the
    programs the first process starts, zero-filled after its length.
    @{ */
#define STORE_HEADER_MAGIC 0               /**< the magic (8) */
#define STORE_HEADER_VERSION 8             /**< the version (4) */
#define STORE_HEADER_REGISTRATION_COUNT 12 /**< the registrations (4) */
#define STORE_HEADER_NODE_COUNT 16         /**< the nodes (4) */
#define STORE_HEADER_BLOB_COUNT 20         /**< the blobs (4) */
#define STORE_HEADER_IMAGE_SIZE 24         /**< the image's size (8) */
/** the registration table's offset (8) */
#define STORE_HEADER_REGISTRATIONS 32
/** the registration table's size (8) */
#define STORE_HEADER_REGISTRATIONS_SIZE 40
#define STORE_HEADER_NODES 48      /**< the node table's offset (8) */
#define STORE_HEADER_NODES_SIZE 56 /**< the node table's size (8) */
#define STORE_HEADER_BLOBS 64      /**< the blob table's offset (8) */
#define STORE_HEADER_BLOBS_SIZE 72 /**< the blob table's size (8) */
/** the registration table's digest (32) */
#define STORE_HEADER_REGISTRATIONS_DIGEST 80
#define STORE_HEADER_BOOT_NAME_LENGTH 112 /**< boot_name's length (4) */
#define STORE_HEADER_BOOT_NAME 116        /**< boot_name */
/** the bytes of the header */
#define STORE_HEADER_SIZE (STORE_HEADER_BOOT_NAME + STORE_BASE_NAME_MAX)
/** @} */

/** @name Registrations
    The registration table: one entry per store path in bytewise order of
    base name, then the strings and reference lists the entries point
    into, each offset from the table's first byte. A reference is the
    4-byte index of a registration in this table, and a path's references
    are in ascending order.
    @{ */
#define STORE_REGISTRATION_ROOT 0         /**< the root node's digest (32) */
#define STORE_REGISTRATION_NAR_HASH 32    /**< the NAR hash (32) */
#define STORE_REGISTRATION_NAR_SIZE 64    /**< the NAR's size (8) */
#define STORE_REGISTRATION_NAME 72        /**< the base name's offset (4) */
#define STORE_REGISTRATION_NAME_LENGTH 76 /**< the base name's length (4) */
#define STORE_REGISTRATION_REFERENCES 80  /**< the references' offset (4) */
/** the references' count (4) */
#define STORE_REGISTRATION_REFERENCE_COUNT 84
/** the deriver's offset, 0 for none (4) */
#define STORE_REGISTRATION_DERIVER 88
/** the deriver's length, 0 for none (4) */
#define STORE_REGISTRATION_DERIVER_LENGTH 92
/** the content address's offset, 0 for none (4) */
#define STORE_REGISTRATION_CA 96
/** the content address's length, 0 for none (4) */
#define STORE_REGISTRATION_CA_LENGTH 100
#define STORE_REGISTRATION_SIZE 104 /**< the bytes of an entry */
#define STORE_REFERENCE_SIZE 4      /**< the bytes of a reference */
/** @} */

/** @name The node table
    The node table: one index entry per node in bytewise order of digest,
    then the nodes' encodings, each offset from the table's first byte.
    @{ */
#define STORE_NODE_ENTRY_DIGEST 0  /**< the node's digest (32) */
#define STORE_NODE_ENTRY_OFFSET 32 /**< its encoding's offset (4) */
#define STORE_NODE_ENTRY_LENGTH 36 /**< its encoding's length (4) */
#define STORE_NODE_ENTRY_SIZE 40   /**< the bytes of an index entry */
/** @} */

/** @name Node encodings
    A node's encoding, whose SHA-256 is its digest. The first byte is its
    kind. A directory follows it with its entry count and its entries in
    bytewise order of name, each a name length, the name and the entry's
    node digest; a file with its executable flag, its length and its
    blob's digest; a link with its target's length and the target.
    @{ */
#define STORE_NODE_DIRECTORY 1    /**< a directory's kind */
#define STORE_NODE_FILE 2         /**< a file's kind */
#define STORE_NODE_LINK 3         /**< a link's kind */
#define STORE_NODE_KIND 0         /**< the kind (1) */
#define STORE_DIRECTORY_COUNT 1   /**< a directory's entry count (4) */
#define STORE_DIRECTORY_ENTRIES 5 /**< a directory's entries */
/** an entry's name, after its 4-byte length */
#define STORE_ENTRY_NAME 4
#define STORE_FILE_EXECUTABLE 1 /**< a file's executable flag (1) */
#define STORE_FILE_LENGTH 2     /**< a file's length (8) */
#define STORE_FILE_BLOB 10      /**< a file's blob's digest (32) */
#define STORE_FILE_SIZE 42      /**< the bytes of a file's encoding */
#define STORE_LINK_LENGTH 1     /**< a link's target's length (4) */
#define STORE_LINK_TARGET 5     /**< a link's target */
/** @} */

/** @name The blob table
    The blob table: one entry per blob in bytewise order of digest. The
    hash tree and the data are at offsets from the image's first byte;
    the data starts on a block boundary.
    @{ */
#define STORE_BLOB_DIGEST 0      /**< the blob's digest (32) */
#define STORE_BLOB_LENGTH 32     /**< the blob's length (8) */
#define STORE_BLOB_TREE 40       /**< its hash tree's offset (8) */
#define STORE_BLOB_DATA 48       /**< its data's offset (8) */
#define STORE_BLOB_ENTRY_SIZE 56 /**< the bytes of an entry */
/** @} */

/** A hash-tree block's digest is the SHA-256 of a prefix that binds the
    block's place, then its bytes: the block's byte offset within its
    level (8 bytes), the level (4) and the bytes hashed (4). */
#define STORE_TREE_PREFIX_SIZE 16

#endif
