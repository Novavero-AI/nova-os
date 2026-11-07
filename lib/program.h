/** @file
    Starting a program from a PE32+ image, a file the boot volume or the
    store holds: a new process holding a copy of the image laid out as the
    kernel lays out the first process (docs/spec/boot-protocol.md, First
    process image), each page with the rights of the sections on it, and a
    stack below the top of user memory. The launcher, the first process or
    login, lays the image out through a window of its own address space,
    which it names and which the load leaves unmapped. */

#ifndef NOVA_PROGRAM_H
#define NOVA_PROGRAM_H

#include "native_abi.h"

/** A program laid out in a new process, not yet started. */
typedef struct {
    UINT32 process; /**< the process's handle */
    UINT32 thread;  /**< its first thread's handle */
    UINT64 entry;   /**< the image's entry point */
    UINT64 stack;   /**< the stack pointer the thread starts with */
} PROGRAM;

/** Where the launcher lays an image out: self, its own process with
    PROCESS_RIGHT_MAP, and window_size bytes at window, page-aligned and
    unmapped, which bound the largest image it lays out. */
typedef struct {
    UINT32 self;        /**< the launcher's process, with MAP */
    UINT64 window;      /**< the window's address */
    UINT64 window_size; /**< the window's bytes */
} LAYOUT_WINDOW;

/** Create a process in job and lay out in it the size bytes at image.
    Returns a status; ERR_INVALID_ARGS if they are not a PE32+ image this
    loader places, ERR_OUT_OF_RANGE if the image does not fit the window.
    On failure nothing is left open. */
UINT32 program_load(const LAYOUT_WINDOW *layout, UINT32 job,
                    const UINT8 *image, UINT64 size, PROGRAM *program);

/** Start program at its entry with bootstrap, which moves into the
    process. Closes the thread's handle; the caller keeps the process's. */
UINT32 program_start(PROGRAM *program, UINT32 bootstrap);

#endif
