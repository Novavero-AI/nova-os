/* The program loader. It checks every offset it uses against the file
   and the image, as the kernel's loader does (kernel/object/image.c), so
   a malformed file fails the load instead of faulting init. The image is
   laid out in one section through a view of it in init, and that
   section is mapped into the process at the image's base, each run of
   pages with one protection as one mapping. The view stays mapped:
   process_map has no inverse yet. */

#include "loader.h"

#define PE_SIGNATURE 0x00004550 /* "PE\0\0" */
#define PE_MACHINE_AMD64 0x8664
#define PE32_PLUS 0x20b
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
#define OPT_HEADERS_END 64
#define SECTION_HEADER_SIZE 40
#define SECTION_VIRTUAL_SIZE 8
#define SECTION_ADDRESS 12
#define SECTION_RAW_SIZE 16
#define SECTION_RAW_OFFSET 20
#define SECTION_CHARACTERISTICS 36
#define SECTION_EXECUTE 0x20000000 /* IMAGE_SCN_MEM_EXECUTE */
#define SECTION_WRITE 0x80000000   /* IMAGE_SCN_MEM_WRITE */

/* A program's stack: STACK_SIZE bytes ending at STACK_TOP, below the top
   of user memory with 4-level and 5-level paging alike, with RSP below
   the 40 bytes S8 has mapped above it, a return address and a home
   area. Images lie far below it. */
#define STACK_SIZE (16 * PAGE_SIZE)
#define STACK_TOP 0x00007fff00000000ULL
#define STACK_START_DEPTH 40

