/* Loading a user program's PE32+ image into an address space, in the
   format docs/spec/boot-protocol.md (The first process) gives. */

#ifndef NOVA_IMAGE_H
#define NOVA_IMAGE_H

#include "../status.h"

/* Where an image was placed: the user addresses of its first byte, of
   the byte past its last page, and of its entry point. */
typedef struct {
    UINT64 base;
    UINT64 end;
    UINT64 entry;
} IMAGE;

/* Load the image in the size bytes at file into space at its ImageBase,
   page by page, each page with the rights of the sections on it; the
   image must end at or below limit. Returns OK; ERR_INVALID_ARGS if the
   file is not an image this kernel loads, which leaves space as it was;
   or ERR_NO_MEMORY, which can leave pages of the image mapped. */
STATUS image_load(UINT64 space, const UINT8 *file, UINT64 size, UINT64 limit,
                  IMAGE *image);

#endif
