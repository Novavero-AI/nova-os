/* Starting a 32-bit Windows program (docs/spec/compat.md C1): a new
   process holding the program's PE32 image and ntdll's, each at its own
   ImageBase, a heap, a stack and the environment block, whose first
   thread starts at ntdll's entry with the view's directory channel. */

#ifndef NOVA_COMPAT_H
#define NOVA_COMPAT_H

#include "init.h"

/* An image file's bytes, in init's memory, and the name the process's
   loader list and parameters give it. */
typedef struct {
    const char *name;
    const UINT8 *data;
    UINT64 size;
} COMPAT_IMAGE;

/* Start program, with ntdll beside it, in a new 32-bit process in job
   whose command line is command_line, ASCII. Returns the process's
   handle, with the server end of its view's directory channel in
   *directory; 0 if it did not start, with nothing left open. The
   process must end before the next is started: they share the pages
   init fills for them. */
UINT32 compat_start(UINT32 job, const COMPAT_IMAGE *program,
                    const COMPAT_IMAGE *ntdll, const char *command_line,
                    UINT32 *directory);

#endif
