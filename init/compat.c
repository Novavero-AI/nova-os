/* Starting a 32-bit program (compat.h). Each image is checked against
   its file and the 32-bit address space before any of it is copied, as
   the kernel checks init's (docs/spec/boot-protocol.md, First process
   image), so a malformed file fails the start instead of faulting init.

   What init writes for a process, the headers and section data of both
   images and the environment block, goes into the staging section,
   which init maps once and each process it starts maps in part; init
   zeroes what the last process was given before filling it again. The
   rest of an image, the zero-filled memory past its data, and the heap
   and the stack come from fresh sections only the process maps, so they
   are freed when it ends. A seed chain program's image declares up to
   128 MiB of such memory and a 64 MiB stack (observed in the chain),
   which init never maps into itself: a fresh section is zeros already.
   The kernel maps no page on demand, so all of it is memory from the
   start. */

#include <stddef.h>

#include "compat.h"
#include "compat_abi.h"
#include "nt32.h"

#define PAGES(n) (((n) + PAGE_SIZE - 1) / PAGE_SIZE * PAGE_SIZE)

/* PE Format: the PE signature's offset at 0x3C, the COFF header after
   the signature, the PE32 optional header after the 20-byte COFF header,
   its fields, and the section table's. */
#define PE_SIGNATURE 0x00004550 /* "PE\0\0" */
#define PE_MACHINE_I386 0x14c
#define PE32_MAGIC 0x10b
#define DOS_PE_OFFSET 0x3c
#define COFF_MACHINE 4
#define COFF_SECTIONS 6
#define COFF_OPTIONAL_SIZE 20
#define OPTIONAL_HEADER 24
#define OPT_ENTRY 16
#define OPT_IMAGE_BASE 28
#define OPT_SECTION_ALIGNMENT 32
#define OPT_IMAGE_SIZE 56
#define OPT_HEADERS_SIZE 60
#define OPT_STACK_RESERVE 72
#define OPT_DIRECTORY_COUNT 92
#define OPT_DIRECTORIES 96
#define DIRECTORY_SIZE 8
#define IMPORT_DIRECTORY 1
#define SECTION_HEADER_SIZE 40
#define SECTION_VIRTUAL_SIZE 8
#define SECTION_ADDRESS 12
#define SECTION_RAW_SIZE 16
#define SECTION_RAW_OFFSET 20
#define SECTION_CHARACTERISTICS 36
#define SECTION_EXECUTE 0x20000000 /* IMAGE_SCN_MEM_EXECUTE */
#define SECTION_WRITE 0x80000000   /* IMAGE_SCN_MEM_WRITE */

#define ADDRESS_LIMIT_32 0x100000000ULL
#define USER_BASE_32 0x10000ULL

/* The process's current directory, the root of its view's drive
   (docs/spec/compat.md C3). */
#define CURRENT_DIRECTORY "C:\\"

/* The view's directory channel queues one open at a time. */
#define DIRECTORY_CAPACITY 1

/* The staging section holds what one process is given: the most one
   section holds. */
#define STAGING_SIZE SECTION_MAX_SIZE

#define LAYOUT_ALIGNMENT 8

/* An image's headers as parse found them. */
typedef struct {
    const UINT8 *sections;
    UINT32 section_count;
    UINT32 headers;    /* SizeOfHeaders */
    UINT64 data_end;   /* the end of the headers and section data, in pages */
    UINT64 image_size; /* SizeOfImage, in pages */
    UINT64 stack;      /* SizeOfStackReserve, in pages */
    UINT32 base;
    UINT32 entry; /* AddressOfEntryPoint */
} PE32;

/* An image as placed in a process: where it lies, its entry, and the
   stack it asks for. */
typedef struct {
    UINT32 base;
    UINT64 end;
    UINT32 entry;
    UINT64 stack;
} IMAGE32;

static UINT32 staging_section;
static UINT8 *staging;
static UINT64 staging_used;

/* ------------------------------------------------------------------------
   PE32 images. */

static UINT32 read16(const UINT8 *p) { return p[0] | p[1] << 8; }

static UINT32 read32(const UINT8 *p)
{
    return (UINT32)p[0] | (UINT32)p[1] << 8 | (UINT32)p[2] << 16 |
           (UINT32)p[3] << 24;
}

