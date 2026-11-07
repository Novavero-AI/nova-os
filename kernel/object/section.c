/* Sections. A memory section's pages come from page_alloc_mapped, so they
   start zeroed, and go back to the allocator when the section is
   destroyed. A device section maps the same physical addresses for its
   whole life and frees none.

   A device section also gives its pages their memory type. Every live
   device section is on a list, and a new one over a page that the
   direct map or a section on the list gives another type is refused, so
   the page keeps the first type until no section over it lives; every
   mapping holds its section, so no mapping outlives the type it was made
   with. RAM is mapped write-back everywhere and no device section covers
   it. */

#include "section.h"
#include "cpu.h"
#include "../acpi/acpi.h"
#include "../mm/memory_map.h"
#include "../mm/page.h"

#define PAGE_LIST_CAPACITY (PAGE_SIZE / sizeof(UINT64))

_Static_assert(SECTION_MAX_SIZE == PAGE_LIST_CAPACITY * PAGE_SIZE,
               "a section's page list fills one page");

/* The range message-signalled interrupts are written to, which the local
   APIC receives (SDM Vol. 3A 13.11.1), and IA32_APIC_BASE, whose bits
   from 12 up give the local APIC's register page (SDM Vol. 3A 13.4.4). */
#define MSI_RANGE_BASE 0xfee00000ULL
#define MSI_RANGE_SIZE 0x100000ULL
#define IA32_APIC_BASE 0x1b
#define APIC_BASE_ADDRESS (~0xfffULL)

/* The memory types a device section may share with the memory map. */
#define DEVICE_TYPES                                                     \
    (MEMORY_TYPE(EFI_MEMORY_MAPPED_IO) |                                 \
     MEMORY_TYPE(EFI_MEMORY_MAPPED_IO_PORT_SPACE))

/* The live device sections, linked through next_device; changed with
   interrupts disabled. */
static SECTION *device_sections;

static void destroy(OBJECT *object)
{
    SECTION *section = (SECTION *)object;
    SECTION **link;
    UINT64 i, flags;

    if (section->kind == SECTION_MEMORY) {
        for (i = 0; i < section->pages; i++)
            page_free_mapped(direct_map(section->physical[i]));
        page_free_mapped(section->physical);
    } else {
        flags = save_and_disable_interrupts();
        for (link = &device_sections; *link != section;
             link = &(*link)->next_device)
            ;
        *link = section->next_device;
        restore_interrupts(flags);
    }
    page_free_mapped(section);
}

static const OBJECT_TYPE section_type = {TYPE_SECTION, 0, destroy, 0, 0};

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
    section->kind = SECTION_MEMORY;
    section->memory_type = USER_WRITE_BACK;
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

static int overlaps(UINT64 base, UINT64 size, UINT64 other, UINT64 other_size)
{
    return other < base + size && base < other + other_size;
}

/* Whether the range reaches memory or registers the kernel keeps (ipc.md
   I6). The configuration space is the one range the memory map may
   describe as reserved: firmware reserves it there so that no OS takes
   it for RAM, and acpi.c accepted it only where it overlaps nothing
   else. */
static int kept_from_sections(UINT64 base, UINT64 size)
{
    UINT64 apic = rdmsr(IA32_APIC_BASE) & APIC_BASE_ADDRESS;

    if (overlaps(base, size, MSI_RANGE_BASE, MSI_RANGE_SIZE) ||
        overlaps(base, size, apic, PAGE_SIZE) ||
        acpi_kernel_registers(base, size))
        return 1;
    if (acpi_ecam_contains(base, size))
        return 0;
    return memory_map_overlaps_other(base, size, DEVICE_TYPES);
}

/* Whether a page of the size bytes at base has a memory type other than
   type, in the direct map or from a live device section. With interrupts
   disabled. */
static int type_differs(UINT64 base, UINT64 size, USER_MEMORY_TYPE type)
{
    const SECTION *other;
    UINT64 page;

    for (other = device_sections; other; other = other->next_device)
        if (other->memory_type != type &&
            overlaps(base, size, other->base, other->pages * PAGE_SIZE))
            return 1;
    for (page = base; page - base < size; page += PAGE_SIZE)
        if (paging_direct_type_differs(page, type))
            return 1;
    return 0;
}

/* The type's check and the section's joining the list run with
   interrupts disabled, so two sections made at once cannot both pass. */
STATUS section_create_device(const RESOURCE *resource, UINT64 base,
                             UINT64 size, UINT32 cache, SECTION **out)
{
    SECTION *section;
    UINT64 flags;

    if (size == 0 || base % PAGE_SIZE != 0 || size % PAGE_SIZE != 0 ||
        base + size < base ||
        (cache != CACHE_UNCACHED && cache != CACHE_WRITE_COMBINING))
        return ERR_INVALID_ARGS;
    if (size > SECTION_MAX_SIZE ||
        !resource_covers(resource, RESOURCE_MMIO, base, size))
        return ERR_OUT_OF_RANGE;
    if (kept_from_sections(base, size))
        return ERR_ACCESS_DENIED;
    section = page_alloc_mapped();
    if (!section)
        return ERR_NO_MEMORY;
    section->kind = SECTION_DEVICE;
    section->memory_type =
        cache == CACHE_UNCACHED ? USER_UNCACHED : USER_WRITE_COMBINING;
    section->pages = size / PAGE_SIZE;
    section->physical = 0;
    section->base = base;
    flags = save_and_disable_interrupts();
    if (type_differs(base, size, section->memory_type)) {
        restore_interrupts(flags);
        page_free_mapped(section);
        return ERR_BAD_STATE;
    }
    section->next_device = device_sections;
    device_sections = section;
    restore_interrupts(flags);
    object_init(&section->object, &section_type);
    *out = section;
    return OK;
}

UINT64 section_page(const SECTION *section, UINT64 index)
{
    if (section->kind == SECTION_DEVICE)
        return section->base + index * PAGE_SIZE;
    return section->physical[index];
}

void section_fill(SECTION *section, const UINT8 *data, UINT64 size)
{
    UINT64 i;

    for (i = 0; i < size; i++)
        ((UINT8 *)direct_map(section->physical[i / PAGE_SIZE]))[i % PAGE_SIZE] =
            data[i];
}
