/* nova-boot: the UEFI loader. It draws the boot screen, loads the kernel
   from \nova\kernel.exe on the volume it was booted from and reads the
   first process's image from \nova\init.exe beside it, builds the
   kernel's page tables, measures the timestamp counter against Stall,
   asks the firmware's random number generators for seeds, takes the
   final memory map, exits boot services, loads the kernel's
   tables and jumps to the kernel's entry on the kernel's stack with a
   BOOT_INFO record. */

#include "efi.h"
#include "boot_info.h"
#include "cpu.h"
#include "memory_layout.h"
#include "page_table.h"
#include "tables.h"

static EFI_GUID graphics_output_guid = {
    0x9042a9de, 0x23dc, 0x4a38,
    {0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a}};
static EFI_GUID edid_active_guid = {
    0xbd8c1056, 0x9f36, 0x44ec,
    {0x92, 0xa8, 0xa6, 0x33, 0x7f, 0x81, 0x79, 0x86}};
static EFI_GUID edid_discovered_guid = {
    0x1c0c34f6, 0xd380, 0x41fa,
    {0xa0, 0x49, 0x8a, 0xd0, 0x6c, 0x1a, 0x66, 0xaa}};
static EFI_GUID loaded_image_guid = {
    0x5b1b31a1, 0x9562, 0x11d2,
    {0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};
static EFI_GUID simple_file_system_guid = {
    0x964e5b22, 0x6459, 0x11d2,
    {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};

static CHAR16 kernel_path[] = L"\\nova\\kernel.exe";
static CHAR16 init_path[] = L"\\nova\\init.exe";

#define CALIBRATION_MICROSECONDS 50000

/* GetMemoryMap calls to fill the map buffer: one to size it, one into it,
   and one more should allocating the buffer outgrow it. */
#define MAP_ATTEMPTS 3

/* ExitBootServices calls. A refusal means the map changed after it was
   taken. UEFI lets the first call shut boot services down in part
   (7.4.6); EDK II stops its timer before it checks the key, so timer
   events no longer change the map between attempts. */
#define EXIT_ATTEMPTS 3

/* Room for this many more descriptors in the map buffer, since allocating
   the buffer can itself grow the map (UEFI 7.2.3). */
#define MAP_SLACK 4

/* The kernel as placed: its pages, its entry's address once its tables
   are loaded, and its section table within the placed headers. */
typedef struct {
    EFI_PHYSICAL_ADDRESS base;
    UINTN pages;
    UINT64 entry;
    const UINT8 *sections;
    UINT32 section_count;
} KERNEL_IMAGE;

/* switch.S: loads cr3, invalidates every translation and the caches,
   moves to stack_top and calls entry with info. */
_Noreturn void boot_switch(UINT64 cr3, UINT64 stack_top, UINT64 entry,
                           BOOT_INFO *info);
extern const UINT8 boot_switch_end[];

static BOOT_INFO boot_info;

_Noreturn static void halt(void)
{
    for (;;)
        __asm__ volatile("cli\n\thlt");
}

/* Whether a mode has a linear framebuffer of 32-bit pixels that
   gfx/gfx.c can draw to. */
static int drawable(const EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info)
{
    return info->PixelFormat == PIXEL_RGB || info->PixelFormat == PIXEL_BGR;
}

/* An EDID base block (VESA E-EDID Release A, Revision 2, Table 3.1): an
   8-byte header (3.3), a checksum that makes its 128 bytes sum to zero
   (3.11), and the display's preferred timing in the first 18-byte
   descriptor (3.10.1). Feature Support bit 1 vouches for that timing:
   EDID 1.3 requires the bit, and in EDID 1.4 it says the timing is the
   display's native format (Table 3.14, note 4). Before EDID 1.3 the
   first descriptor may be a display descriptor instead, which a zero
   pixel clock marks (Table 3.1, note 3; Table 3.21). */
#define EDID_BLOCK_SIZE 128
#define EDID_HEADER_SIZE 8
#define EDID_CHECKSUM_MODULUS 256
#define EDID_FEATURE_SUPPORT 0x18
#define EDID_PREFERRED_NATIVE 0x02
#define EDID_PREFERRED_TIMING 0x36

/* Detailed timing descriptor fields (Tables 3.21 and 3.22): the pixel
   clock in bytes 0 and 1; the low 8 bits of the addressable width and
   height in bytes 2 and 5 and their high 4 bits in the upper nibbles of
   bytes 4 and 7; and the interlace flag. An interlaced timing's height
   counts the lines of one field, half the frame, as Appendix A's
   1920 x 1080i example gives 540 lines. */
#define DTD_CLOCK_LOW 0
#define DTD_CLOCK_HIGH 1
#define DTD_WIDTH_LOW 2
#define DTD_WIDTH_HIGH 4
#define DTD_HEIGHT_LOW 5
#define DTD_HEIGHT_HIGH 7
#define DTD_FLAGS 17
#define DTD_HIGH_NIBBLE 0xf0
#define DTD_HIGH_SHIFT 4
#define DTD_INTERLACED 0x80

static const UINT8 edid_header[EDID_HEADER_SIZE] = {0x00, 0xff, 0xff, 0xff,
                                                    0xff, 0xff, 0xff, 0x00};

/* The size of the preferred timing in the size bytes at edid, or 0 x 0 if
   they hold no valid EDID base block or it vouches for no preferred
   timing. Every read lies in the first EDID_BLOCK_SIZE bytes, after size
   is checked to cover them. */
static void edid_preferred(const UINT8 *edid, UINT32 size, UINT32 *width,
                           UINT32 *height)
{
    const UINT8 *timing;
    UINT32 i, sum = 0;

    *width = 0;
    *height = 0;
    if (!edid || size < EDID_BLOCK_SIZE)
        return;
    for (i = 0; i < EDID_HEADER_SIZE; i++)
        if (edid[i] != edid_header[i])
            return;
    for (i = 0; i < EDID_BLOCK_SIZE; i++)
        sum += edid[i];
    timing = edid + EDID_PREFERRED_TIMING;
    if (sum % EDID_CHECKSUM_MODULUS != 0 ||
        !(edid[EDID_FEATURE_SUPPORT] & EDID_PREFERRED_NATIVE) ||
        (timing[DTD_CLOCK_LOW] == 0 && timing[DTD_CLOCK_HIGH] == 0))
        return;
    *width = timing[DTD_WIDTH_LOW] |
             (UINT32)(timing[DTD_WIDTH_HIGH] & DTD_HIGH_NIBBLE)
                 << DTD_HIGH_SHIFT;
    *height = timing[DTD_HEIGHT_LOW] |
              (UINT32)(timing[DTD_HEIGHT_HIGH] & DTD_HIGH_NIBBLE)
                  << DTD_HIGH_SHIFT;
    if (timing[DTD_FLAGS] & DTD_INTERLACED)
        *height *= 2;
}

/* The preferred size the EDID on the display's handle gives, or 0 x 0:
   the active EDID, which a platform override may have replaced, else the
   one read from the display (UEFI 12.9.2.4, 12.9.2.5). A handle that
   drives several displays as one carries neither (UEFI 12.10). */
static void display_preferred(EFI_BOOT_SERVICES *boot, EFI_HANDLE display,
                              UINT32 *width, UINT32 *height)
{
    EFI_EDID_ACTIVE_PROTOCOL *edid;

    *width = 0;
    *height = 0;
    if (boot->HandleProtocol(display, &edid_active_guid, (VOID **)&edid) ==
            EFI_SUCCESS ||
        boot->HandleProtocol(display, &edid_discovered_guid,
                             (VOID **)&edid) == EFI_SUCCESS)
        edid_preferred(edid->Edid, edid->SizeOfEdid, width, height);
}

/* Whether a mode is drawable and width x height, with 0 x 0 matching
   none. */
static int native(const EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info,
                  UINT32 width, UINT32 height)
{
    return width != 0 && info->HorizontalResolution == width &&
           info->VerticalResolution == height && drawable(info);
}

/* The mode to draw in: a drawable mode of the display's preferred size
   width x height, the current one if it qualifies; else the current mode,
   the firmware's choice, if it is drawable; else the drawable mode with
   the most pixels. That mode comes last because it need not be the
   display's: it can be larger than the display or another shape, and be
   scaled to fit. */
static UINT32 choose_mode(EFI_BOOT_SERVICES *boot,
                          EFI_GRAPHICS_OUTPUT_PROTOCOL *gop, UINT32 width,
                          UINT32 height)
{
    UINT32 mode, largest = gop->Mode->Mode;
    UINT64 largest_area = 0;

    if (native(gop->Mode->Info, width, height))
        return gop->Mode->Mode;
    for (mode = 0; mode < gop->Mode->MaxMode; mode++) {
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info;
        UINTN size;
        UINT64 area;
        int found;

        if (gop->QueryMode(gop, mode, &size, &info) != EFI_SUCCESS)
            continue;
        found = native(info, width, height);
        area = (UINT64)info->HorizontalResolution * info->VerticalResolution;
        if (drawable(info) && area > largest_area) {
            largest = mode;
            largest_area = area;
        }
        boot->FreePool(info);
        if (found)
            return mode;
    }
    return drawable(gop->Mode->Info) ? gop->Mode->Mode : largest;
}

/* Open the first Graphics Output Protocol instance and switch it to the
   mode choose_mode picks. Whether or not SetMode succeeds, the mode in
   effect afterwards is the one drawn in. */
static EFI_STATUS open_screen(EFI_BOOT_SERVICES *boot,
                              EFI_GRAPHICS_OUTPUT_PROTOCOL **gop)
{
    EFI_HANDLE *handles;
    UINTN count;
    UINT32 width, height, mode;
    EFI_STATUS status;

    /* Success means at least one handle (UEFI 7.3.15). */
    status = boot->LocateHandleBuffer(BY_PROTOCOL, &graphics_output_guid, 0,
                                      &count, &handles);
    if (status != EFI_SUCCESS)
        return status;
    status = boot->HandleProtocol(handles[0], &graphics_output_guid,
                                  (VOID **)gop);
    if (status == EFI_SUCCESS) {
        display_preferred(boot, handles[0], &width, &height);
        mode = choose_mode(boot, *gop, width, height);
        if (mode != (*gop)->Mode->Mode)
            (*gop)->SetMode(*gop, mode);
    }
    boot->FreePool(handles);
    return status;
}

/* ------------------------------------------------------------------------
   Loading the kernel: a PE32+ image (PE Format), copied section by section
   into fresh pages and relocated for KERNEL_IMAGE_BASE, where the kernel's
   tables map it. Every offset and size the file gives is checked against
   the file or the image before it is used. */

#define PE_SIGNATURE 0x00004550 /* "PE\0\0" */
#define PE_MACHINE_AMD64 0x8664
#define PE32_PLUS 0x20b
#define PE_RELOCS_STRIPPED 0x0001
#define PE_BASE_RELOCATION_DIRECTORY 5
#define REL_BASED_ABSOLUTE 0
#define REL_BASED_DIR64 10

/* Field offsets: the PE signature's offset in the MS-DOS header; the COFF
   header's from the signature; the PE32+ optional header's, which follows
   the 20-byte COFF header; a section header's; a relocation block's. */
#define DOS_PE_OFFSET 0x3c
#define COFF_MACHINE 4
#define COFF_SECTIONS 6
#define COFF_OPTIONAL_SIZE 20
#define COFF_CHARACTERISTICS 22
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
#define RELOC_BLOCK_HEADER 8
#define RELOC_ENTRY_SIZE 2
#define RELOC_TYPE_SHIFT 12
#define RELOC_OFFSET_MASK 0xfff

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

static EFI_STATUS read_file(EFI_BOOT_SERVICES *boot, EFI_FILE_PROTOCOL *file,
                            UINT8 **data, UINTN *size)
{
    UINT64 end;
    UINTN read;
    EFI_STATUS status;

    /* A directory accepts no position but 0 (UEFI 13.5.11). */
    status = file->SetPosition(file, EFI_FILE_END);
    if (status == EFI_SUCCESS)
        status = file->GetPosition(file, &end);
    if (status == EFI_SUCCESS)
        status = file->SetPosition(file, 0);
    if (status != EFI_SUCCESS)
        return status;
    status = boot->AllocatePool(EFI_LOADER_DATA, end, (VOID **)data);
    if (status != EFI_SUCCESS)
        return status;
    read = end;
    status = file->Read(file, &read, *data);
    if (status == EFI_SUCCESS && read != end)
        status = EFI_LOAD_ERROR;
    if (status != EFI_SUCCESS) {
        boot->FreePool(*data);
        return status;
    }
    *size = end;
    return EFI_SUCCESS;
}

/* Read the file at path on nova-boot's own volume into pool memory of
   type EfiLoaderData. */
static EFI_STATUS read_boot_file(EFI_HANDLE image, EFI_BOOT_SERVICES *boot,
                                 CHAR16 *path, UINT8 **data, UINTN *size)
{
    EFI_LOADED_IMAGE_PROTOCOL *loaded;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *volume;
    EFI_FILE_PROTOCOL *root, *file;
    EFI_STATUS status;

    status = boot->HandleProtocol(image, &loaded_image_guid,
                                  (VOID **)&loaded);
    if (status != EFI_SUCCESS)
        return status;
    status = boot->HandleProtocol(loaded->DeviceHandle,
                                  &simple_file_system_guid, (VOID **)&volume);
    if (status != EFI_SUCCESS)
        return status;
    status = volume->OpenVolume(volume, &root);
    if (status != EFI_SUCCESS)
        return status;
    status = root->Open(root, &file, path, EFI_FILE_MODE_READ, 0);
    if (status == EFI_SUCCESS) {
        status = read_file(boot, file, data, size);
        file->Close(file);
    }
    root->Close(root);
    return status;
}

/* Add delta to every DIR64 location the base relocation table at rva
   names. */
static EFI_STATUS relocate(UINT8 *image, UINT32 image_size, UINT32 rva,
                           UINT32 size, UINT64 delta)
{
    UINT32 offset, block_size;

    /* Each block covers a 4 KiB page: its address, its size, then 16-bit
       entries of a 4-bit type and a 12-bit offset into the page. */
    for (offset = 0; offset < size; offset += block_size) {
        const UINT8 *block = image + rva + offset;
        UINT32 page, e;

        if (size - offset < RELOC_BLOCK_HEADER)
            return EFI_LOAD_ERROR;
        page = read32(block);
        block_size = read32(block + 4);
        if (block_size < RELOC_BLOCK_HEADER || block_size > size - offset)
            return EFI_LOAD_ERROR;
        for (e = RELOC_BLOCK_HEADER; block_size - e >= RELOC_ENTRY_SIZE;
             e += RELOC_ENTRY_SIZE) {
            UINT32 entry = read16(block + e);
            UINT32 type = entry >> RELOC_TYPE_SHIFT;
            UINT64 target = (UINT64)page + (entry & RELOC_OFFSET_MASK);

            if (type == REL_BASED_DIR64 &&
                within(target, sizeof(UINT64), image_size))
                *(UINT64 *)(image + target) += delta;
            else if (type != REL_BASED_ABSOLUTE)
                return EFI_LOAD_ERROR;
        }
    }
    return EFI_SUCCESS;
}

/* Check the headers, lay the image out at its section addresses in fresh
   pages and apply its base relocations for KERNEL_IMAGE_BASE. */
static EFI_STATUS place_kernel(EFI_BOOT_SERVICES *boot, const UINT8 *file,
                               UINTN size, KERNEL_IMAGE *kernel)
{
    const UINT8 *nt, *opt, *sections;
    UINT32 nt_offset, optional_size, sections_count, image_size,
        headers_size, entry, i, offset;
    UINT32 reloc_rva = 0, reloc_size = 0;
    UINT64 sections_end;
    UINT8 *image;
    EFI_STATUS status;

    if (size < DOS_PE_OFFSET + 4 || file[0] != 'M' || file[1] != 'Z')
        return EFI_LOAD_ERROR;
    nt_offset = read32(file + DOS_PE_OFFSET);
    if (!within(nt_offset, OPTIONAL_HEADER, size))
        return EFI_LOAD_ERROR;
    nt = file + nt_offset;
    opt = nt + OPTIONAL_HEADER;
    sections_count = read16(nt + COFF_SECTIONS);
    optional_size = read16(nt + COFF_OPTIONAL_SIZE);
    if (read32(nt) != PE_SIGNATURE ||
        read16(nt + COFF_MACHINE) != PE_MACHINE_AMD64 ||
        (read16(nt + COFF_CHARACTERISTICS) & PE_RELOCS_STRIPPED) ||
        optional_size < OPT_DIRECTORIES ||
        !within((UINT64)nt_offset + OPTIONAL_HEADER,
                optional_size + (UINT64)sections_count * SECTION_HEADER_SIZE,
                size) ||
        read16(opt) != PE32_PLUS ||
        read32(opt + OPT_SECTION_ALIGNMENT) > EFI_PAGE_SIZE)
        return EFI_LOAD_ERROR;
    sections = opt + optional_size;

    /* An image with no slot for the directory has no relocations. */
    if (read32(opt + OPT_DIRECTORY_COUNT) > PE_BASE_RELOCATION_DIRECTORY) {
        UINT32 slot = OPT_DIRECTORIES +
                      PE_BASE_RELOCATION_DIRECTORY * DIRECTORY_SIZE;

        if (optional_size < slot + DIRECTORY_SIZE)
            return EFI_LOAD_ERROR;
        reloc_rva = read32(opt + slot);
        reloc_size = read32(opt + slot + 4);
    }
    /* SizeOfHeaders covers the section table (PE Format, Optional
       Header), so the placed headers hold it for mapping the sections. */
    image_size = read32(opt + OPT_IMAGE_SIZE);
    headers_size = read32(opt + OPT_HEADERS_SIZE);
    sections_end = (UINT64)(sections - file) +
                   (UINT64)sections_count * SECTION_HEADER_SIZE;
    entry = read32(opt + OPT_ENTRY);
    if (headers_size > image_size || headers_size > size ||
        sections_end > headers_size || image_size > KERNEL_IMAGE_SPACE ||
        entry == 0 || entry >= image_size ||
        !within(reloc_rva, reloc_size, image_size))
        return EFI_LOAD_ERROR;
    for (i = 0; i < sections_count; i++) {
        const UINT8 *section = sections + i * SECTION_HEADER_SIZE;
        UINT32 virtual_size = read32(section + SECTION_VIRTUAL_SIZE);
        UINT32 raw_size = read32(section + SECTION_RAW_SIZE);
        UINT32 n = raw_size < virtual_size ? raw_size : virtual_size;

        if (!within(read32(section + SECTION_ADDRESS), virtual_size,
                    image_size) ||
            !within(read32(section + SECTION_RAW_OFFSET), n, size))
            return EFI_LOAD_ERROR;
    }

    kernel->pages = ((UINT64)image_size + EFI_PAGE_SIZE - 1) / EFI_PAGE_SIZE;
    status = boot->AllocatePages(ALLOCATE_ANY_PAGES, EFI_LOADER_CODE,
                                 kernel->pages, &kernel->base);
    if (status != EFI_SUCCESS)
        return status;
    image = (UINT8 *)kernel->base;
    for (offset = 0; offset < image_size; offset++)
        image[offset] = 0;
    for (offset = 0; offset < headers_size; offset++)
        image[offset] = file[offset];
    for (i = 0; i < sections_count; i++) {
        const UINT8 *section = sections + i * SECTION_HEADER_SIZE;
        UINT32 virtual_size = read32(section + SECTION_VIRTUAL_SIZE);
        UINT32 address = read32(section + SECTION_ADDRESS);
        UINT32 raw_size = read32(section + SECTION_RAW_SIZE);
        UINT32 raw_offset = read32(section + SECTION_RAW_OFFSET);
        UINT32 n = raw_size < virtual_size ? raw_size : virtual_size;

        for (offset = 0; offset < n; offset++)
            image[address + offset] = file[(UINT64)raw_offset + offset];
    }

    status = relocate(image, image_size, reloc_rva, reloc_size,
                      KERNEL_IMAGE_BASE - read64(opt + OPT_IMAGE_BASE));
    if (status != EFI_SUCCESS) {
        boot->FreePages(kernel->base, kernel->pages);
        return status;
    }
    kernel->entry = KERNEL_IMAGE_BASE + entry;
    kernel->sections = image + (sections - file);
    kernel->section_count = sections_count;
    return EFI_SUCCESS;
}

/* ------------------------------------------------------------------------
   The memory map */

typedef struct {
    UINT8 *buffer;
    UINTN capacity, size, key, descriptor_size;
} MEMORY_MAP;

/* Take the memory map into map's buffer, growing the buffer when the map
   has outgrown it. */
static EFI_STATUS take_map(EFI_BOOT_SERVICES *boot, MEMORY_MAP *map)
{
    UINT32 version;
    EFI_STATUS status;
    int attempt;

    for (attempt = 0; attempt < MAP_ATTEMPTS; attempt++) {
        map->size = map->capacity;
        status = boot->GetMemoryMap(&map->size, map->buffer, &map->key,
                                    &map->descriptor_size, &version);
        if (status == EFI_SUCCESS)
            return version == EFI_MEMORY_DESCRIPTOR_VERSION &&
                           map->descriptor_size >=
                               sizeof(EFI_MEMORY_DESCRIPTOR)
                       ? EFI_SUCCESS
                       : EFI_INCOMPATIBLE_VERSION;
        if (status != EFI_BUFFER_TOO_SMALL)
            return status;
        if (map->buffer)
            boot->FreePool(map->buffer);
        map->buffer = 0;
        map->capacity = map->size + MAP_SLACK * map->descriptor_size;
        status = boot->AllocatePool(EFI_LOADER_DATA, map->capacity,
                                    (VOID **)&map->buffer);
        if (status != EFI_SUCCESS) {
            map->capacity = 0;
            return status;
        }
    }
    return EFI_BUFFER_TOO_SMALL;
}

/* ------------------------------------------------------------------------
   The kernel's address space (memory_layout.h): the direct map, the
   kernel image, the kernel's stack and boot_switch's own pages. */

typedef struct {
    PAGE_TABLES tables;
    EFI_PHYSICAL_ADDRESS stack;
} ADDRESS_SPACE;

typedef struct {
    UINT64 start, end;
} RANGE;

/* Append the pages covering size bytes at start, or fail if they reach
   past the direct map. */
static EFI_STATUS add_range(RANGE *ranges, UINTN *count, UINT64 start,
                            UINT64 size)
{
    UINT64 end;

    if (start >= DIRECT_MAP_SIZE || size > DIRECT_MAP_SIZE - start)
        return EFI_UNSUPPORTED;
    end = start + size + PAGE_SIZE - 1;
    ranges[*count].start = start - start % PAGE_SIZE;
    ranges[*count].end = end - end % PAGE_SIZE;
    (*count)++;
    return EFI_SUCCESS;
}

/* Sort by start, then merge ranges that overlap or meet, so that memory
   the map splits into many descriptors still gets large pages. Returns
   the number left. */
static UINTN merge_ranges(RANGE *ranges, UINTN count)
{
    UINTN i, j, merged = 0;

    for (i = 1; i < count; i++) {
        UINT64 start = ranges[i].start, end = ranges[i].end;

        for (j = i; j > 0 && ranges[j - 1].start > start; j--) {
            ranges[j].start = ranges[j - 1].start;
            ranges[j].end = ranges[j - 1].end;
        }
        ranges[j].start = start;
        ranges[j].end = end;
    }
    for (i = 0; i < count; i++) {
        if (merged > 0 && ranges[i].start <= ranges[merged - 1].end) {
            if (ranges[i].end > ranges[merged - 1].end)
                ranges[merged - 1].end = ranges[i].end;
        } else {
            ranges[merged].start = ranges[i].start;
            ranges[merged].end = ranges[i].end;
            merged++;
        }
    }
    return merged;
}

/* The fixed-range MTRRs give the first MiB memory types 4 KiB at a time,
   and a large page over memory the MTRRs give several types is undefined;
   SDM Vol. 3A 14.11.9 advises large pages only from 4 MiB up. */
#define LARGE_PAGES_START 0x400000ULL

/* The direct map from start to end, with the PTE bits memory_type for
   its memory type, in 4 KiB pages below LARGE_PAGES_START and the
   largest pages that fit above it. */
static EFI_STATUS map_direct_pages(PAGE_TABLES *tables, UINT64 start,
                                   UINT64 end, UINT64 memory_type)
{
    UINT64 split = start > LARGE_PAGES_START  ? start
                   : end < LARGE_PAGES_START ? end
                                             : LARGE_PAGES_START;
    UINT64 flags = PTE_WRITABLE | PTE_NO_EXECUTE | memory_type;
    EFI_STATUS status;

    status = tables_map(tables, DIRECT_MAP_BASE + start, start, split - start,
                        flags, PAGE_TABLE_LEVEL);
    if (status == EFI_SUCCESS)
        status = tables_map(tables, DIRECT_MAP_BASE + split, split,
                            end - split, flags, PDPT_LEVEL);
    return status;
}

static UINT64 clamp(UINT64 value, UINT64 low, UINT64 high)
{
    return value < low ? low : value > high ? high : value;
}

/* One merged range of the direct map: write-combining where it covers
   the framebuffer's pages and with no memory-type bits elsewhere. */
static EFI_STATUS map_direct_range(PAGE_TABLES *tables, const RANGE *range,
                                   const RANGE *framebuffer)
{
    UINT64 start = clamp(framebuffer->start, range->start, range->end);
    UINT64 end = clamp(framebuffer->end, range->start, range->end);
    EFI_STATUS status;

    status = map_direct_pages(tables, range->start, start, 0);
    if (status == EFI_SUCCESS)
        status = map_direct_pages(tables, start, end, PTE_WRITE_COMBINING);
    if (status == EFI_SUCCESS)
        status = map_direct_pages(tables, end, range->end, 0);
    return status;
}

/* Map every range the memory map describes, and the framebuffer, which
   it need not describe, at DIRECT_MAP_BASE plus its physical address.

   The map is taken here, before the final one. UEFI has it describe all
   memory present, however it is used (7.2.3), so the allocations after it
   change descriptor types, not the memory described. EDK II lists memory
   it has not tested as EfiReservedMemoryType and makes it conventional
   memory only when an allocation needs it
   (MdeModulePkg/Core/Dxe/Mem/Page.c, CoreGetMemoryMap and
   PromoteMemoryResource), so there too the ranges stay the same. A
   descriptor UEFI does not allow, unaligned or empty, is skipped, as the
   kernel's allocator skips it.

   The framebuffer's pages, its base and size rounded out to whole pages,
   select PAT_WRITE_COMBINING_ENTRY, which NOVA_PAT makes WC, and WC with
   any MTRR type is WC (SDM Vol. 3A Table 14-7): the type SDM Vol. 3A 14.3
   gives framebuffers, which combines writes to reduce memory accesses.
   The other entries leave PAT, PCD and PWT clear, as EDK II's identity map
   does (MdeModulePkg/Core/DxeIplPeim/X64/VirtualMemory.c), so a page gets
   the memory type PAT entry 0 and the MTRRs select together (SDM Vol. 3A
   5.9.2, Table 14-7). Above LARGE_PAGES_START either holds only where the
   variable MTRRs give each large page one type (SDM Vol. 3A 14.11.9),
   which nova-boot does not check; EDK II's identity map, of 2 MiB and
   1 GiB pages, assumes the same. */
static EFI_STATUS map_direct(EFI_BOOT_SERVICES *boot, PAGE_TABLES *tables,
                             const EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *screen)
{
    MEMORY_MAP map;
    RANGE *ranges = 0;
    RANGE framebuffer;
    UINTN offset, count = 0, i;
    EFI_STATUS status;

    map.buffer = 0;
    map.capacity = 0;
    map.descriptor_size = sizeof(EFI_MEMORY_DESCRIPTOR);
    status = take_map(boot, &map);
    if (status == EFI_SUCCESS)
        status = boot->AllocatePool(
            EFI_LOADER_DATA,
            (map.size / map.descriptor_size + 1) * sizeof(RANGE),
            (VOID **)&ranges);
    for (offset = 0; status == EFI_SUCCESS &&
                     offset + map.descriptor_size <= map.size;
         offset += map.descriptor_size) {
        const EFI_MEMORY_DESCRIPTOR *d =
            (const EFI_MEMORY_DESCRIPTOR *)(map.buffer + offset);

        if (d->PhysicalStart % PAGE_SIZE != 0 || d->NumberOfPages == 0)
            continue;
        status = d->NumberOfPages > DIRECT_MAP_SIZE / PAGE_SIZE
                     ? EFI_UNSUPPORTED
                     : add_range(ranges, &count, d->PhysicalStart,
                                 d->NumberOfPages * PAGE_SIZE);
    }
    if (status == EFI_SUCCESS)
        status = add_range(ranges, &count, screen->FrameBufferBase,
                           screen->FrameBufferSize);
    if (status == EFI_SUCCESS) {
        framebuffer.start = ranges[count - 1].start;
        framebuffer.end = ranges[count - 1].end;
        count = merge_ranges(ranges, count);
    }
    for (i = 0; status == EFI_SUCCESS && i < count; i++)
        status = map_direct_range(tables, &ranges[i], &framebuffer);
    if (ranges)
        boot->FreePool(ranges);
    if (map.buffer)
        boot->FreePool(map.buffer);
    return status;
}

/* Map the image page by page at KERNEL_IMAGE_BASE with the rights of the
   sections on the page: executable if one is IMAGE_SCN_MEM_EXECUTE,
   writable if one is IMAGE_SCN_MEM_WRITE (PE Format, Section Flags), and
   otherwise read-only and not executable, as the headers are. A page
   that would be both writable and executable fails the load. */
static EFI_STATUS map_kernel(PAGE_TABLES *tables, const KERNEL_IMAGE *kernel)
{
    UINT64 page;

    for (page = 0; page < kernel->pages; page++) {
        UINT64 start = page * PAGE_SIZE;
        UINT32 rights = 0, i;
        EFI_STATUS status;

        for (i = 0; i < kernel->section_count; i++) {
            const UINT8 *section =
                kernel->sections + (UINT64)i * SECTION_HEADER_SIZE;
            UINT64 address = read32(section + SECTION_ADDRESS);
            UINT64 size = read32(section + SECTION_VIRTUAL_SIZE);

            if (address < start + PAGE_SIZE && start < address + size)
                rights |= read32(section + SECTION_CHARACTERISTICS);
        }
        if ((rights & SECTION_WRITE) && (rights & SECTION_EXECUTE))
            return EFI_LOAD_ERROR;
        status = tables_map(tables, KERNEL_IMAGE_BASE + start,
                            kernel->base + start, PAGE_SIZE,
                            ((rights & SECTION_WRITE) ? PTE_WRITABLE : 0) |
                                ((rights & SECTION_EXECUTE) ? 0
                                                            : PTE_NO_EXECUTE),
                            PAGE_TABLE_LEVEL);
        if (status != EFI_SUCCESS)
            return status;
    }
    return EFI_SUCCESS;
}

/* boot_switch's pages at their own address, read-only and executable:
   the only lower-half mapping, which the kernel removes. */
static EFI_STATUS map_switch(PAGE_TABLES *tables)
{
    UINT64 first = (UINT64)boot_switch - (UINT64)boot_switch % PAGE_SIZE;
    UINT64 end = (UINT64)boot_switch_end + PAGE_SIZE - 1;

    return tables_map(tables, first, first, end - end % PAGE_SIZE - first, 0,
                      PAGE_TABLE_LEVEL);
}

static void free_address_space(EFI_BOOT_SERVICES *boot, ADDRESS_SPACE *space)
{
    tables_free(&space->tables);
    boot->FreePages(space->stack, KERNEL_STACK_SIZE / EFI_PAGE_SIZE);
}

/* Allocate the kernel's stack and build its tables, before
   ExitBootServices so that the allocations can still fail cleanly. */
static EFI_STATUS build_address_space(
    EFI_BOOT_SERVICES *boot, const KERNEL_IMAGE *kernel,
    const EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *screen, ADDRESS_SPACE *space)
{
    EFI_STATUS status;

    status = boot->AllocatePages(ALLOCATE_ANY_PAGES, EFI_LOADER_DATA,
                                 KERNEL_STACK_SIZE / EFI_PAGE_SIZE,
                                 &space->stack);
    if (status != EFI_SUCCESS)
        return status;
    status = tables_create(boot, &space->tables);
    if (status == EFI_SUCCESS)
        status = map_direct(boot, &space->tables, screen);
    if (status == EFI_SUCCESS)
        status = map_kernel(&space->tables, kernel);
    if (status == EFI_SUCCESS)
        status = tables_map(&space->tables, KERNEL_STACK_BASE, space->stack,
                            KERNEL_STACK_SIZE, PTE_WRITABLE | PTE_NO_EXECUTE,
                            PAGE_TABLE_LEVEL);
    if (status == EFI_SUCCESS)
        status = map_switch(&space->tables);
    if (status != EFI_SUCCESS)
        free_address_space(boot, space);
    return status;
}

/* ------------------------------------------------------------------------
   Random seeds */

static EFI_GUID rng_guid = {0x3152bca5, 0xeade, 0x433d,
                            {0x86, 0x2e, 0xc0, 0x1c, 0xdc, 0x29, 0x1f, 0x44}};

/* Ask each EFI_RNG_PROTOCOL instance (UEFI 37.5), up to
   BOOT_RANDOM_SEEDS of them, for a seed from its default algorithm, into
   BOOT_INFO. Every instance is asked, not just the first LocateProtocol
   finds, since an instance can be the processor's RDRAND, which the
   kernel reads itself. The kernel hashes the seeds with its own sources;
   nova-boot records what each call returned and boots with or without a
   seed. */
static void take_random_seeds(EFI_BOOT_SERVICES *boot)
{
    EFI_HANDLE *handles;
    UINTN count, i;

    if (boot->LocateHandleBuffer(BY_PROTOCOL, &rng_guid, 0, &count,
                                 &handles) != EFI_SUCCESS)
        return;
    if (count > BOOT_RANDOM_SEEDS)
        count = BOOT_RANDOM_SEEDS;
    for (i = 0; i < count; i++) {
        BOOT_RANDOM_SEED *seed = &boot_info.random_seeds[i];
        EFI_RNG_PROTOCOL *rng;

        seed->status =
            boot->HandleProtocol(handles[i], &rng_guid, (VOID **)&rng);
        if (seed->status == EFI_SUCCESS)
            seed->status =
                rng->GetRNG(rng, 0, sizeof(seed->bytes), seed->bytes);
    }
    boot_info.random_seed_count = count;
    boot->FreePool(handles);
}

/* ------------------------------------------------------------------------
   Leaving the firmware */

/* Take the final memory map into boot_info and exit boot services. If the
   map changed after it was taken, ExitBootServices refuses its key and it
   is taken again (UEFI 7.4.6). After the first ExitBootServices call only
   the memory allocation services remain, so from then on a failure halts
   rather than returning to the firmware. */
static EFI_STATUS exit_boot_services(EFI_HANDLE image,
                                     EFI_BOOT_SERVICES *boot)
{
    MEMORY_MAP map;
    EFI_STATUS status;
    int attempt;

    /* UEFI returns the descriptor size only with a map (7.2.3). */
    map.buffer = 0;
    map.capacity = 0;
    map.descriptor_size = sizeof(EFI_MEMORY_DESCRIPTOR);
    status = take_map(boot, &map);
    if (status != EFI_SUCCESS) {
        if (map.buffer)
            boot->FreePool(map.buffer);
        return status;
    }
    for (attempt = 0; attempt < EXIT_ATTEMPTS; attempt++) {
        if (boot->ExitBootServices(image, map.key) == EFI_SUCCESS) {
            boot_info.memory_map = direct_map((UINT64)map.buffer);
            boot_info.memory_map_size = map.size;
            boot_info.descriptor_size = map.descriptor_size;
            return EFI_SUCCESS;
        }
        if (take_map(boot, &map) != EFI_SUCCESS)
            break;
    }
    halt();
}

/* Fill in what BOOT_INFO still lacks, program the PAT and turn on the
   protections the kernel's tables rely on, and switch to them. UEFI
   leaves the interrupt flag after ExitBootServices unstated (7.4.6), and
   the firmware's IDT and handlers are in memory the kernel owns, which
   its tables do not map at those addresses. The framebuffer's entries
   select the PAT's WC entry, so IA32_PAT takes NOVA_PAT before the tables
   are loaded, and boot_switch then invalidates the translations and
   caches the change can leave stale (SDM Vol. 3A 14.12.4). XD is reserved
   until IA32_EFER.NXE is set (SDM Vol. 3A 5.5.4), and read-only pages
   bind supervisor writes only with CR0.WP (SDM Vol. 3A 5.1.3). */
_Noreturn static void enter_kernel(const KERNEL_IMAGE *kernel,
                                   const PAGE_TABLES *tables)
{
    disable_interrupts();
    boot_info.magic = BOOT_INFO_MAGIC;
    boot_info.version = BOOT_INFO_VERSION;
    boot_info.screen.pixels = direct_map((UINT64)boot_info.screen.pixels);
    wrmsr(IA32_PAT, NOVA_PAT);
    if (tables->no_execute)
        wrmsr(IA32_EFER, rdmsr(IA32_EFER) | EFER_NO_EXECUTE);
    write_cr0(read_cr0() | CR0_WRITE_PROTECT);
    boot_switch(tables->top, KERNEL_STACK_TOP, kernel->entry,
                direct_map((UINT64)&boot_info));
}

/* ------------------------------------------------------------------------ */

EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
    EFI_BOOT_SERVICES *boot = SystemTable->BootServices;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *mode;
    KERNEL_IMAGE kernel;
    ADDRESS_SPACE space;
    UINT8 *file, *init;
    UINTN file_size, init_size;
    UINT64 t0;
    EFI_STATUS status;

    /* The boot manager arms a five-minute watchdog before it starts an
       application (UEFI 2.11, 3.1.2). */
    boot->SetWatchdogTimer(0, 0, 0, 0);

    status = open_screen(boot, &gop);
    if (status != EFI_SUCCESS)
        return status;
    mode = gop->Mode->Info;
    if (!drawable(mode))
        return EFI_LOAD_ERROR;
    boot_info.screen.pixels = (UINT32 *)gop->Mode->FrameBufferBase;
    boot_info.screen.width = mode->HorizontalResolution;
    boot_info.screen.height = mode->VerticalResolution;
    boot_info.screen.stride = mode->PixelsPerScanLine;
    boot_info.screen.format = mode->PixelFormat;
    gfx_clear(&boot_info.screen);
    gfx_boot_screen(&boot_info.screen, &boot_info.spinner);
    gfx_spinner_step(&boot_info.screen, &boot_info.spinner);

    status = read_boot_file(ImageHandle, boot, kernel_path, &file,
                            &file_size);
    if (status != EFI_SUCCESS)
        return status;
    status = place_kernel(boot, file, file_size, &kernel);
    boot->FreePool(file);
    if (status != EFI_SUCCESS)
        return status;
    /* The kernel loads the first process's image itself; nova-boot hands
       it over as read, in memory the kernel keeps. */
    status = read_boot_file(ImageHandle, boot, init_path, &init, &init_size);
    if (status != EFI_SUCCESS) {
        boot->FreePages(kernel.base, kernel.pages);
        return status;
    }
    boot_info.init_image = direct_map((UINT64)init);
    boot_info.init_image_size = init_size;
    status = build_address_space(boot, &kernel, gop->Mode, &space);
    if (status != EFI_SUCCESS) {
        boot->FreePool(init);
        boot->FreePages(kernel.base, kernel.pages);
        return status;
    }
    gfx_spinner_step(&boot_info.screen, &boot_info.spinner);

    t0 = rdtsc();
    boot->Stall(CALIBRATION_MICROSECONDS);
    boot_info.tsc_per_ms = (rdtsc() - t0) / (CALIBRATION_MICROSECONDS / 1000);

    take_random_seeds(boot);
    status = exit_boot_services(ImageHandle, boot);
    if (status != EFI_SUCCESS) {
        free_address_space(boot, &space);
        boot->FreePool(init);
        boot->FreePages(kernel.base, kernel.pages);
        return status;
    }
    enter_kernel(&kernel, &space.tables);
}
