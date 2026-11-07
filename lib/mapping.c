/* A program's mappings (mapping.h): sections mapped one after another
   from the address it chose, each a whole number of pages. */

#include "mapping.h"
#include "limb.h"
#include "native.h"

#define PIXEL_BYTES 4

static UINT64 whole_pages(UINT64 size)
{
    return (size + PAGE_SIZE - 1) / PAGE_SIZE * PAGE_SIZE;
}

void *mapping_section(MAPPING *mapping, UINT32 section, UINT64 size,
                      UINT32 protection, UINT32 *status)
{
    UINT64 at = mapping->next;

    size = whole_pages(size);
    *status = process_map(mapping->self, section, 0, at, size, protection);
    if (*status != OK)
        return 0;
    mapping->next += size;
    return (void *)at;
}

void *mapping_memory(MAPPING *mapping, UINT64 size, UINT32 *status)
{
    UINT64 start = mapping->next, offset, piece;
    UINT32 section;

    for (offset = 0; offset < size; offset += piece) {
        piece = size - offset < SECTION_MAX_SIZE ? size - offset
                                                 : SECTION_MAX_SIZE;
        piece = whole_pages(piece);
        *status = section_create(piece, 0, &section);
        if (*status != OK)
            return 0;
        mapping_section(mapping, section, piece, MAP_READ | MAP_WRITE,
                        status);
        handle_close(section);
        if (*status != OK)
            return 0;
    }
    return (void *)start;
}

/* Whether display describes a screen drawn on: a pixel format known, a
   side Limb draws, rows no shorter than the screen is wide, and every
   pixel within the address space. */
static int drawable(const BOOTSTRAP_DISPLAY *d)
{
    return (d->format == PIXEL_RGB || d->format == PIXEL_BGR) &&
           d->width >= LIMB_MIN_SIDE && d->height >= LIMB_MIN_SIDE &&
           d->width <= LIMB_MAX_SIDE && d->height <= LIMB_MAX_SIDE &&
           d->stride >= d->width && d->stride <= LIMB_MAX_SIDE &&
           d->base % PIXEL_BYTES == 0 &&
           d->base <= ~0ULL - (UINT64)d->stride * d->height * PIXEL_BYTES -
                          PAGE_SIZE;
}

UINT32 mapping_display(MAPPING *mapping, UINT32 resource,
                       const BOOTSTRAP_DISPLAY *display, SCREEN *screen)
{
    UINT64 first = display->base - display->base % PAGE_SIZE;
    UINT64 end = whole_pages(display->base + (UINT64)display->stride *
                                                 display->height * PIXEL_BYTES);
    UINT64 start = mapping->next, offset, piece;
    UINT32 section, status;

    if (!drawable(display))
        return ERR_NOT_SUPPORTED;
    for (offset = first; offset < end; offset += piece) {
        piece = end - offset < SECTION_MAX_SIZE ? end - offset
                                                : SECTION_MAX_SIZE;
        status = section_create_physical(resource, offset, piece,
                                         CACHE_WRITE_COMBINING, &section);
        if (status != OK)
            return status;
        mapping_section(mapping, section, piece, MAP_READ | MAP_WRITE,
                        &status);
        handle_close(section);
        if (status != OK)
            return status;
    }
    screen->pixels = (UINT32 *)(start + (display->base - first));
    screen->width = display->width;
    screen->height = display->height;
    screen->stride = display->stride;
    screen->format = display->format;
    return OK;
}
