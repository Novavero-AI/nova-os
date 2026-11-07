/* PE32+ images for user mode (PE Format). The image is placed at its own
   ImageBase, so it needs no relocation, and every offset and size the
   file gives is checked against the file or the image before it is
   used. */

#include "image.h"
#include "memory_layout.h"
#include "../mm/page.h"
#include "../x86_64/paging.h"
#include "../x86_64/user.h"

#define PE_SIGNATURE 0x00004550 /* "PE\0\0" */
#define PE_MACHINE_AMD64 0x8664
#define PE32_PLUS 0x20b
#define PE_IMPORT_DIRECTORY 1

/* Field offsets: the PE signature's offset in the MS-DOS header; the COFF
   header's from the signature; the PE32+ optional header's, which follows
   the 20-byte COFF header; a section header's. */
#define DOS_PE_OFFSET 0x3c
#define COFF_MACHINE 4
#define COFF_SECTIONS 6
#define COFF_OPTIONAL_SIZE 20
#define OPTIONAL_HEADER 24
#define OPT_ENTRY 16
#define OPT_IMAGE_BASE 24
#define OPT_SECTION_ALIGNMENT 32
#define OPT_IMAGE_SIZE 56
#define OPT_HEADERS_SIZE 60
#define OPT_DIRECTORY_COUNT 108
#define OPT_DIRECTORIES 112
#define DIRECTORY_SIZE 8
#define SECTION_HEADER_SIZE 40
#define SECTION_VIRTUAL_SIZE 8
#define SECTION_ADDRESS 12
#define SECTION_RAW_SIZE 16
#define SECTION_RAW_OFFSET 20
#define SECTION_CHARACTERISTICS 36
#define SECTION_EXECUTE 0x20000000 /* IMAGE_SCN_MEM_EXECUTE */
#define SECTION_WRITE 0x80000000   /* IMAGE_SCN_MEM_WRITE */

typedef struct {
    const UINT8 *file;
    UINT64 file_size;
    const UINT8 *sections;
    UINT32 section_count;
    UINT32 headers_size;
    UINT32 image_size;
} PE;

static UINT32 read16(const UINT8 *p) { return p[0] | p[1] << 8; }

static UINT32 read32(const UINT8 *p)
{
    return (UINT32)p[0] | (UINT32)p[1] << 8 | (UINT32)p[2] << 16 |
           (UINT32)p[3] << 24;
}

static UINT64 read64(const UINT8 *p)
{
    return read32(p) | (UINT64)read32(p + 4) << 32;
}

/* Whether length bytes at offset lie within limit bytes, without the sum
   overflowing. */
static int within(UINT64 offset, UINT64 length, UINT64 limit)
{
    return offset <= limit && length <= limit - offset;
}

static const UINT8 *section_at(const PE *pe, UINT32 i)
{
    return pe->sections + (UINT64)i * SECTION_HEADER_SIZE;
}

static UINT32 section_data_size(const UINT8 *section)
{
    UINT32 virtual_size = read32(section + SECTION_VIRTUAL_SIZE);
    UINT32 raw_size = read32(section + SECTION_RAW_SIZE);

    return raw_size < virtual_size ? raw_size : virtual_size;
}

/* The union of the characteristics of the sections that reach the page
   at offset start of the image. */
static UINT32 page_rights(const PE *pe, UINT64 start)
{
    UINT32 rights = 0, i;

    for (i = 0; i < pe->section_count; i++) {
        const UINT8 *section = section_at(pe, i);
        UINT64 address = read32(section + SECTION_ADDRESS);
        UINT64 size = read32(section + SECTION_VIRTUAL_SIZE);

        if (address < start + PAGE_SIZE && start < address + size)
            rights |= read32(section + SECTION_CHARACTERISTICS);
    }
    return rights;
}

/* No page may be both writable and executable (docs/spec/objects.md O5),
   so a page that a writable section and an executable one share refuses
   the image. */
static int writable_and_executable(const PE *pe)
{
    UINT64 start;

    for (start = 0; start < pe->image_size; start += PAGE_SIZE) {
        UINT32 rights = page_rights(pe, start);

        if ((rights & SECTION_WRITE) && (rights & SECTION_EXECUTE))
            return 1;
    }
    return 0;
}

/* Check the headers and section table, and that the image lies in user
   memory below limit at its ImageBase, and fill in pe and image. */
