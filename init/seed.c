/* The seed's run: init loads stage0-pe32's hex0-seed.exe, an i386 PE32
   with no imports, into a 32-bit process with ntdll beside it, serves
   that process's view of storage, and checks what the seed writes
   (docs/spec/compat.md). hex0-seed.exe assembles hex0_x86.hex0 into its
   own bytes, so the output must equal the seed's image.

   The process's derivation, as nova-nix gives it, runs the seed with
   the source file and the output path as its arguments; its view is
   exactly those two files, the source read-only from the boot volume
   and the output in init's memory, and the process holds nothing else
   (DESIGN.md, principle 7). Its one section is readable, writable and
   executable, the case WRITE_EXECUTE exists for (docs/spec/objects.md
   O5), so the process is created with PROCESS_OPTION_WRITE_EXECUTE. */

#include <stddef.h>

#include "init.h"
#include "compat_abi.h"
#include "file_protocol.h"
#include "nt32.h"
#include "sha256.h"

/* The boot volume's names, 8.3 as docs/spec/disk-layout.md requires, and
   the names the seed's view gives them, stage0-pe32's. */
#define SEED_FILE "hex0seed.exe"
#define SOURCE_FILE "hex0_x86.hex"
#define NTDLL_FILE "ntdll.dll"
#define VIEW_SOURCE "hex0_x86.hex0"
#define VIEW_OUTPUT "hex0.exe"

/* The process's command line, its image's name and its current
   directory, the root of its view. */
#define COMMAND_LINE "hex0-seed.exe " VIEW_SOURCE " " VIEW_OUTPUT
#define IMAGE_NAME "hex0-seed.exe"
#define CURRENT_DIRECTORY "C:\\"

/* The most bytes the output file holds. */
#define OUTPUT_CAPACITY 0x40000

/* The most files the process holds open at once. */
#define OPEN_FILES_MAX 8

/* The view's directory channel queues one open at a time. */
#define DIRECTORY_CAPACITY 1

#define PAGE_SIZE 4096
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

typedef struct {
    UINT32 base;
    UINT32 entry;
} IMAGE32;

typedef struct {
    const char *name;
    const UINT8 *data;
    UINT8 *buffer; /* the output's bytes, or 0 for a read-only file */
    UINT64 size;
    int exists;
} VIEW_FILE;

typedef struct {
    UINT32 channel;
    VIEW_FILE *file;
    UINT64 position;
    UINT32 access;
} OPEN_FILE;

static UINT8 message[CHANNEL_MAX_BYTES];
static UINT8 reply[sizeof(FILE_REPLY) + FILE_TRANSFER_MAX];

/* A reply carries no handles. */
static const HANDLE_TRANSFER no_handles[1];

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

static UINT32 min32(UINT32 a, UINT32 b) { return a < b ? a : b; }

/* The union of the characteristics of the sections that reach the page
   at image offset start. */
static UINT32 page_characteristics(const UINT8 *sections, UINT32 count,
                                   UINT64 start)
{
    UINT32 rights = 0, i;

    for (i = 0; i < count; i++) {
        const UINT8 *section = sections + i * SECTION_HEADER_SIZE;
        UINT64 address = read32(section + SECTION_ADDRESS);
        UINT64 size = read32(section + SECTION_VIRTUAL_SIZE);

        if (address < start + PAGE_SIZE && start < address + size)
            rights |= read32(section + SECTION_CHARACTERISTICS);
    }
    return rights;
}

static UINT32 protection_of(UINT32 characteristics)
{
    return MAP_READ | ((characteristics & SECTION_WRITE) ? MAP_WRITE : 0) |
           ((characteristics & SECTION_EXECUTE) ? MAP_EXECUTE : 0);
}

/* Load the PE32 image in the size bytes at file into process at its
   ImageBase, which must lie with the whole image in [USER_BASE, 4 GiB),
   as the kernel loads init's (docs/spec/boot-protocol.md, First process
   image): headers and sections checked against the file and the image,
   no imports, the headers and each section's data copied to a fresh
   section, each page mapped with the rights of the sections on it. */
