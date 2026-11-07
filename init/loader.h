/* Starting a program from a PE32+ image, a file the boot volume or the
   store holds: a new process holding a copy of the image laid out as the
   kernel lays out the first process (docs/spec/boot-protocol.md, First
   process image), each page with the rights of the sections on it, and a
   stack below the top of user memory. */

#ifndef NOVA_LOADER_H
#define NOVA_LOADER_H

#include "init.h"

/* A program laid out in a new process, not yet started. */
typedef struct {
    UINT32 process;
    UINT32 thread;
    UINT64 entry;
    UINT64 stack;
} PROGRAM;

/* Create a process in job and lay out in it the size bytes at image.
   Returns a status; ERR_INVALID_ARGS if they are not a PE32+ image this
   loader places. On failure nothing is left open. */
UINT32 loader_load_image(UINT32 job, const UINT8 *image, UINT64 size,
                         PROGRAM *program);

/* loader_load_image of a boot file's bytes. */
UINT32 loader_load(UINT32 job, const BOOT_FILE_VIEW *file, PROGRAM *program);

/* Start program at its entry with bootstrap, which moves into the
   process. Closes the thread's handle; the caller keeps the process's. */
UINT32 loader_start(PROGRAM *program, UINT32 bootstrap);

#endif
