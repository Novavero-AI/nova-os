/* The bootstrap message a launcher sends a program it starts from /store
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

#define LAUNCH_DEBUG_LOG 0
#define LAUNCH_SELF 1
#define LAUNCH_FIRST_DIRECTORY 2

/* The most view entries a message carries, the longest prefix, and the
   most argument bytes. */
#define LAUNCH_VIEW_MAX 16
#define LAUNCH_PREFIX_MAX 256
#define LAUNCH_ARGUMENTS_MAX 4096

typedef struct {
    UINT32 view_count;
    UINT32 argument_bytes;
} LAUNCH_HEADER;

typedef struct {
    UINT32 length;
    UINT8 prefix[LAUNCH_PREFIX_MAX];
} LAUNCH_VIEW_ENTRY;

_Static_assert(LAUNCH_FIRST_DIRECTORY + LAUNCH_VIEW_MAX <= CHANNEL_MAX_HANDLES,
               "one message carries a whole view");
_Static_assert(sizeof(LAUNCH_HEADER) +
                       LAUNCH_VIEW_MAX * sizeof(LAUNCH_VIEW_ENTRY) +
                       LAUNCH_ARGUMENTS_MAX <=
                   CHANNEL_MAX_BYTES,
               "one message carries a whole view and its arguments");

#endif
