/** @file
    A program's mappings of memory into its own address space: new
    memory, a section it holds, and the framebuffer of a display it is
    given (docs/spec/console.md C1), each at the next free address. */

#ifndef NOVA_MAPPING_H
#define NOVA_MAPPING_H

#include "gfx.h"
#include "native_abi.h"

/** Where a program maps: its own process, with PROCESS_RIGHT_MAP, and
    the address the next mapping goes at, page-aligned, which each call
    moves past what it maps. Nothing else may be mapped from there up. */
typedef struct {
    UINT32 self; /**< the program's own process */
    UINT64 next; /**< where the next mapping goes */
} MAPPING;

/** Map size bytes of section, from its start, with MAP_ protection.
    Returns where they lie, or 0 with the status of the call that failed
    in *status. The section's handle stays the caller's. */
void *mapping_section(MAPPING *mapping, UINT32 section, UINT64 size,
                      UINT32 protection, UINT32 *status);

/** Remove the size bytes mapped at address, which a call here mapped,
    rounded up to whole pages, as each call rounds what it maps. The
    address range is not mapped again. Returns process_unmap's status. */
UINT32 mapping_unmap(MAPPING *mapping, const void *address, UINT64 size);

/** Map size bytes of new zeroed memory, read-write, in sections of at
    most SECTION_MAX_SIZE. Returns where they lie, or 0 with the status
    of the call that failed in *status and nothing of them left
    mapped. */
void *mapping_memory(MAPPING *mapping, UINT64 size, UINT32 *status);

/** Map display's framebuffer, write-combining, through resource, an
    MMIO resource covering its pages, and describe it in *screen.
    Returns OK; ERR_NOT_SUPPORTED, mapping nothing, for a display that
    is not drawn on: a pixel format other than PIXEL_RGB or PIXEL_BGR, a
    side Limb does not draw (limb.h), rows shorter than the screen is
    wide, or a framebuffer not within the address space; or the status
    of the call that failed, with what was mapped before it still
    mapped. The resource's handle stays the caller's. */
UINT32 mapping_display(MAPPING *mapping, UINT32 resource,
                       const BOOTSTRAP_DISPLAY *display, SCREEN *screen);

#endif
