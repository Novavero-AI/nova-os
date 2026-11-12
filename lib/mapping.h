/** @file
    A program's mappings of memory into its own address space: new
    memory, a section it holds, and the framebuffer of a display it is
    given (docs/spec/console.md C1), each at the next free address, and
    the images given one after another for one place, each in a range of
    addresses kept for them (docs/spec/display.md D6). */

#ifndef NOVA_MAPPING_H
#define NOVA_MAPPING_H

#include "gfx.h"
#include "native_abi.h"

/** Where a program maps: its own process, with PROCESS_RIGHT_MAP, and
    the address the next mapping goes at, page-aligned. It only moves
    up: mapping_section, mapping_memory, mapping_image and
    mapping_display move it past what they map there, mapping_memory
    also past the sections it had mapped where it fails part way, and
    mapping_range_reserve moves it past the range it keeps. Nothing
    else may be mapped from there up. */
typedef struct {
    UINT32 self; /**< the program's own process */
    UINT64 next; /**< where the next mapping goes */
} MAPPING;

/** The addresses a range keeps: the most an image holds, since a message
    carries at most CHANNEL_MAX_HANDLES sections, of SECTION_MAX_SIZE
    each. */
#define MAPPING_RANGE_BYTES ((UINT64)CHANNEL_MAX_HANDLES * SECTION_MAX_SIZE)

/** A range of addresses kept for one image at a time
    (mapping_range_reserve): MAPPING_RANGE_BYTES from base, where each
    image given for it is mapped in place of the one before, so that
    the images of one place take these addresses and no others, however
    many are given. */
typedef struct {
    UINT64 base;   /**< where the range, and each image in it, starts */
    UINT64 mapped; /**< the bytes of the image mapped there, in whole
                        pages; 0 while the range holds none */
} MAPPING_RANGE;

/** Map size bytes of section, from its start, with MAP_ protection.
    Returns where they lie, or 0 with the status of the call that failed
    in *status. The section's handle stays the caller's. */
void *mapping_section(MAPPING *mapping, UINT32 section, UINT64 size,
                      UINT32 protection, UINT32 *status);

/** Remove the size bytes mapped at address, which mapping_section,
    mapping_memory or mapping_image mapped, rounded up to whole pages,
    as each rounds what it maps. Their addresses are not mapped again;
    only a range's are, and what a range holds is removed with
    mapping_range_clear. Returns process_unmap's status. */
UINT32 mapping_unmap(MAPPING *mapping, const void *address, UINT64 size);

/** Map size bytes of new zeroed memory, read-write, in sections of at
    most SECTION_MAX_SIZE. Returns where they lie, or 0 with the status
    of the call that failed in *status and nothing of them left
    mapped. */
void *mapping_memory(MAPPING *mapping, UINT64 size, UINT32 *status);

/** Map the count sections of an image of bytes bytes, as the kernel hands
    a background on (native_abi.h): bands of SECTION_MAX_SIZE bytes but
    the last, read one after another, row after row. Returns where the
    image starts; or 0, with nothing left mapped, mapping->next where it
    was and the status of the call that failed in *status. The handles
    stay the caller's. */
const void *mapping_image(MAPPING *mapping, const RECEIVED_HANDLE *handles,
                          UINT32 count, UINT64 bytes, UINT32 *status);

/** Keep the MAPPING_RANGE_BYTES of addresses from mapping->next for
    range, which holds no image, and move mapping->next past them. It
    maps nothing and makes no call. */
void mapping_range_reserve(MAPPING *mapping, MAPPING_RANGE *range);

/** Map an image at range's base in place of the one the range holds:
    the count sections, at least one and at most CHANNEL_MAX_HANDLES, of
    an image of bytes bytes, as mapping_image takes them, read only.
    The image the range holds is unmapped first, so that the two are
    never mapped together and mapping->next stays where it is. Returns
    where the image starts, the range's base; or 0 with the status of
    the call that failed in *status. If it was the mapping of a
    section, nothing of either image is left mapped and the range holds
    none; if it was the unmapping, the range holds the image it held,
    still mapped, and nothing of the new one is. The handles stay the
    caller's. */
const void *mapping_range_image(const MAPPING *mapping, MAPPING_RANGE *range,
                                const RECEIVED_HANDLE *handles, UINT32 count,
                                UINT64 bytes, UINT32 *status);

/** Unmap the image range holds, if it holds one, leaving the range's
    addresses kept for the next. Returns OK; or process_unmap's status,
    with the image still mapped and the range still holding it. */
UINT32 mapping_range_clear(const MAPPING *mapping, MAPPING_RANGE *range);

/** How many bytes of section it holds from its start, up to limit, in
    whole pages: no call reports a section's size, so this maps ranges of
    it read-only, each unmapped at once, and is told by process_map's
    refusal of a range past its end, where mapping->next lies and nothing
    else is mapped. A section of limit bytes or more gives limit rounded
    down to a page. Returns the bytes, or 0 with the status of the call
    that failed in *status, which is OK for a section of no whole page. */
UINT64 mapping_section_bytes(MAPPING *mapping, UINT32 section, UINT64 limit,
                             UINT32 *status);

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