static int within(UINT64 offset, UINT64 length, UINT64 limit)
{
    return offset <= limit && length <= limit - offset;
}

static const UINT8 *section_at(const PE32 *pe, UINT32 i)
{
    return pe->sections + (UINT64)i * SECTION_HEADER_SIZE;
}

/* The bytes of a section's data in the file: no more than its virtual
   size, the rest of which is zero. */
static UINT32 data_size(const UINT8 *section)
{
    UINT32 virtual_size = read32(section + SECTION_VIRTUAL_SIZE);
    UINT32 raw_size = read32(section + SECTION_RAW_SIZE);

    return raw_size < virtual_size ? raw_size : virtual_size;
}

/* The headers of the size bytes at file: an i386 PE32 with no imports,
   a page-aligned ImageBase with the whole image in [USER_BASE, 4 GiB),
   an entry inside the image, and headers, section table and sections
   within the file and the image. */
static int parse(const UINT8 *file, UINT64 size, PE32 *pe)
{
    const UINT8 *nt, *opt;
    UINT32 nt_offset, optional_size, image_size, i;
    UINT64 end;

    if (size < DOS_PE_OFFSET + 4 || file[0] != 'M' || file[1] != 'Z')
        return 0;
    nt_offset = read32(file + DOS_PE_OFFSET);
    if (!within(nt_offset, OPTIONAL_HEADER, size))
        return 0;
    nt = file + nt_offset;
    opt = nt + OPTIONAL_HEADER;
    pe->section_count = read16(nt + COFF_SECTIONS);
    optional_size = read16(nt + COFF_OPTIONAL_SIZE);
    if (read32(nt) != PE_SIGNATURE ||
        read16(nt + COFF_MACHINE) != PE_MACHINE_I386 ||
        optional_size < OPT_DIRECTORIES ||
        !within((UINT64)nt_offset + OPTIONAL_HEADER,
                optional_size +
                    (UINT64)pe->section_count * SECTION_HEADER_SIZE,
                size) ||
        read16(opt) != PE32_MAGIC ||
        read32(opt + OPT_SECTION_ALIGNMENT) > PAGE_SIZE)
        return 0;
    if (read32(opt + OPT_DIRECTORY_COUNT) > IMPORT_DIRECTORY) {
        UINT32 slot = OPT_DIRECTORIES + IMPORT_DIRECTORY * DIRECTORY_SIZE;

        if (optional_size < slot + DIRECTORY_SIZE ||
            read32(opt + slot + 4) != 0)
            return 0;
    }
    pe->sections = opt + optional_size;
    pe->base = read32(opt + OPT_IMAGE_BASE);
    image_size = read32(opt + OPT_IMAGE_SIZE);
    pe->image_size = PAGES((UINT64)image_size);
    pe->headers = read32(opt + OPT_HEADERS_SIZE);
    pe->entry = read32(opt + OPT_ENTRY);
    pe->stack = PAGES((UINT64)read32(opt + OPT_STACK_RESERVE));
    if (pe->headers > image_size || pe->headers > size ||
        (UINT64)(pe->sections - file) +
                (UINT64)pe->section_count * SECTION_HEADER_SIZE >
            pe->headers ||
        pe->entry == 0 || pe->entry >= image_size ||
        pe->base % PAGE_SIZE != 0 || pe->base < USER_BASE_32 ||
        !within(pe->base, pe->image_size, ADDRESS_LIMIT_32))
        return 0;
    end = pe->headers;
    for (i = 0; i < pe->section_count; i++) {
        const UINT8 *s = section_at(pe, i);
        UINT64 address = read32(s + SECTION_ADDRESS);

        if (!within(address, read32(s + SECTION_VIRTUAL_SIZE), image_size) ||
            !within(read32(s + SECTION_RAW_OFFSET), data_size(s), size))
            return 0;
        if (address + data_size(s) > end)
            end = address + data_size(s);
    }
    pe->data_end = PAGES(end);
    return 1;
}

/* The protection of the image page at offset start: readable, with
   write and execute where a section reaching it has them. */