typedef struct {
    const UINT8 *file;
    const UINT8 *sections;
    UINT32 section_count;
    UINT32 headers_size;
    UINT64 image_size; /* rounded up to whole pages */
    UINT64 base;
    UINT64 entry;
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

static int within(UINT64 offset, UINT64 length, UINT64 limit)
{
    return offset <= limit && length <= limit - offset;
}

static UINT64 round_to_pages(UINT64 size)
{
    return (size + PAGE_SIZE - 1) / PAGE_SIZE * PAGE_SIZE;
}

static const UINT8 *section_at(const PE *pe, UINT32 i)
{
    return pe->sections + (UINT64)i * SECTION_HEADER_SIZE;
}

static UINT32 data_size(const UINT8 *section)
{
    UINT32 virtual_size = read32(section + SECTION_VIRTUAL_SIZE);
    UINT32 raw_size = read32(section + SECTION_RAW_SIZE);

    return raw_size < virtual_size ? raw_size : virtual_size;
}

/* The headers, as boot-protocol.md's First process image lists them:
   PE32+ for x86-64, a page-aligned base with the image below the stack,
   an entry inside the image, sections within the file and the image. */
static UINT32 parse(const UINT8 *file, UINT64 size, PE *pe)
{
    const UINT8 *nt, *opt;
    UINT32 nt_offset, optional_size, entry, i;

    if (size < DOS_PE_OFFSET + 4 || file[0] != 'M' || file[1] != 'Z')
        return ERR_INVALID_ARGS;
    nt_offset = read32(file + DOS_PE_OFFSET);
    if (!within(nt_offset, OPTIONAL_HEADER, size))
        return ERR_INVALID_ARGS;
    nt = file + nt_offset;
    opt = nt + OPTIONAL_HEADER;
    optional_size = read16(nt + COFF_OPTIONAL_SIZE);
    pe->file = file;
    pe->section_count = read16(nt + COFF_SECTIONS);
    if (read32(nt) != PE_SIGNATURE ||
        read16(nt + COFF_MACHINE) != PE_MACHINE_AMD64 ||
        optional_size < OPT_HEADERS_END ||
        !within((UINT64)nt_offset + OPTIONAL_HEADER,
                optional_size +
                    (UINT64)pe->section_count * SECTION_HEADER_SIZE,
                size) ||
        read16(opt) != PE32_PLUS ||
        read32(opt + OPT_SECTION_ALIGNMENT) > PAGE_SIZE)
        return ERR_INVALID_ARGS;
    pe->sections = opt + optional_size;
    pe->base = read64(opt + OPT_IMAGE_BASE);
    pe->image_size = round_to_pages(read32(opt + OPT_IMAGE_SIZE));
    pe->headers_size = read32(opt + OPT_HEADERS_SIZE);
    entry = read32(opt + OPT_ENTRY);
    if (pe->base % PAGE_SIZE != 0 || pe->base < PAGE_SIZE ||
        pe->image_size == 0 ||
        !within(pe->base, pe->image_size, STACK_TOP - STACK_SIZE) ||
        pe->headers_size > size || pe->headers_size > pe->image_size ||
        entry == 0 || entry >= pe->image_size)
        return ERR_INVALID_ARGS;
    pe->entry = pe->base + entry;
    for (i = 0; i < pe->section_count; i++) {
        const UINT8 *section = section_at(pe, i);

        if (!within(read32(section + SECTION_ADDRESS),
                    read32(section + SECTION_VIRTUAL_SIZE), pe->image_size) ||
            !within(read32(section + SECTION_RAW_OFFSET), data_size(section),
                    size))
            return ERR_INVALID_ARGS;
    }
    return OK;
}

/* The protection of the image page at offset start: executable if an
   executable section reaches it, else writable if a writable one does,
   else read-only. A page both would reach is refused, as the kernel
   refuses one in the first process's image. */
static UINT32 page_protection(const PE *pe, UINT64 start)
{
    UINT32 rights = 0, i;

    for (i = 0; i < pe->section_count; i++) {
        const UINT8 *section = section_at(pe, i);
        UINT64 address = read32(section + SECTION_ADDRESS);
        UINT64 size = read32(section + SECTION_VIRTUAL_SIZE);

        if (address < start + PAGE_SIZE && start < address + size)
            rights |= read32(section + SECTION_CHARACTERISTICS);
    }
    if ((rights & SECTION_EXECUTE) && (rights & SECTION_WRITE))
        return 0;
    if (rights & SECTION_EXECUTE)
        return MAP_READ | MAP_EXECUTE;
    if (rights & SECTION_WRITE)
        return MAP_READ | MAP_WRITE;
    return MAP_READ;
}

static void copy(UINT8 *to, const UINT8 *from, UINT64 count)
{
    UINT64 i;

    for (i = 0; i < count; i++)
        to[i] = from[i];
}

static UINT32 map_image(const PE *pe, UINT32 process)
{
    UINT64 start, run;
    UINT32 section, protection, i;
    UINT32 status = section_create(pe->image_size, 0, &section);
    UINT8 *view;

    if (status != OK)
        return status;
    view = map_into_init(section, pe->image_size, MAP_READ | MAP_WRITE);
    if (!view)
        status = ERR_NO_MEMORY;
    if (status == OK) {
        copy(view, pe->file, pe->headers_size);
        for (i = 0; i < pe->section_count; i++) {
            const UINT8 *header = section_at(pe, i);

            copy(view + read32(header + SECTION_ADDRESS),
                 pe->file + read32(header + SECTION_RAW_OFFSET),
                 data_size(header));
        }
    }
    for (start = 0; status == OK && start < pe->image_size; start += run) {
        protection = page_protection(pe, start);
        if (!protection) {
            status = ERR_INVALID_ARGS;
            break;
        }
        for (run = PAGE_SIZE; start + run < pe->image_size &&
                              page_protection(pe, start + run) == protection;
             run += PAGE_SIZE)
            ;
        status = process_map(process, section, start, pe->base + start, run,
                             protection);
    }
    handle_close(section);
    return status;
}

static UINT32 map_stack(UINT32 process)
{
    UINT32 section;
    UINT32 status = section_create(STACK_SIZE, 0, &section);

    if (status != OK)
        return status;
    status = process_map(process, section, 0, STACK_TOP - STACK_SIZE,
                         STACK_SIZE, MAP_READ | MAP_WRITE);
    handle_close(section);
    return status;
}

UINT32 loader_load(UINT32 job, const BOOT_FILE_VIEW *file, PROGRAM *program)
{
    UINT32 status;
    PE pe;

    program->process = HANDLE_INVALID;
    program->thread = HANDLE_INVALID;
    if (!file->data)
        return ERR_INVALID_ARGS;
    status = parse(file->data, file->size, &pe);
    if (status == OK)
        status = process_create(job, 0, &program->process);
    if (status == OK)
        status = map_image(&pe, program->process);
    if (status == OK)
        status = map_stack(program->process);
    if (status == OK)
        status = thread_create(program->process, &program->thread);
    if (status != OK) {
        if (program->process != HANDLE_INVALID)
            handle_close(program->process);
        program->process = HANDLE_INVALID;
        return status;
    }
    program->entry = pe.entry;
    program->stack = STACK_TOP - STACK_START_DEPTH;
    return OK;
}

UINT32 loader_start(PROGRAM *program, UINT32 bootstrap)
{
    UINT32 status = process_start(program->process, program->thread,
                                  program->entry, program->stack, bootstrap,
                                  0);

    handle_close(program->thread);
    program->thread = HANDLE_INVALID;
    return status;
}
