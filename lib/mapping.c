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

UINT32 mapping_unmap(MAPPING *mapping, const void *address, UINT64 size)
{
    return process_unmap(mapping->self, (UINT64)address, whole_pages(size));
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
        if (*status == OK) {
            mapping_section(mapping, section, piece, MAP_READ | MAP_WRITE,
                            status);
            handle_close(section);
        }
        if (*status != OK) {
            if (offset)
                mapping_unmap(mapping, (const void *)start, offset);
            return 0;
        }
    }
    return (void *)start;
}

/* Whether section holds bytes from its start, mapped and unmapped at
   mapping->next: *holds is 1 if so and 0 if process_map refuses the range
   as past the section's end. Returns OK, or the status of a call that
   failed otherwise. */
static UINT32 section_holds(const MAPPING *mapping, UINT32 section,
                            UINT64 bytes, int *holds)
{
    UINT32 status = process_map(mapping->self, section, 0, mapping->next, bytes,
                                MAP_READ);

    *holds = status == OK;
    if (status == OK)
        return process_unmap(mapping->self, mapping->next, bytes);
    return status == ERR_OUT_OF_RANGE ? OK : status;
}

UINT64 mapping_section_bytes(MAPPING *mapping, UINT32 section, UINT64 limit,
                             UINT32 *status)
{
    /* The pages known to be held, and the least known not to be, or
       the most asked about while that is not known. */
    UINT64 held = 0, past = limit / PAGE_SIZE;
    int holds;

    *status = OK;
    if (past == 0)
        return 0;
    *status = section_holds(mapping, section, past * PAGE_SIZE, &holds);
    if (*status != OK)
        return 0;
    if (holds)
        return past * PAGE_SIZE;
    while (past - held > 1) {
        UINT64 middle = held + (past - held) / 2;

        *status = section_holds(mapping, section, middle * PAGE_SIZE, &holds);
        if (*status != OK)
            return 0;
        if (holds)
            held = middle;
        else
            past = middle;
    }
    return held * PAGE_SIZE;
}

/* The bytes of band index of an image of bytes bytes. */
static UINT64 band_size(UINT64 bytes, UINT32 index)
{
    UINT64 rest = bytes - (UINT64)index * SECTION_MAX_SIZE;

    return rest < SECTION_MAX_SIZE ? rest : SECTION_MAX_SIZE;
}

const void *mapping_image(MAPPING *mapping, const RECEIVED_HANDLE *handles,
                          UINT32 count, UINT64 bytes, UINT64 *mapped,
                          UINT32 *status)
{
    const UINT8 *start = 0;
    UINT32 i;

    *mapped = 0;
    for (i = 0; i < count; i++) {
        const UINT8 *band = mapping_section(mapping, handles[i].handle,
                                            band_size(bytes, i), MAP_READ,
                                            status);

        if (!band) {
            if (start)
                mapping_unmap(mapping, start, *mapped);
            return 0;
        }
        if (!start)
            start = band;
        *mapped = (UINT64)(mapping->next - (UINT64)start);
    }
    return start;
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