static int load_image(UINT32 process, const UINT8 *file, UINT64 size,
                      IMAGE32 *image)
{
    const UINT8 *nt, *opt, *sections;
    UINT32 nt_offset, optional_size, count, image_size, headers, entry, i;
    UINT32 section;
    UINT64 base, start, run;
    UINT8 *memory;

    if (size < DOS_PE_OFFSET + 4 || file[0] != 'M' || file[1] != 'Z')
        return 0;
    nt_offset = read32(file + DOS_PE_OFFSET);
    if (!within(nt_offset, OPTIONAL_HEADER, size))
        return 0;
    nt = file + nt_offset;
    opt = nt + OPTIONAL_HEADER;
    count = read16(nt + COFF_SECTIONS);
    optional_size = read16(nt + COFF_OPTIONAL_SIZE);
    if (read32(nt) != PE_SIGNATURE ||
        read16(nt + COFF_MACHINE) != PE_MACHINE_I386 ||
        optional_size < OPT_DIRECTORIES ||
        !within((UINT64)nt_offset + OPTIONAL_HEADER,
                optional_size + (UINT64)count * SECTION_HEADER_SIZE, size) ||
        read16(opt) != PE32_MAGIC ||
        read32(opt + OPT_SECTION_ALIGNMENT) > PAGE_SIZE)
        return 0;
    if (read32(opt + OPT_DIRECTORY_COUNT) > IMPORT_DIRECTORY) {
        UINT32 slot = OPT_DIRECTORIES + IMPORT_DIRECTORY * DIRECTORY_SIZE;

        if (optional_size < slot + DIRECTORY_SIZE ||
            read32(opt + slot + 4) != 0)
            return 0;
    }
    sections = opt + optional_size;
    base = read32(opt + OPT_IMAGE_BASE);
    image_size = read32(opt + OPT_IMAGE_SIZE);
    headers = read32(opt + OPT_HEADERS_SIZE);
    entry = read32(opt + OPT_ENTRY);
    if (headers > image_size || headers > size ||
        (UINT64)(sections - file) + (UINT64)count * SECTION_HEADER_SIZE >
            headers ||
        entry == 0 || entry >= image_size || base % PAGE_SIZE != 0 ||
        base < USER_BASE_32 || !within(base, PAGES((UINT64)image_size),
                                       ADDRESS_LIMIT_32) ||
        PAGES((UINT64)image_size) > SECTION_MAX_SIZE)
        return 0;
    for (i = 0; i < count; i++) {
        const UINT8 *s = sections + i * SECTION_HEADER_SIZE;

        if (!within(read32(s + SECTION_ADDRESS), read32(s + SECTION_VIRTUAL_SIZE),
                    image_size) ||
            !within(read32(s + SECTION_RAW_OFFSET),
                    min32(read32(s + SECTION_RAW_SIZE),
                          read32(s + SECTION_VIRTUAL_SIZE)),
                    size))
            return 0;
    }
    if (section_create(image_size, 0, &section) != OK)
        return 0;
    memory = map_into_init(section, PAGES((UINT64)image_size),
                           MAP_READ | MAP_WRITE);
    if (!memory) {
        handle_close(section);
        return 0;
    }
    for (i = 0; i < headers; i++)
        memory[i] = file[i];
    for (i = 0; i < count; i++) {
        const UINT8 *s = sections + i * SECTION_HEADER_SIZE;
        UINT32 length = min32(read32(s + SECTION_RAW_SIZE),
                              read32(s + SECTION_VIRTUAL_SIZE));
        UINT32 j;

        for (j = 0; j < length; j++)
            memory[read32(s + SECTION_ADDRESS) + j] =
                file[read32(s + SECTION_RAW_OFFSET) + j];
    }
    for (start = 0; start < image_size; start = run) {
        UINT32 protection = protection_of(
            page_characteristics(sections, count, start));

        for (run = start + PAGE_SIZE;
             run < image_size &&
             protection_of(page_characteristics(sections, count, run)) ==
                 protection;
             run += PAGE_SIZE)
            ;
        if (process_map(process, section, start, base + start, run - start,
                        protection) != OK) {
            handle_close(section);
            return 0;
        }
    }
    handle_close(section);
    image->base = (UINT32)base;
    image->entry = (UINT32)base + entry;
    return 1;
}

