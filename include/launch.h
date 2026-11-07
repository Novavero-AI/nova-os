/** @file
    The bootstrap message a launcher sends a program it starts from /store
    (docs/spec/storage.md F5, objects.md O6): the program's view, a table
    of path prefixes and the directories they name, and its arguments.

    Its handles, in order: the debug log's resource, the program's own
    process with MAP, then one directory channel per view entry. Its
    bytes: a LAUNCH_HEADER, then view_count LAUNCH_VIEW_ENTRYs in the
    order of the directories, then argument_bytes of arguments, each
    NUL-terminated. A prefix is an absolute path with no empty, "." or
    ".." component. */

#ifndef NOVA_LAUNCH_H
#define NOVA_LAUNCH_H

#include "native_abi.h"

/** @name Handles
    The message's handles, in this order.
    @{ */
#define LAUNCH_DEBUG_LOG 0 /**< the debug log's resource */
#define LAUNCH_SELF 1      /**< the program's process, with MAP */
/** the first view entry's directory; one follows for each entry */
#define LAUNCH_FIRST_DIRECTORY 2
/** @} */

/** @name Limits
    The most view entries a message carries, the longest prefix, and the
    most argument bytes.
    @{ */
#define LAUNCH_VIEW_MAX 16        /**< the most view entries */
#define LAUNCH_PREFIX_MAX 256     /**< the longest prefix, in bytes */
#define LAUNCH_ARGUMENTS_MAX 4096 /**< the most argument bytes */
/** @} */

/** What the message's bytes start with. */
typedef struct {
    UINT32 view_count;     /**< the view entries that follow */
    UINT32 argument_bytes; /**< the bytes of arguments after them */
} LAUNCH_HEADER;

/** A view entry: a prefix, whose directory is the handle in the same
    place among the directories. */
typedef struct {
    UINT32 length;                   /**< the prefix's bytes */
    UINT8 prefix[LAUNCH_PREFIX_MAX]; /**< the prefix */
} LAUNCH_VIEW_ENTRY;

_Static_assert(LAUNCH_FIRST_DIRECTORY + LAUNCH_VIEW_MAX <= CHANNEL_MAX_HANDLES,
               "one message carries a whole view");
_Static_assert(sizeof(LAUNCH_HEADER) +
                       LAUNCH_VIEW_MAX * sizeof(LAUNCH_VIEW_ENTRY) +
                       LAUNCH_ARGUMENTS_MAX <=
                   CHANNEL_MAX_BYTES,
               "one message carries a whole view and its arguments");

#endif
