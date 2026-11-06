/* A UEFI application that draws nova-os's first screen through the Graphics
   Output Protocol: a vertical gradient, "nova-os" in a 5x7 pixel font, and
   an accent bar, written straight into the framebuffer. It then waits for a
   key and returns EFI_SUCCESS.

   Types and layouts are written out from the UEFI Specification 2.10:
     2.3.1  common data types; 2.3.4 x64 calling convention
     4.3.1  EFI_SYSTEM_TABLE; 4.4.1 EFI_BOOT_SERVICES
     7.1.5  WaitForEvent; 7.3.16 LocateProtocol; 7.5.1 SetWatchdogTimer
     12.3.1 EFI_SIMPLE_TEXT_INPUT_PROTOCOL
     12.9   EFI_GRAPHICS_OUTPUT_PROTOCOL
   A table member this program never calls is declared VOID * so the offsets
   of the members it does call stay right; the static assertions pin those
   offsets to the specification's. Drawing uses integer arithmetic only:
   the image links no C runtime, and TinyCC compiles floating-point
   conversions as calls into one. */

#include <stddef.h>

typedef unsigned char UINT8;
typedef unsigned short UINT16;
typedef unsigned int UINT32;
typedef unsigned long long UINT64; /* x86_64 PE is LLP64: long is 32 bits */
typedef UINT64 UINTN;
typedef void VOID;
typedef UINTN EFI_STATUS;
typedef VOID *EFI_HANDLE;
typedef VOID *EFI_EVENT;
typedef UINT64 EFI_PHYSICAL_ADDRESS;

#define EFI_SUCCESS 0

typedef struct {
    UINT32 Data1;
    UINT16 Data2;
    UINT16 Data3;
    UINT8 Data4[8];
} EFI_GUID;

typedef struct {
    UINT64 Signature;
    UINT32 Revision;
    UINT32 HeaderSize;
    UINT32 CRC32;
    UINT32 Reserved;
} EFI_TABLE_HEADER;

typedef struct {
    VOID *Reset;
    VOID *ReadKeyStroke;
    EFI_EVENT WaitForKey;
} EFI_SIMPLE_TEXT_INPUT_PROTOCOL;

typedef EFI_STATUS (*EFI_WAIT_FOR_EVENT)(UINTN NumberOfEvents,
                                         EFI_EVENT *Event, UINTN *Index);
typedef EFI_STATUS (*EFI_SET_WATCHDOG_TIMER)(UINTN Timeout,
                                             UINT64 WatchdogCode,
                                             UINTN DataSize, UINT16 *Data);
typedef EFI_STATUS (*EFI_LOCATE_PROTOCOL)(EFI_GUID *Protocol,
                                          VOID *Registration,
                                          VOID **Interface);

/* A prefix of the table, through LocateProtocol, the last member this
   program calls; it is only ever reached through the firmware's pointer. */
typedef struct {
    EFI_TABLE_HEADER Hdr;
    VOID *RaiseTPL;
    VOID *RestoreTPL;
    VOID *AllocatePages;
    VOID *FreePages;
    VOID *GetMemoryMap;
    VOID *AllocatePool;
    VOID *FreePool;
    VOID *CreateEvent;
    VOID *SetTimer;
    EFI_WAIT_FOR_EVENT WaitForEvent;
    VOID *SignalEvent;
    VOID *CloseEvent;
    VOID *CheckEvent;
    VOID *InstallProtocolInterface;
    VOID *ReinstallProtocolInterface;
    VOID *UninstallProtocolInterface;
    VOID *HandleProtocol;
    VOID *Reserved;
    VOID *RegisterProtocolNotify;
    VOID *LocateHandle;
    VOID *LocateDevicePath;
    VOID *InstallConfigurationTable;
    VOID *LoadImage;
    VOID *StartImage;
    VOID *Exit;
    VOID *UnloadImage;
    VOID *ExitBootServices;
    VOID *GetNextMonotonicCount;
    VOID *Stall;
    EFI_SET_WATCHDOG_TIMER SetWatchdogTimer;
    VOID *ConnectController;
    VOID *DisconnectController;
    VOID *OpenProtocol;
    VOID *CloseProtocol;
    VOID *OpenProtocolInformation;
    VOID *ProtocolsPerHandle;
    VOID *LocateHandleBuffer;
    EFI_LOCATE_PROTOCOL LocateProtocol;
} EFI_BOOT_SERVICES_PREFIX;

_Static_assert(offsetof(EFI_BOOT_SERVICES_PREFIX, WaitForEvent) == 0x60,
               "WaitForEvent is the tenth service after the header");
_Static_assert(offsetof(EFI_BOOT_SERVICES_PREFIX, LocateProtocol) == 0x140,
               "LocateProtocol is the thirty-eighth service after the header");

typedef struct {
    EFI_TABLE_HEADER Hdr;
    UINT16 *FirmwareVendor;
    UINT32 FirmwareRevision;
    EFI_HANDLE ConsoleInHandle;
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL *ConIn;
    EFI_HANDLE ConsoleOutHandle;
    VOID *ConOut;
    EFI_HANDLE StandardErrorHandle;
    VOID *StdErr;
    VOID *RuntimeServices;
    EFI_BOOT_SERVICES_PREFIX *BootServices;
    UINTN NumberOfTableEntries;
    VOID *ConfigurationTable;
} EFI_SYSTEM_TABLE;

/* EFI_GRAPHICS_PIXEL_FORMAT */
#define PIXEL_RGB 0 /* PixelRedGreenBlueReserved8BitPerColor */
#define PIXEL_BGR 1 /* PixelBlueGreenRedReserved8BitPerColor */