/* ------------------------------------------------------------------------
   The process environment: the TEB in the environment section's first
   page, and after it the PEB, the loader data with the program and
   ntdll, in that order, the process parameters, the COMPAT_START block
   and the strings they point at (nt32.h, compat_abi.h). */

typedef struct {
    UINT8 *memory; /* the section, as init maps it */
    UINT32 used;
} LAYOUT;

#define LAYOUT_ALIGNMENT 8

/* Room for size bytes in the layout; their offset, or 0 if the section
   is full, which no offset of a placed object is. */
static UINT32 place(LAYOUT *layout, UINT32 size)
{
    UINT32 at = (layout->used + LAYOUT_ALIGNMENT - 1) &
                ~(UINT32)(LAYOUT_ALIGNMENT - 1);

    if (size > COMPAT_ENVIRONMENT_SIZE - at)
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

/* Build the environment in memory, which init maps; returns the child's
   address of the COMPAT_START block, or 0. */
static UINT32 build_environment(UINT8 *memory, const IMAGE32 *program,
                                const IMAGE32 *ntdll)
{
    LAYOUT layout;
    TEB32 *teb = (TEB32 *)memory;
    UINT32 peb_at, ldr_at, exe_at, dll_at, parameters_at, start_at;
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
    if (!peb_at || !ldr_at || !exe_at || !dll_at || !parameters_at ||
        !start_at)
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
    exe->DllBase = program->base;
    dll->DllBase = ntdll->base;
    if (!put_string(&layout, IMAGE_NAME, &exe->FullDllName) ||
        !put_string(&layout, NTDLL_FILE, &dll->FullDllName) ||
        !put_string(&layout, IMAGE_NAME, &parameters->ImagePathName) ||
        !put_string(&layout, COMMAND_LINE, &parameters->CommandLine) ||
        !put_string(&layout, CURRENT_DIRECTORY, &directory))
        return 0;
    start->teb = COMPAT_ENVIRONMENT_BASE;
    start->entry = program->entry;
    start->heap_base = COMPAT_HEAP_BASE;
    start->heap_size = COMPAT_HEAP_SIZE;
    start->current_directory = directory.Buffer;
    return child_address(start_at);
}

/* A fresh section of size bytes mapped read-write at address in
   process; returns it as init maps it, or 0. */
static UINT8 *map_fresh(UINT32 process, UINT64 address, UINT64 size)
{
    UINT32 section;
    UINT8 *memory;

    if (section_create(size, 0, &section) != OK)
        return 0;
    memory = map_into_init(section, size, MAP_READ | MAP_WRITE);
    if (!memory ||
        process_map(process, section, 0, address, size,
                    MAP_READ | MAP_WRITE) != OK)
        memory = 0;
    handle_close(section);
    return memory;
}

/* ------------------------------------------------------------------------
   The view's server. */

static VIEW_FILE view[2];
static OPEN_FILE opens[OPEN_FILES_MAX];
static UINT32 open_count;

static int exact_name(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/* The view's file of the length bytes at name, which the request may
   ask to match regardless of ASCII case. */
static VIEW_FILE *find(const UINT8 *name, UINT32 length, int any_case)
{
    char wanted[BOOT_FILE_NAME_SIZE];
    UINT32 i;

    if (length >= BOOT_FILE_NAME_SIZE)
        return 0;
    for (i = 0; i < length; i++)
        wanted[i] = (char)name[i];
    wanted[length] = 0;
    for (i = 0; i < sizeof view / sizeof view[0]; i++)
        if (any_case ? same_name(view[i].name, wanted)
                     : exact_name(view[i].name, wanted))
            return &view[i];
    return 0;
}

/* The outcome of opening file with the disposition and access in flags,
   NT's dispositions applied to a read-only file that exists and to the
   output, which exists once created. */
static UINT32 open_file(VIEW_FILE *file, UINT32 flags, UINT64 *information)
{
    UINT32 disposition = flags & FILE_FLAGS_DISPOSITION;
    int truncate = disposition == FILE_SUPERSEDE ||
                   disposition == FILE_OVERWRITE ||
                   disposition == FILE_OVERWRITE_IF;

    if (!file)
        return FILE_NOT_FOUND;
    if (disposition > FILE_OVERWRITE_IF)
        return FILE_INVALID;
    if (!file->buffer && ((flags & FILE_ACCESS_WRITE) || truncate ||
                          disposition == FILE_CREATE))
        return disposition == FILE_CREATE ? FILE_EXISTS : FILE_DENIED;
    if (file->exists) {
        if (disposition == FILE_CREATE)
            return FILE_EXISTS;
        if (truncate)
            file->size = 0;
        *information = disposition == FILE_SUPERSEDE   ? FILE_SUPERSEDED
                       : truncate                       ? FILE_OVERWRITTEN
                                                        : FILE_OPENED;
        return FILE_OK;
    }
    if (disposition == FILE_OPEN || disposition == FILE_OVERWRITE)
        return FILE_NOT_FOUND;
    file->exists = 1;
    file->size = 0;
    *information = FILE_CREATED;
    return FILE_OK;
}

static void answer(UINT32 channel, UINT32 transaction, UINT32 status,
                   UINT64 information, UINT32 data)
{
    FILE_REPLY *header = (FILE_REPLY *)reply;

    header->transaction = transaction;
    header->status = status;
    header->information = information;
    channel_write(channel, reply, sizeof(FILE_REPLY) + data, no_handles, 0);
}

/* A message's bytes into message, closing any handles it carries but
   the one an open passes, which goes to *handle. Returns the byte count,
   or -1 if nothing could be read. */
static INT64 take(UINT32 channel, UINT32 *handle)
{
    RECEIVED_HANDLE handles[CHANNEL_MAX_HANDLES];
    UINT32 bytes, count, i;

    if (channel_read(channel, message, sizeof message, handles,
                     CHANNEL_MAX_HANDLES, &bytes, &count) != OK)
        return -1;
    *handle = HANDLE_INVALID;
    for (i = 0; i < count; i++) {
        if (i == 0 && handles[0].type == TYPE_CHANNEL)
            *handle = handles[0].handle;
        else
            handle_close(handles[i].handle);
    }
    return bytes;
}

/* An open whose request is malformed is answered with transaction 0,
   which no request of the client's carries. */
static void serve_open(UINT32 directory)
{
    const FILE_REQUEST *request = (const FILE_REQUEST *)message;
    UINT64 information = 0;
    UINT32 endpoint, status, transaction = 0;
    VIEW_FILE *file = 0;
    INT64 bytes = take(directory, &endpoint);

    if (bytes < 0 || endpoint == HANDLE_INVALID)
        return;
    if ((UINT64)bytes < sizeof(FILE_REQUEST) ||
        request->operation != FILE_OPERATION_OPEN ||
        request->length != bytes - sizeof(FILE_REQUEST)) {
        status = FILE_INVALID;
    } else {
        transaction = request->transaction;
        file = find(message + sizeof(FILE_REQUEST), request->length,
                    (request->flags & FILE_NAME_ANY_CASE) != 0);
        status = open_count == OPEN_FILES_MAX
                     ? FILE_FULL
                     : open_file(file, request->flags, &information);
    }
    answer(endpoint, transaction, status, information, 0);
    if (status != FILE_OK) {
        handle_close(endpoint);
        return;
    }
    opens[open_count].channel = endpoint;
    opens[open_count].file = file;
    opens[open_count].position = 0;
    opens[open_count].access =
        request->flags & (FILE_ACCESS_READ | FILE_ACCESS_WRITE);
    open_count++;
}

static void serve_file(OPEN_FILE *open)
{
    const FILE_REQUEST *request = (const FILE_REQUEST *)message;
    VIEW_FILE *file = open->file;
    UINT32 endpoint;
    INT64 bytes = take(open->channel, &endpoint);
    UINT64 at, count = 0, i;
    UINT32 status = FILE_OK;

    if (endpoint != HANDLE_INVALID)
        handle_close(endpoint);
    if (bytes < 0)
        return;
    if (bytes < (INT64)sizeof(FILE_REQUEST)) {
        answer(open->channel, 0, FILE_INVALID, 0, 0);
        return;
    }
    at = request->offset == FILE_OFFSET_CURRENT ? open->position
                                                : request->offset;
    if (request->operation == FILE_OPERATION_READ) {
        if (!(open->access & FILE_ACCESS_READ))
            status = FILE_DENIED;
        else if (at < file->size)
            count = file->size - at;
        if (count > request->length)
            count = request->length;
        if (count > FILE_TRANSFER_MAX)
            count = FILE_TRANSFER_MAX;
        for (i = 0; i < count; i++)
            reply[sizeof(FILE_REPLY) + i] =
                (file->buffer ? file->buffer : file->data)[at + i];
    } else if (request->operation == FILE_OPERATION_WRITE) {
        count = request->length;
        if (!(open->access & FILE_ACCESS_WRITE) || !file->buffer)
            status = FILE_DENIED;
        else if (count != bytes - sizeof(FILE_REQUEST))
            status = FILE_INVALID;
        else if (at > OUTPUT_CAPACITY || count > OUTPUT_CAPACITY - at)
            status = FILE_FULL;
        for (i = file->size; status == FILE_OK && i < at; i++)
            file->buffer[i] = 0;
        for (i = 0; status == FILE_OK && i < count; i++)
            file->buffer[at + i] = message[sizeof(FILE_REQUEST) + i];
        if (status == FILE_OK && at + count > file->size)
            file->size = at + count;
    } else {
        status = FILE_INVALID;
    }
    if (status != FILE_OK)
        count = 0;
    else
        open->position = at + count;
    answer(open->channel, request->transaction, status, count,
           request->operation == FILE_OPERATION_READ ? (UINT32)count : 0);
}

/* Serve the view until process ends. The wait lists the process, the
   directory and the open files, in that order; a channel whose peer has
   closed leaves it, since its PEER_CLOSED stays active. The open files
   are served before the directory, whose new opens join the end of the
   list, so each index still names the file it was waited on as. */
#define WAIT_PROCESS 0
#define WAIT_DIRECTORY 1

static void serve(UINT32 directory, UINT32 process)
{
    WAIT_ENTRY items[2 + OPEN_FILES_MAX];
    int directory_open = 1;
    UINT32 count, first, waited, i;

    for (;;) {
        items[WAIT_PROCESS].handle = process;
        items[WAIT_PROCESS].signals = PROCESS_TERMINATED;
        items[WAIT_DIRECTORY].handle = directory;
        items[WAIT_DIRECTORY].signals = CHANNEL_READABLE | CHANNEL_PEER_CLOSED;
        first = count = directory_open ? WAIT_DIRECTORY + 1 : WAIT_PROCESS + 1;
        for (i = 0; i < open_count; i++) {
            items[count].handle = opens[i].channel;
            items[count++].signals = CHANNEL_READABLE | CHANNEL_PEER_CLOSED;
        }
        waited = open_count;
        if (object_wait_many(items, count, DEADLINE_INFINITE) != OK ||
            (items[WAIT_PROCESS].observed & PROCESS_TERMINATED))
            break;
        for (i = waited; i-- > 0;) {
            UINT32 observed = items[first + i].observed;

            if (observed & CHANNEL_READABLE) {
                serve_file(&opens[i]);
            } else if (observed & CHANNEL_PEER_CLOSED) {
                handle_close(opens[i].channel);
                opens[i] = opens[--open_count];
            }
        }
        if (directory_open) {
            if (items[WAIT_DIRECTORY].observed & CHANNEL_READABLE)
                serve_open(directory);
            else if (items[WAIT_DIRECTORY].observed & CHANNEL_PEER_CLOSED)
                directory_open = 0;
        }
    }
    for (i = 0; i < open_count; i++)
        handle_close(opens[i].channel);
    open_count = 0;
    handle_close(directory);
}

/* ------------------------------------------------------------------------
   The run. */

static void say_hex_bytes(const UINT8 *bytes, UINT32 count)
{
    static const char digits[] = "0123456789abcdef";
    char text[2 * SHA256_DIGEST_SIZE + 1];
    UINT32 i;

    for (i = 0; i < count && i < SHA256_DIGEST_SIZE; i++) {
        text[2 * i] = digits[bytes[i] >> 4];
        text[2 * i + 1] = digits[bytes[i] & 0xf];
    }
    text[2 * i] = 0;
    say(text);
}

static int fail(const char *why)
{
    say("nova-init: seed run failed: ");
    say(why);
    say("\r\n");
    return 0;
}

/* Start the seed in a new 32-bit process and return its handle, with the
   server end of its view's directory channel in *directory; 0 if it did
   not start. */
static UINT32 launch(UINT32 job, const BOOT_FILE_VIEW *seed,
                     const BOOT_FILE_VIEW *ntdll_file, UINT32 *directory)
{
    IMAGE32 program, ntdll;
    UINT32 process, thread, client, start;
    UINT8 *environment;

    if (process_create(job, PROCESS_OPTION_32_BIT | PROCESS_OPTION_WRITE_EXECUTE,
                       &process) != OK)
        return 0;
    if (!load_image(process, seed->data, seed->size, &program) ||
        !load_image(process, ntdll_file->data, ntdll_file->size, &ntdll) ||
        !map_fresh(process, COMPAT_STACK_BASE, COMPAT_STACK_SIZE) ||
        !map_fresh(process, COMPAT_HEAP_BASE, COMPAT_HEAP_SIZE) ||
        !(environment = map_fresh(process, COMPAT_ENVIRONMENT_BASE,
                                  COMPAT_ENVIRONMENT_SIZE)) ||
        !(start = build_environment(environment, &program, &ntdll)) ||
        thread_create(process, &thread) != OK) {
        handle_close(process);
        return 0;
    }
    if (channel_create(DIRECTORY_CAPACITY, directory, &client) != OK) {
        handle_close(thread);
        handle_close(process);
        return 0;
    }
    if (process_start(process, thread, ntdll.entry,
                      COMPAT_STACK_BASE + COMPAT_STACK_SIZE, client,
                      start) != OK) {
        handle_close(client);
        handle_close(*directory);
        handle_close(thread);
        handle_close(process);
        return 0;
    }
    handle_close(thread);
    return process;
}

int seed_run(UINT32 job)
{
    const BOOT_FILE_VIEW *seed = boot_file(SEED_FILE);
    const BOOT_FILE_VIEW *source = boot_file(SOURCE_FILE);
    const BOOT_FILE_VIEW *ntdll = boot_file(NTDLL_FILE);
    SHA256_CONTEXT context;
    UINT8 digest[SHA256_DIGEST_SIZE];
    UINT32 process, directory, buffer, state, exit_code;
    VIEW_FILE *output = &view[1];
    UINT64 i;

    if (!seed || !source) {
        say("nova-init: no seed on the boot volume, its run skipped\r\n");
        return 1;
    }
    if (!ntdll)
        return fail("no ntdll.dll on the boot volume");
    view[0].name = VIEW_SOURCE;
    view[0].data = source->data;
    view[0].size = source->size;
    view[0].exists = 1;
    output->name = VIEW_OUTPUT;
    if (section_create(OUTPUT_CAPACITY, 0, &buffer) != OK ||
        !(output->buffer = map_into_init(buffer, OUTPUT_CAPACITY,
                                         MAP_READ | MAP_WRITE)))
        return fail("no memory for the output");
    process = launch(job, seed, ntdll, &directory);
    if (!process)
        return fail("the 32-bit process did not start");
    say("nova-init: hex0-seed.exe started as a 32-bit process\r\n");
    serve(directory, process);
    process_info(process, &state, &exit_code);
    handle_close(process);
    say("nova-init: hex0-seed.exe ended with exit status ");
    say_number(exit_code);
    say("\r\n");
    if (state != PROCESS_STATE_TERMINATED || exit_code != STATUS_SUCCESS)
        return fail("hex0-seed.exe did not succeed");
    if (!output->exists)
        return fail("hex0-seed.exe wrote no hex0.exe");
    sha256_init(&context);
    sha256_update(&context, output->buffer, output->size);
    sha256_final(&context, digest);
    say("nova-init: hex0.exe: ");
    say_number(output->size);
    say(" bytes, sha256 ");
    say_hex_bytes(digest, SHA256_DIGEST_SIZE);
    say("\r\n");
    if (output->size != seed->size)
        return fail("hex0.exe is not the seed's size");
    for (i = 0; i < seed->size; i++)
        if (output->buffer[i] != seed->data[i])
            return fail("hex0.exe differs from hex0-seed.exe");
    say("nova-init: hex0.exe is hex0-seed.exe byte for byte\r\n");
    return 1;
}
