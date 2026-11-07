/* Sections. Each page comes from page_alloc_mapped, so it starts zeroed,
   and goes back to the allocator when the section is destroyed. */

#include "section.h"
#include "../mm/page.h"

#define PAGE_LIST_CAPACITY (PAGE_SIZE / sizeof(UINT64))

_Static_assert(SECTION_MAX_SIZE == PAGE_LIST_CAPACITY * PAGE_SIZE,
               "a section's page list fills one page");

static void destroy(OBJECT *object)
{
    SECTION *section = (SECTION *)object;
    UINT64 i;

    for (i = 0; i < section->pages; i++)
        page_free_mapped(direct_map(section->physical[i]));
    page_free_mapped(section->physical);
    page_free_mapped(section);
}

static const OBJECT_TYPE section_type = {TYPE_SECTION, 0, destroy, 0};

STATUS section_create(UINT64 size, SECTION **out)
{
    SECTION *section;
    UINT64 pages, i;

    if (size == 0)
        return ERR_INVALID_ARGS;
    if (size > SECTION_MAX_SIZE)
        return ERR_OUT_OF_RANGE;
    pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    section = page_alloc_mapped();
    if (!section)
        return ERR_NO_MEMORY;
    section->physical = page_alloc_mapped();
    if (!section->physical) {
        page_free_mapped(section);
        return ERR_NO_MEMORY;
    }
    for (i = 0; i < pages; i++) {
        UINT8 *page = page_alloc_mapped();

        if (!page)
            break;
        section->physical[i] = (UINT64)page - DIRECT_MAP_BASE;
    }
    section->pages = i;
    object_init(&section->object, &section_type);
    if (i < pages) {
        object_close(&section->object);
        return ERR_NO_MEMORY;
    }
    *out = section;
    return OK;
}

void section_fill(SECTION *section, const UINT8 *data, UINT64 size)
{
    UINT64 i;

    for (i = 0; i < size; i++)
        ((UINT8 *)direct_map(section->physical[i / PAGE_SIZE]))[i % PAGE_SIZE] =
            data[i];
}
