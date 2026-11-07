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
#define FRAME_MICROSECONDS 33000
#define EXIT_ATTEMPTS 3

typedef VOID (*KERNEL_ENTRY)(BOOT_INFO *info);

static BOOT_INFO boot_info;

static UINT64 rdtsc(void)
{
    UINT32 lo, hi;

    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (UINT64)hi << 32 | lo;
}

/* Switch to the largest mode within MAX_WIDTH x MAX_HEIGHT that has a
   linear 32-bit framebuffer, if it is larger than the current one. */
static void largest_mode(EFI_BOOT_SERVICES *boot,
                         EFI_GRAPHICS_OUTPUT_PROTOCOL *gop)
{
    UINT32 mode, best = gop->Mode->Mode;
    INT64 best_area = (INT64)gop->Mode->Info->HorizontalResolution *
                      gop->Mode->Info->VerticalResolution;

    for (mode = 0; mode < gop->Mode->MaxMode; mode++) {
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info;
        UINTN size;
        INT64 area;

        if (gop->QueryMode(gop, mode, &size, &info) != EFI_SUCCESS)
            continue;
        area = (INT64)info->HorizontalResolution * info->VerticalResolution;
        if (info->HorizontalResolution <= MAX_WIDTH &&
            info->VerticalResolution <= MAX_HEIGHT && area > best_area &&
            (info->PixelFormat == PIXEL_RGB ||
             info->PixelFormat == PIXEL_BGR)) {
            best = mode;
            best_area = area;
        }
        boot->FreePool(info);
    }
    if (best != gop->Mode->Mode)
        gop->SetMode(gop, best);
}

/* ------------------------------------------------------------------------
   Loading the kernel: a PE32+ image (PE/COFF format), copied section by
   section into fresh pages and relocated to where those pages are. */

#define PE_MACHINE_AMD64 0x8664
#define PE32_PLUS 0x20b
#define REL_BASED_ABSOLUTE 0
#define REL_BASED_DIR64 10

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

static EFI_STATUS read_kernel(EFI_HANDLE image, EFI_BOOT_SERVICES *boot,
                              UINT8 **file, UINTN *size)
{
    EFI_LOADED_IMAGE_PROTOCOL *loaded;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *volume;
    EFI_FILE_PROTOCOL *root, *kernel;
    UINT64 end;
    UINTN read;
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
        kernel->SetPosition(kernel, EFI_FILE_END);
        kernel->GetPosition(kernel, &end);
        kernel->SetPosition(kernel, 0);
        *size = end;
        status = boot->AllocatePool(EFI_LOADER_DATA, *size, (VOID **)file);
        if (status == EFI_SUCCESS) {
            read = *size;
            status = kernel->Read(kernel, &read, *file);
            if (status == EFI_SUCCESS && read != *size)
                status = EFI_LOAD_ERROR;
            if (status != EFI_SUCCESS)
                boot->FreePool(*file);
        }
        kernel->Close(kernel);
    }
    root->Close(root);
    return status;
}

/* Lay the image out at its section addresses in fresh pages and apply its
   base relocations; the entry point's address goes to *entry. */
