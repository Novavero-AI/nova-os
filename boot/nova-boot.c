/* nova-boot: the UEFI loader. It draws the boot screen, loads the kernel
   from \nova\kernel.exe on the volume it was booted from, measures the
   timestamp counter against Stall, takes the final memory map, exits boot
   services and jumps to the kernel's entry with a BOOT_INFO record. */

#include "efi.h"
#include "boot_info.h"

static EFI_GUID graphics_output_guid = {
    0x9042a9de, 0x23dc, 0x4a38,
    {0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a}};
static EFI_GUID loaded_image_guid = {
    0x5b1b31a1, 0x9562, 0x11d2,
    {0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};
static EFI_GUID simple_file_system_guid = {
    0x964e5b22, 0x6459, 0x11d2,
    {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};

static CHAR16 kernel_path[] = L"\\nova\\kernel.exe";

/* The largest mode the screen is switched to. */
#define MAX_WIDTH 1920
#define MAX_HEIGHT 1080

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

typedef VOID (*KERNEL_ENTRY)(BOOT_INFO *info);

typedef struct {
    EFI_PHYSICAL_ADDRESS base;
    UINTN pages;
    KERNEL_ENTRY entry;
} KERNEL_IMAGE;

static BOOT_INFO boot_info;

static UINT64 rdtsc(void)
{
    UINT32 lo, hi;

    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (UINT64)hi << 32 | lo;
}

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

/* Switch to the largest drawable mode within MAX_WIDTH x MAX_HEIGHT, if
   it is larger than the current mode or the current mode is not
   drawable. */
static void largest_mode(EFI_BOOT_SERVICES *boot,
                         EFI_GRAPHICS_OUTPUT_PROTOCOL *gop)
{
    UINT32 mode, best = gop->Mode->Mode;
    INT64 best_area = drawable(gop->Mode->Info)
                          ? (INT64)gop->Mode->Info->HorizontalResolution *
                                gop->Mode->Info->VerticalResolution
                          : 0;

    for (mode = 0; mode < gop->Mode->MaxMode; mode++) {
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info;
        UINTN size;
        INT64 area;

        if (gop->QueryMode(gop, mode, &size, &info) != EFI_SUCCESS)
            continue;
        area = (INT64)info->HorizontalResolution * info->VerticalResolution;
        if (info->HorizontalResolution <= MAX_WIDTH &&
            info->VerticalResolution <= MAX_HEIGHT && area > best_area &&
            drawable(info)) {
            best = mode;
            best_area = area;
        }
        boot->FreePool(info);
    }
    if (best != gop->Mode->Mode)
        gop->SetMode(gop, best);
}

/* ------------------------------------------------------------------------
   Loading the kernel: a PE32+ image (PE Format), copied section by section
   into fresh pages and relocated to where those pages are. Every offset
   and size the file gives is checked against the file or the image before
   it is used. */

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

static EFI_STATUS read_kernel(EFI_HANDLE image, EFI_BOOT_SERVICES *boot,
                              UINT8 **data, UINTN *size)
{
    EFI_LOADED_IMAGE_PROTOCOL *loaded;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *volume;
    EFI_FILE_PROTOCOL *root, *kernel;
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
    status = root->Open(root, &kernel, kernel_path, EFI_FILE_MODE_READ, 0);
    if (status == EFI_SUCCESS) {
        status = read_file(boot, kernel, data, size);
        kernel->Close(kernel);
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
   pages and apply its base relocations. */
static EFI_STATUS place_kernel(EFI_BOOT_SERVICES *boot, const UINT8 *file,
                               UINTN size, KERNEL_IMAGE *kernel)
{
    const UINT8 *nt, *opt, *sections;
    UINT32 nt_offset, optional_size, sections_count, image_size,
        headers_size, entry, i, offset;
    UINT32 reloc_rva = 0, reloc_size = 0;
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
    image_size = read32(opt + OPT_IMAGE_SIZE);
    headers_size = read32(opt + OPT_HEADERS_SIZE);
    entry = read32(opt + OPT_ENTRY);
    if (headers_size > image_size || headers_size > size || entry == 0 ||
        entry >= image_size || !within(reloc_rva, reloc_size, image_size))
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
                      kernel->base - read64(opt + OPT_IMAGE_BASE));
    if (status != EFI_SUCCESS) {
        boot->FreePages(kernel->base, kernel->pages);
        return status;
    }
    kernel->entry = (KERNEL_ENTRY)(image + entry);
    return EFI_SUCCESS;
}

/* ------------------------------------------------------------------------
   Leaving the firmware */

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
            return version == EFI_MEMORY_DESCRIPTOR_VERSION
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
            boot_info.memory_map = map.buffer;
            boot_info.memory_map_size = map.size;
            boot_info.descriptor_size = map.descriptor_size;
            return EFI_SUCCESS;
        }
        if (take_map(boot, &map) != EFI_SUCCESS)
            break;
    }
    halt();
}

/* ------------------------------------------------------------------------ */

EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
    EFI_BOOT_SERVICES *boot = SystemTable->BootServices;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *mode;
    KERNEL_IMAGE kernel;
    UINT8 *file;
    UINTN file_size;
    UINT64 t0;
    EFI_STATUS status;

    /* The boot manager arms a five-minute watchdog before it starts an
       application (UEFI 2.11, 3.1.2). */
    boot->SetWatchdogTimer(0, 0, 0, 0);

    status = boot->LocateProtocol(&graphics_output_guid, 0, (VOID **)&gop);
    if (status != EFI_SUCCESS)
        return status;
    largest_mode(boot, gop);
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

    status = read_kernel(ImageHandle, boot, &file, &file_size);
    if (status != EFI_SUCCESS)
        return status;
    status = place_kernel(boot, file, file_size, &kernel);
    boot->FreePool(file);
    if (status != EFI_SUCCESS)
        return status;
    gfx_spinner_step(&boot_info.screen, &boot_info.spinner);

    t0 = rdtsc();
    boot->Stall(CALIBRATION_MICROSECONDS);
    boot_info.tsc_per_ms = (rdtsc() - t0) / (CALIBRATION_MICROSECONDS / 1000);

    status = exit_boot_services(ImageHandle, boot);
    if (status != EFI_SUCCESS) {
        boot->FreePages(kernel.base, kernel.pages);
        return status;
    }

    /* UEFI leaves the interrupt flag after ExitBootServices unstated
       (7.4.6), and the firmware's handlers sit in memory the kernel now
       owns. */
    __asm__ volatile("cli");
    kernel.entry(&boot_info);
    halt();
}