typedef struct {
    UINT32 RedMask;
    UINT32 GreenMask;
    UINT32 BlueMask;
    UINT32 ReservedMask;
} EFI_PIXEL_BITMASK;

typedef struct {
    UINT32 Version;
    UINT32 HorizontalResolution;
    UINT32 VerticalResolution;
    UINT32 PixelFormat;
    EFI_PIXEL_BITMASK PixelInformation;
    UINT32 PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
    UINT32 MaxMode;
    UINT32 Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
    UINTN SizeOfInfo;
    EFI_PHYSICAL_ADDRESS FrameBufferBase;
    UINTN FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

typedef struct {
    VOID *QueryMode;
    VOID *SetMode;
    VOID *Blt;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode;
} EFI_GRAPHICS_OUTPUT_PROTOCOL;

static EFI_GUID graphics_output_guid = {
    0x9042a9de, 0x23dc, 0x4a38,
    {0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a}};

/* The screen as this program draws it: a 32-bit framebuffer whose pixels
   hold red, green and blue in the byte order its format names. */
typedef struct {
    UINT32 *pixels;
    UINT32 width;
    UINT32 height;
    UINT32 stride;
    UINT32 format;
} SCREEN;

static UINT32 pack(const SCREEN *screen, UINT32 r, UINT32 g, UINT32 b)
{
    if (screen->format == PIXEL_BGR)
        return r << 16 | g << 8 | b;
    return b << 16 | g << 8 | r;
}

static void fill(const SCREEN *screen, UINT32 x, UINT32 y, UINT32 w, UINT32 h,
                 UINT32 color)
{
    UINT32 row, col;

    for (row = y; row < y + h && row < screen->height; row++)
        for (col = x; col < x + w && col < screen->width; col++)
            screen->pixels[row * screen->stride + col] = color;
}

/* Linear interpolation between two 8-bit channels, at step of steps. */
static UINT32 mix(UINT32 from, UINT32 to, UINT32 step, UINT32 steps)
{
    return (from * (steps - step) + to * step) / steps;
}

/* "nova-os" in a 5x7 font: one byte per row, bit 4 the leftmost column. */
static const UINT8 glyphs[7][7] = {
    {0x00, 0x00, 0x16, 0x19, 0x11, 0x11, 0x11}, /* n */
    {0x00, 0x00, 0x0E, 0x11, 0x11, 0x11, 0x0E}, /* o */
    {0x00, 0x00, 0x11, 0x11, 0x11, 0x0A, 0x04}, /* v */
    {0x00, 0x00, 0x0E, 0x01, 0x0F, 0x11, 0x0F}, /* a */
    {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}, /* - */
    {0x00, 0x00, 0x0E, 0x11, 0x11, 0x11, 0x0E}, /* o */
    {0x00, 0x00, 0x0F, 0x10, 0x0E, 0x01, 0x1E}, /* s */
};
#define GLYPHS 7
#define GLYPH_WIDTH 5
#define GLYPH_HEIGHT 7
#define GLYPH_ADVANCE 6 /* the glyph and one blank column */

static void draw(const SCREEN *screen)
{
    UINT32 row, g, glyph_row, col;
    UINT32 scale = screen->width / 80;
    UINT32 text_width, text_x, text_y, bar_y;

    for (row = 0; row < screen->height; row++)
        fill(screen, 0, row, screen->width, 1,
             pack(screen, mix(12, 52, row, screen->height),
                  mix(14, 18, row, screen->height),
                  mix(42, 92, row, screen->height)));

    if (scale < 2)
        scale = 2;
    text_width = (GLYPHS * GLYPH_ADVANCE - 1) * scale;
    text_x = (screen->width - text_width) / 2;
    text_y = (screen->height - GLYPH_HEIGHT * scale) / 2;
    for (g = 0; g < GLYPHS; g++)
        for (glyph_row = 0; glyph_row < GLYPH_HEIGHT; glyph_row++)
            for (col = 0; col < GLYPH_WIDTH; col++)
                if (glyphs[g][glyph_row] >> (GLYPH_WIDTH - 1 - col) & 1)
                    fill(screen, text_x + (g * GLYPH_ADVANCE + col) * scale,
                         text_y + glyph_row * scale, scale, scale,
                         pack(screen, 236, 238, 255));

    bar_y = text_y + (GLYPH_HEIGHT + 3) * scale;
    for (col = 0; col < text_width; col++)
        fill(screen, text_x + col, bar_y, 1, scale / 2 + 1,
             pack(screen, mix(64, 236, col, text_width),
                  mix(214, 72, col, text_width), 255));
}

EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
    EFI_BOOT_SERVICES_PREFIX *boot = SystemTable->BootServices;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info;
    SCREEN screen;
    UINTN index;
    EFI_STATUS status;

    (void)ImageHandle;
    /* The boot manager arms a five-minute watchdog before it starts an
       application (UEFI 2.10, 3.1.2); this one waits on a person. */
    boot->SetWatchdogTimer(0, 0, 0, 0);

    status = boot->LocateProtocol(&graphics_output_guid, 0, (VOID **)&gop);
    if (status != EFI_SUCCESS)
        return status;
    info = gop->Mode->Info;
    if (info->PixelFormat == PIXEL_RGB || info->PixelFormat == PIXEL_BGR) {
        screen.pixels = (UINT32 *)gop->Mode->FrameBufferBase;
        screen.width = info->HorizontalResolution;
        screen.height = info->VerticalResolution;
        screen.stride = info->PixelsPerScanLine;
        screen.format = info->PixelFormat;
        draw(&screen);
    }

    boot->WaitForEvent(1, &SystemTable->ConIn->WaitForKey, &index);
    return EFI_SUCCESS;
}