static EFI_STATUS place_kernel(EFI_BOOT_SERVICES *boot, const UINT8 *file,
                               UINTN size, KERNEL_ENTRY *entry)
{
    const UINT8 *nt, *opt, *sections;
    UINT32 sections_count, image_size, headers_size, i;
    UINT64 linked_base, delta;
    EFI_PHYSICAL_ADDRESS base;
    UINT8 *image;
    UINT32 reloc_rva, reloc_size, offset;
    EFI_STATUS status;

    if (size < 0x40 || file[0] != 'M' || file[1] != 'Z')
        return EFI_LOAD_ERROR;
    nt = file + read32(file + 0x3c);
    if (nt + 24 > file + size || read32(nt) != 0x00004550 ||
        read16(nt + 4) != PE_MACHINE_AMD64)
        return EFI_LOAD_ERROR;
    sections_count = read16(nt + 6);
    opt = nt + 24;
    if (read16(opt) != PE32_PLUS)
        return EFI_LOAD_ERROR;
    sections = opt + read16(nt + 20);
    linked_base = read64(opt + 24);
    image_size = read32(opt + 56);
    headers_size = read32(opt + 60);
    reloc_rva = read32(opt + 112 + 5 * 8);
    reloc_size = read32(opt + 112 + 5 * 8 + 4);

    status = boot->AllocatePages(ALLOCATE_ANY_PAGES, EFI_LOADER_CODE,
                                 (image_size + EFI_PAGE_SIZE - 1) /
                                     EFI_PAGE_SIZE,
                                 &base);
    if (status != EFI_SUCCESS)
        return status;
    image = (UINT8 *)base;
    for (offset = 0; offset < image_size; offset++)
        image[offset] = 0;
    for (offset = 0; offset < headers_size && offset < size; offset++)
        image[offset] = file[offset];
    for (i = 0; i < sections_count; i++) {
        const UINT8 *section = sections + i * 40;
        UINT32 virtual_size = read32(section + 8);
        UINT32 address = read32(section + 12);
        UINT32 raw_size = read32(section + 16);
        UINT32 raw_offset = read32(section + 20);
        UINT32 n = raw_size < virtual_size ? raw_size : virtual_size;

        for (offset = 0; offset < n; offset++)
            image[address + offset] = file[raw_offset + offset];
    }

    /* Each block covers a 4 KiB page: its address, its size, then 16-bit
       entries of a 4-bit type and a 12-bit offset into the page. */
    delta = base - linked_base;
    for (offset = 0; offset < reloc_size;) {
        const UINT8 *block = image + reloc_rva + offset;
        UINT32 page = read32(block), block_size = read32(block + 4), e;

        if (block_size < 8)
            break;
        for (e = 8; e + 2 <= block_size; e += 2) {
            UINT32 entry_value = read16(block + e);
            UINT32 type = entry_value >> 12;

            if (type == REL_BASED_DIR64)
                *(UINT64 *)(image + page + (entry_value & 0xfff)) += delta;
            else if (type != REL_BASED_ABSOLUTE)
                return EFI_LOAD_ERROR;
        }
        offset += block_size;
    }
    *entry = (KERNEL_ENTRY)(image + read32(opt + 16));
    return EFI_SUCCESS;
}

/* ------------------------------------------------------------------------ */

EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
    EFI_BOOT_SERVICES *boot = SystemTable->BootServices;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *mode;
    KERNEL_ENTRY entry;
    UINT8 *file, *map = 0;
    UINTN file_size, map_size = 0, map_key, descriptor_size;
    UINT32 descriptor_version;
    UINT64 t0;
    int attempt;
    EFI_STATUS status;

    /* The boot manager arms a five-minute watchdog before it starts an
       application (UEFI 2.10, 3.1.2). */
    boot->SetWatchdogTimer(0, 0, 0, 0);

    status = boot->LocateProtocol(&graphics_output_guid, 0, (VOID **)&gop);
    if (status != EFI_SUCCESS)
        return status;
    largest_mode(boot, gop);
    mode = gop->Mode->Info;
    if (mode->PixelFormat != PIXEL_RGB && mode->PixelFormat != PIXEL_BGR)
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
    status = place_kernel(boot, file, file_size, &entry);
    boot->FreePool(file);
    if (status != EFI_SUCCESS)
        return status;
    gfx_spinner_step(&boot_info.screen, &boot_info.spinner);

    t0 = rdtsc();
    boot->Stall(CALIBRATION_MICROSECONDS);
    boot_info.tsc_per_ms = (rdtsc() - t0) / (CALIBRATION_MICROSECONDS / 1000);

    /* The map grows by a descriptor or two when its own buffer is
       allocated, so the buffer gets room for a few more. A map taken
       before any later allocation is the one ExitBootServices accepts; if
       it changed regardless, take it again. */
    boot->GetMemoryMap(&map_size, 0, &map_key, &descriptor_size,
                       &descriptor_version);
    map_size += 4 * descriptor_size;
    status = boot->AllocatePool(EFI_LOADER_DATA, map_size, (VOID **)&map);
    if (status != EFI_SUCCESS)
        return status;
    for (attempt = 0; attempt < EXIT_ATTEMPTS; attempt++) {
        UINTN size = map_size;

        status = boot->GetMemoryMap(&size, map, &map_key, &descriptor_size,
                                    &descriptor_version);
        if (status != EFI_SUCCESS)
            return status;
        boot_info.memory_map = map;
        boot_info.memory_map_size = size;
        boot_info.descriptor_size = descriptor_size;
        if (boot->ExitBootServices(ImageHandle, map_key) == EFI_SUCCESS) {
            entry(&boot_info);
            for (;;)
                __asm__ volatile("hlt");
        }
    }
    return EFI_LOAD_ERROR;
}