static STATUS parse(const UINT8 *file, UINT64 size, UINT64 limit, PE *pe,
                    IMAGE *image)
{
    const UINT8 *nt, *opt;
    UINT32 nt_offset, optional_size, entry, i;
    UINT64 base, sections_end;

    if (size < DOS_PE_OFFSET + 4 || file[0] != 'M' || file[1] != 'Z')
        return ERR_INVALID_ARGS;
    nt_offset = read32(file + DOS_PE_OFFSET);
    if (!within(nt_offset, OPTIONAL_HEADER, size))
        return ERR_INVALID_ARGS;
    nt = file + nt_offset;
    opt = nt + OPTIONAL_HEADER;
    pe->file = file;
    pe->file_size = size;
    pe->section_count = read16(nt + COFF_SECTIONS);
    optional_size = read16(nt + COFF_OPTIONAL_SIZE);
    if (read32(nt) != PE_SIGNATURE ||
        read16(nt + COFF_MACHINE) != PE_MACHINE_AMD64 ||
        optional_size < OPT_DIRECTORIES ||
        !within((UINT64)nt_offset + OPTIONAL_HEADER,
                optional_size +
                    (UINT64)pe->section_count * SECTION_HEADER_SIZE,
                size) ||
        read16(opt) != PE32_PLUS ||
        read32(opt + OPT_SECTION_ALIGNMENT) > PAGE_SIZE)
        return ERR_INVALID_ARGS;
    /* The kernel resolves no imports, so an image with an import
       directory is refused. */
    if (read32(opt + OPT_DIRECTORY_COUNT) > PE_IMPORT_DIRECTORY) {
        UINT32 slot = OPT_DIRECTORIES + PE_IMPORT_DIRECTORY * DIRECTORY_SIZE;

        if (optional_size < slot + DIRECTORY_SIZE ||
            read32(opt + slot + 4) != 0)
            return ERR_INVALID_ARGS;
    }
    pe->sections = opt + optional_size;
    pe->image_size = read32(opt + OPT_IMAGE_SIZE);
    pe->headers_size = read32(opt + OPT_HEADERS_SIZE);
    sections_end = (UINT64)(pe->sections - file) +
                   (UINT64)pe->section_count * SECTION_HEADER_SIZE;
    base = read64(opt + OPT_IMAGE_BASE);
    entry = read32(opt + OPT_ENTRY);
    if (pe->headers_size > pe->image_size || pe->headers_size > size ||
        sections_end > pe->headers_size || entry == 0 ||
        entry >= pe->image_size || base % PAGE_SIZE != 0 ||
        base < USER_BASE || !within(base, pe->image_size, limit))
        return ERR_INVALID_ARGS;
    for (i = 0; i < pe->section_count; i++) {
        const UINT8 *section = section_at(pe, i);

        if (!within(read32(section + SECTION_ADDRESS),
                    read32(section + SECTION_VIRTUAL_SIZE), pe->image_size) ||
            !within(read32(section + SECTION_RAW_OFFSET),
                    section_data_size(section), size))
            return ERR_INVALID_ARGS;
    }
    if (writable_and_executable(pe))
        return ERR_INVALID_ARGS;
    image->base = base;
    image->end = base + (pe->image_size + PAGE_SIZE - 1) / PAGE_SIZE * PAGE_SIZE;
    image->entry = base + entry;
    return OK;
}

/* Copy into page what of the count bytes at source, which belong at
   image offset at, falls in the page at image offset start. */
static void copy_part(UINT8 *page, UINT64 start, const UINT8 *source,
                      UINT64 at, UINT64 count)
{
    UINT64 from = at > start ? at : start;
    UINT64 end = at + count < start + PAGE_SIZE ? at + count : start + PAGE_SIZE;

    for (; from < end; from++)
        page[from - start] = source[from - at];
}

static USER_PROTECTION protection_of(UINT32 rights)
{
    if (rights & SECTION_EXECUTE)
        return USER_READ_EXECUTE;
    if (rights & SECTION_WRITE)
        return USER_READ_WRITE;
    return USER_READ;
}

/* A fresh page holding the headers and section data that fall in the
   page at image offset start, zero elsewhere, as the loader for the
   kernel image lays it out (docs/spec/boot-protocol.md, Placement). */
static STATUS map_page(UINT64 space, const PE *pe, const IMAGE *image,
                       UINT64 start)
{
    UINT8 *page = page_alloc_mapped();
    UINT32 i;

    if (!page)
        return ERR_NO_MEMORY;
    copy_part(page, start, pe->file, 0, pe->headers_size);
    for (i = 0; i < pe->section_count; i++) {
        const UINT8 *section = section_at(pe, i);

        copy_part(page, start,
                  pe->file + read32(section + SECTION_RAW_OFFSET),
                  read32(section + SECTION_ADDRESS),
                  section_data_size(section));
    }
    if (!paging_map_user(space, image->base + start,
                         (UINT64)page - DIRECT_MAP_BASE,
                         protection_of(page_rights(pe, start)))) {
        page_free_mapped(page);
        return ERR_NO_MEMORY;
    }
    return OK;
}

STATUS image_load(UINT64 space, const UINT8 *file, UINT64 size, UINT64 limit,
                  IMAGE *image)
{
    PE pe;
    STATUS status = parse(file, size, limit, &pe, image);
    UINT64 start;

    for (start = 0; status == OK && start < pe.image_size; start += PAGE_SIZE)
        status = map_page(space, &pe, image, start);
    return status;
}