static UINT32 page_protection(const PE32 *pe, UINT64 start)
{
    UINT32 rights = 0, i;

    for (i = 0; i < pe->section_count; i++) {
        const UINT8 *section = section_at(pe, i);
        UINT64 address = read32(section + SECTION_ADDRESS);
        UINT64 size = read32(section + SECTION_VIRTUAL_SIZE);

        if (address < start + PAGE_SIZE && start < address + size)
            rights |= read32(section + SECTION_CHARACTERISTICS);
    }
    return MAP_READ | ((rights & SECTION_WRITE) ? MAP_WRITE : 0) |
           ((rights & SECTION_EXECUTE) ? MAP_EXECUTE : 0);
}

/* Map the image's pages [from, to) into process at their place, from
   section, whose byte at offset first holds the image's byte at from,
   one mapping for each run of pages with one protection. */
static int map_pages(UINT32 process, const PE32 *pe, UINT32 section,
                     UINT64 first, UINT64 from, UINT64 to)
{
    UINT64 start, run;
    UINT32 protection;

    for (start = from; start < to; start = run) {
        protection = page_protection(pe, start);
        for (run = start + PAGE_SIZE;
             run < to && page_protection(pe, run) == protection;
             run += PAGE_SIZE)
            ;
        if (process_map(process, section, first + (start - from),
                        pe->base + start, run - start, protection) != OK)
            return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------------
   The staging section. */

/* Make the staging section ready for a new process: mapped into init,
   and zero where the last process was given it. */
static int staging_begin(void)
{
    UINT64 *words;
    UINT64 i;

    if (!staging) {
        if (section_create(STAGING_SIZE, 0, &staging_section) != OK)
            return 0;
        staging = map_into_init(staging_section, STAGING_SIZE,
                                MAP_READ | MAP_WRITE);
        if (!staging) {
            handle_close(staging_section);
            return 0;
        }
    }
    words = (UINT64 *)staging;
    for (i = 0; i < staging_used / sizeof *words; i++)
        words[i] = 0;
    staging_used = 0;
    return 1;
}

/* size bytes of the staging section, whole pages, at *offset. */
static int staging_take(UINT64 size, UINT64 *offset)
{
    size = PAGES(size);
    if (size > STAGING_SIZE - staging_used)
        return 0;
    *offset = staging_used;
    staging_used += size;
    return 1;
}

/* Load image into process at its ImageBase: its headers and section data
   copied to the staging section and mapped from there, the rest of the
   image mapped from fresh sections, each page with the rights of the
   sections on it. */
static int load_image(UINT32 process, const COMPAT_IMAGE *image,
                      IMAGE32 *loaded)
{
    PE32 pe;
    UINT8 *memory;
    UINT64 at, start, size, j;
    UINT32 i, section;

    if (!parse(image->data, image->size, &pe) ||
        !staging_take(pe.data_end, &at))
        return 0;
    memory = staging + at;
    for (j = 0; j < pe.headers; j++)
        memory[j] = image->data[j];
    for (i = 0; i < pe.section_count; i++) {
        const UINT8 *s = section_at(&pe, i);
        const UINT8 *from = image->data + read32(s + SECTION_RAW_OFFSET);
        UINT8 *to = memory + read32(s + SECTION_ADDRESS);
        UINT32 length = data_size(s);

        for (j = 0; j < length; j++)
            to[j] = from[j];
    }
    if (!map_pages(process, &pe, staging_section, at, 0, pe.data_end))
        return 0;
    for (start = pe.data_end; start < pe.image_size; start += size) {
        size = pe.image_size - start < SECTION_MAX_SIZE
                   ? pe.image_size - start
                   : SECTION_MAX_SIZE;
        if (section_create(size, 0, &section) != OK)
            return 0;
        if (!map_pages(process, &pe, section, 0, start, start + size)) {
            handle_close(section);
            return 0;
        }
        handle_close(section);
    }
    loaded->base = pe.base;
    loaded->end = pe.base + pe.image_size;
    loaded->entry = pe.base + pe.entry;
    loaded->stack = pe.stack;
    return 1;
}

/* size bytes of fresh sections, whole pages, mapped read-write at
   address in process only. */
static int map_zeroed(UINT32 process, UINT64 address, UINT64 size)
{
    UINT64 at, piece;
    UINT32 section;
    int mapped;

    for (at = 0; at < size; at += piece) {
        piece = size - at < SECTION_MAX_SIZE ? size - at : SECTION_MAX_SIZE;
        if (section_create(piece, 0, &section) != OK)
            return 0;
        mapped = process_map(process, section, 0, address + at, piece,
                             MAP_READ | MAP_WRITE) == OK;
        handle_close(section);
        if (!mapped)
            return 0;
    }
    return 1;
}

/* The base of the program's stack, which ends at COMPAT_STACK_TOP and
   holds its SizeOfStackReserve; 0 if that is none, or reaches below
   USER_BASE or into the program's image. */
static UINT64 stack_base(const IMAGE32 *program)
{
    UINT64 base;

    if (program->stack == 0 || program->stack > COMPAT_STACK_TOP - USER_BASE_32)
        return 0;
    base = COMPAT_STACK_TOP - program->stack;
    if (base < program->end && program->base < COMPAT_STACK_TOP)
        return 0;
    return base;
}

/* ------------------------------------------------------------------------
   The environment block: the TEB in its first page, and after it the
   PEB, the loader data with the program and ntdll, in that order, the
   process parameters, the COMPAT_START block, and the strings and the
   environment they point at (nt32.h, compat_abi.h). */

typedef struct {
    UINT8 *memory; /* the block, as init maps it */
    UINT32 used;
} LAYOUT;

/* Room for size bytes in the layout; their offset, or 0 if the block
   is full, which no offset of a placed object is. */
static UINT32 place(LAYOUT *layout, UINT32 size)
{
    UINT32 at = (layout->used + LAYOUT_ALIGNMENT - 1) &
                ~(UINT32)(LAYOUT_ALIGNMENT - 1);

    if (at > COMPAT_ENVIRONMENT_SIZE || size > COMPAT_ENVIRONMENT_SIZE - at)
        return 0;
    layout->used = at + size;
    return at;
}

static UINT32 child_address(UINT32 offset)
{
    return COMPAT_ENVIRONMENT_BASE + offset;
}

/* ascii as a NUL-terminated UTF-16 string in the layout, described by
   string, whose Length leaves out the NUL. */
static int put_string(LAYOUT *layout, const char *ascii,
                      UNICODE_STRING32 *string)
{
    UINT32 length = 0, at, i;
    UINT16 *units;

    while (ascii[length])
        length++;
    if (length >= COMPAT_ENVIRONMENT_SIZE / sizeof(UINT16))
        return 0;
    at = place(layout, (length + 1) * sizeof(UINT16));
    if (!at)
        return 0;
    units = (UINT16 *)(layout->memory + at);
    for (i = 0; i <= length; i++)
        units[i] = (UINT8)ascii[i];
    string->Length = (UINT16)(length * sizeof(UINT16));
    string->MaximumLength = (UINT16)((length + 1) * sizeof(UINT16));
    string->Buffer = child_address(at);
    return 1;
}

static UINT32 links_of(UINT32 entry)
{
    return child_address(entry) +
           (UINT32)offsetof(LDR_DATA_TABLE_ENTRY32, InMemoryOrderLinks);
}

/* An environment block of no variables: the two NULs that end every
   block ("Environment Variables"). */
#define EMPTY_ENVIRONMENT_UNITS 2

/* Build the environment block in memory, zeroed, which init maps;
   returns the child's address of the COMPAT_START block, or 0. The
   standard handles in the parameters stay 0, no handle. */
static UINT32 build_environment(UINT8 *memory, const COMPAT_IMAGE *program,
                                const IMAGE32 *program_image,
                                const COMPAT_IMAGE *ntdll,
                                const IMAGE32 *ntdll_image,
                                const char *command_line)
{
    LAYOUT layout;
    TEB32 *teb = (TEB32 *)memory;
    UINT32 peb_at, ldr_at, exe_at, dll_at, parameters_at, start_at;
    UINT32 environment_at;
    PEB32 *peb;
    PEB_LDR_DATA32 *ldr;
    LDR_DATA_TABLE_ENTRY32 *exe, *dll;
    RTL_USER_PROCESS_PARAMETERS32 *parameters;
    COMPAT_START *start;
    UNICODE_STRING32 directory;
    UINT32 head;

    layout.memory = memory;
    layout.used = PAGE_SIZE;
    peb_at = place(&layout, sizeof(PEB32));
    ldr_at = place(&layout, sizeof(PEB_LDR_DATA32));
    exe_at = place(&layout, sizeof(LDR_DATA_TABLE_ENTRY32));
    dll_at = place(&layout, sizeof(LDR_DATA_TABLE_ENTRY32));
    parameters_at = place(&layout, sizeof(RTL_USER_PROCESS_PARAMETERS32));
    start_at = place(&layout, sizeof(COMPAT_START));
    environment_at = place(&layout, EMPTY_ENVIRONMENT_UNITS * sizeof(UINT16));
    if (!peb_at || !ldr_at || !exe_at || !dll_at || !parameters_at ||
        !start_at || !environment_at)
        return 0;
    peb = (PEB32 *)(memory + peb_at);
    ldr = (PEB_LDR_DATA32 *)(memory + ldr_at);
    exe = (LDR_DATA_TABLE_ENTRY32 *)(memory + exe_at);
    dll = (LDR_DATA_TABLE_ENTRY32 *)(memory + dll_at);
    parameters = (RTL_USER_PROCESS_PARAMETERS32 *)(memory + parameters_at);
    start = (COMPAT_START *)(memory + start_at);

    teb->ProcessEnvironmentBlock = child_address(peb_at);
    peb->Ldr = child_address(ldr_at);
    peb->ProcessParameters = child_address(parameters_at);
    head = child_address(ldr_at) +
           (UINT32)offsetof(PEB_LDR_DATA32, InMemoryOrderModuleList);
    ldr->InMemoryOrderModuleList.Flink = links_of(exe_at);
    ldr->InMemoryOrderModuleList.Blink = links_of(dll_at);
    exe->InMemoryOrderLinks.Flink = links_of(dll_at);
    exe->InMemoryOrderLinks.Blink = head;
    dll->InMemoryOrderLinks.Flink = head;
    dll->InMemoryOrderLinks.Blink = links_of(exe_at);
    exe->DllBase = program_image->base;
    dll->DllBase = ntdll_image->base;
    parameters->Environment = child_address(environment_at);
    if (!put_string(&layout, program->name, &exe->FullDllName) ||
        !put_string(&layout, ntdll->name, &dll->FullDllName) ||
        !put_string(&layout, program->name, &parameters->ImagePathName) ||
        !put_string(&layout, command_line, &parameters->CommandLine) ||
        !put_string(&layout, CURRENT_DIRECTORY, &directory))
        return 0;
    start->teb = COMPAT_ENVIRONMENT_BASE;
    start->entry = program_image->entry;
    start->heap_base = COMPAT_HEAP_BASE;
    start->heap_size = COMPAT_HEAP_SIZE;
    start->current_directory = directory.Buffer;
    return child_address(start_at);
}

/* ------------------------------------------------------------------------
   The start. */

UINT32 compat_start(UINT32 job, const COMPAT_IMAGE *program,
                    const COMPAT_IMAGE *ntdll, const char *command_line,
                    UINT32 *directory)
{
    IMAGE32 program_image, ntdll_image;
    UINT32 process, thread, client, start;
    UINT64 environment, stack;

    if (!staging_begin() ||
        process_create(job,
                       PROCESS_OPTION_32_BIT | PROCESS_OPTION_WRITE_EXECUTE,
                       &process) != OK)
        return 0;
    if (!load_image(process, program, &program_image) ||
        !load_image(process, ntdll, &ntdll_image) ||
        !(stack = stack_base(&program_image)) ||
        !map_zeroed(process, stack, COMPAT_STACK_TOP - stack) ||
        !map_zeroed(process, COMPAT_HEAP_BASE, COMPAT_HEAP_SIZE) ||
        !staging_take(COMPAT_ENVIRONMENT_SIZE, &environment) ||
        !(start = build_environment(staging + environment, program,
                                    &program_image, ntdll, &ntdll_image,
                                    command_line)) ||
        process_map(process, staging_section, environment,
                    COMPAT_ENVIRONMENT_BASE, COMPAT_ENVIRONMENT_SIZE,
                    MAP_READ | MAP_WRITE) != OK ||
        thread_create(process, &thread) != OK) {
        handle_close(process);
        return 0;
    }
    if (channel_create(DIRECTORY_CAPACITY, directory, &client) != OK) {
        handle_close(thread);
        handle_close(process);
        return 0;
    }
    if (process_start(process, thread, ntdll_image.entry, COMPAT_STACK_TOP,
                      client, start) != OK) {
        handle_close(client);
        handle_close(*directory);
        handle_close(thread);
        handle_close(process);
        return 0;
    }
    handle_close(thread);
    return process;
}
