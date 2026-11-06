/* nova-os's boot screen: on black, the wordmark "nova" and a spinning arc,
   drawn into the Graphics Output Protocol's framebuffer until a key is
   pressed.

   Types and layouts follow the UEFI Specification 2.10: 2.3 (data types,
   x64 calling convention), 4.3.1 (EFI_SYSTEM_TABLE), 4.4.1
   (EFI_BOOT_SERVICES), 7.1 (CheckEvent), 7.2.4 (FreePool), 7.3.16
   (LocateProtocol), 7.5 (SetWatchdogTimer, Stall), 12.3.1 (simple text
   input) and 12.9 (EFI_GRAPHICS_OUTPUT_PROTOCOL). Members this program
   never calls are VOID * so the offsets of those it does call stay right;
   the static assertions pin them.

   The image links no C runtime, and TinyCC compiles floating-point
   conversions as calls into one, so all arithmetic is integer. Geometry is
   in eighths of a pixel, and a pixel's coverage is how many of 16 sample
   points fall inside a stroke. */

#include <stddef.h>

typedef unsigned char UINT8;
typedef unsigned short UINT16;
typedef unsigned int UINT32;
typedef unsigned long long UINT64; /* x86_64 PE is LLP64: long is 32 bits */
typedef long long INT64;
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

typedef EFI_STATUS (*EFI_FREE_POOL)(VOID *Buffer);
typedef EFI_STATUS (*EFI_CHECK_EVENT)(EFI_EVENT Event);
typedef EFI_STATUS (*EFI_STALL)(UINTN Microseconds);
typedef EFI_STATUS (*EFI_SET_WATCHDOG_TIMER)(UINTN Timeout,
                                             UINT64 WatchdogCode,
                                             UINTN DataSize, UINT16 *Data);
typedef EFI_STATUS (*EFI_LOCATE_PROTOCOL)(EFI_GUID *Protocol,
                                          VOID *Registration,
                                          VOID **Interface);

/* The table through LocateProtocol, the last member this program calls. */
typedef struct {
    EFI_TABLE_HEADER Hdr;
    VOID *RaiseTPL;
    VOID *RestoreTPL;
    VOID *AllocatePages;
    VOID *FreePages;
    VOID *GetMemoryMap;
    VOID *AllocatePool;
    EFI_FREE_POOL FreePool;
    VOID *CreateEvent;
    VOID *SetTimer;
    VOID *WaitForEvent;
    VOID *SignalEvent;
    VOID *CloseEvent;
    EFI_CHECK_EVENT CheckEvent;
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
    EFI_STALL Stall;
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

_Static_assert(offsetof(EFI_BOOT_SERVICES_PREFIX, FreePool) == 0x48, "");
_Static_assert(offsetof(EFI_BOOT_SERVICES_PREFIX, CheckEvent) == 0x78, "");
_Static_assert(offsetof(EFI_BOOT_SERVICES_PREFIX, Stall) == 0xF8, "");
_Static_assert(offsetof(EFI_BOOT_SERVICES_PREFIX, LocateProtocol) == 0x140,
               "");

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

typedef struct EFI_GRAPHICS_OUTPUT_PROTOCOL EFI_GRAPHICS_OUTPUT_PROTOCOL;

struct EFI_GRAPHICS_OUTPUT_PROTOCOL {
    EFI_STATUS (*QueryMode)(EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
                            UINT32 ModeNumber, UINTN *SizeOfInfo,
                            EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **Info);
    EFI_STATUS (*SetMode)(EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
                          UINT32 ModeNumber);
    VOID *Blt;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode;
};

static EFI_GUID graphics_output_guid = {
    0x9042a9de, 0x23dc, 0x4a38,
    {0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a}};

/* The largest mode the screen is switched to. */
#define MAX_WIDTH 1920
#define MAX_HEIGHT 1080

#define SUBPIXEL 8
#define SAMPLES 4
#define COVERAGE_MAX (SAMPLES * SAMPLES)

typedef struct {
    UINT32 *pixels;
    INT64 width;
    INT64 height;
    INT64 stride;
    UINT32 format;
} SCREEN;

typedef struct {
    INT64 r, g, b;
} RGB;

static const RGB ink = {242, 242, 246};

/* The ink at a brightness of 0..255 times coverage, over black. */
static void put(const SCREEN *screen, INT64 x, INT64 y, INT64 brightness,
                INT64 coverage)
{
    INT64 a = brightness * coverage / COVERAGE_MAX;
    INT64 r = ink.r * a / 255, g = ink.g * a / 255, b = ink.b * a / 255;

    if (x < 0 || y < 0 || x >= screen->width || y >= screen->height)
        return;
    screen->pixels[y * screen->stride + x] =
        (UINT32)(screen->format == PIXEL_BGR ? r << 16 | g << 8 | b
                                             : b << 16 | g << 8 | r);
}

/* ------------------------------------------------------------------------
   Strokes */

#define CAPSULE 0 /* the segment (ax, ay)-(bx, by), radius r */
#define RING 1    /* centre (ax, ay), radius bx, half-width r */
#define ARCH 2    /* the upper half of a RING */

typedef struct {
    int kind;
    INT64 ax, ay, bx, by, r;
} SHAPE;

static int inside(const SHAPE *s, INT64 px, INT64 py)
{
    INT64 qx = px - s->ax, qy = py - s->ay;

    if (s->kind == CAPSULE) {
        INT64 dx = s->bx - s->ax, dy = s->by - s->ay;
        INT64 dot = qx * dx + qy * dy, len2 = dx * dx + dy * dy;
        INT64 cross;

        if (dot <= 0)
            return qx * qx + qy * qy <= s->r * s->r;
        if (dot >= len2) {
            INT64 ex = px - s->bx, ey = py - s->by;
            return ex * ex + ey * ey <= s->r * s->r;
        }
        /* The squared distance to the line is cross^2 / len2. */
        cross = qx * dy - qy * dx;
        return cross * cross <= s->r * s->r * len2;
    } else {
        INT64 d2 = qx * qx + qy * qy;
        INT64 lo = s->bx - s->r, hi = s->bx + s->r;

        if (s->kind == ARCH && py > s->ay)
            return 0;
        if (lo < 0)
            lo = 0;
        return d2 >= lo * lo && d2 <= hi * hi;
    }
}

static INT64 coverage(const SHAPE *shapes, int count, INT64 x, INT64 y)
{
    INT64 sx, sy, hits = 0;
    int i;

    for (sy = 0; sy < SAMPLES; sy++)
        for (sx = 0; sx < SAMPLES; sx++) {
            INT64 px = x * SUBPIXEL + sx * (SUBPIXEL / SAMPLES) + 1;
            INT64 py = y * SUBPIXEL + sy * (SUBPIXEL / SAMPLES) + 1;

            for (i = 0; i < count; i++)
                if (inside(&shapes[i], px, py)) {
                    hits++;
                    break;
                }
        }
    return hits;
}

/* ------------------------------------------------------------------------
   The wordmark, in geometric strokes: x-height h, stroke radius r. Each
   letter adds its strokes with its top-left corner at (x, y) and returns
   its width. */

#define MAX_SHAPES 8
#define LETTER_GAP 32 /* percent of h */

static SHAPE wordmark[MAX_SHAPES];
static int wordmark_count;

static void add(int kind, INT64 ax, INT64 ay, INT64 bx, INT64 by, INT64 r)
{
    SHAPE *s = &wordmark[wordmark_count++];

    s->kind = kind;
    s->ax = ax;
    s->ay = ay;
    s->bx = bx;
    s->by = by;
    s->r = r;
}

static INT64 letter_n(INT64 x, INT64 y, INT64 h, INT64 r)
{
    INT64 w = h * 82 / 100, arc = w / 2 - r;

    add(CAPSULE, x + r, y + r, x + r, y + h - r, r);
    add(ARCH, x + w / 2, y + r + arc, arc, 0, r);
    add(CAPSULE, x + w - r, y + r + arc, x + w - r, y + h - r, r);
    return w;
}

static INT64 letter_o(INT64 x, INT64 y, INT64 h, INT64 r)
{
    add(RING, x + h / 2, y + h / 2, h / 2 - r, 0, r);
    return h;
}

static INT64 letter_v(INT64 x, INT64 y, INT64 h, INT64 r)
{
    INT64 w = h * 90 / 100;

    add(CAPSULE, x + r, y + r, x + w / 2, y + h - r, r);
    add(CAPSULE, x + w / 2, y + h - r, x + w - r, y + r, r);
    return w;
}

static INT64 letter_a(INT64 x, INT64 y, INT64 h, INT64 r)
{
    add(RING, x + h / 2, y + h / 2, h / 2 - r, 0, r);
    add(CAPSULE, x + h - r, y + r, x + h - r, y + h - r, r);
    return h;
}

static INT64 wordmark_width(INT64 h)
{
    return h * 82 / 100 + h + h * 90 / 100 + h + 3 * (h * LETTER_GAP / 100);
}

static void draw_wordmark(const SCREEN *screen, INT64 left, INT64 top,
                          INT64 h)
{
    INT64 r = h / 13, x = left, y;
    INT64 right = (left + wordmark_width(h)) / SUBPIXEL + 1;

    x += letter_n(x, top, h, r) + h * LETTER_GAP / 100;
    x += letter_o(x, top, h, r) + h * LETTER_GAP / 100;
    x += letter_v(x, top, h, r) + h * LETTER_GAP / 100;
    letter_a(x, top, h, r);
    for (y = top / SUBPIXEL; y <= (top + h) / SUBPIXEL + 1; y++)
        for (x = left / SUBPIXEL - 1; x <= right; x++)
            put(screen, x, y, 255,
                coverage(wordmark, wordmark_count, x, y));
}

/* ------------------------------------------------------------------------
   The spinner: a thin ring whose bright head sweeps clockwise, trailed by
   a fading tail, over a faint track. Angles are in sixteenths of a degree,
   clockwise from twelve o'clock. */

#define TURN (360 * 16)
#define TAIL (TURN * 3 / 4)
#define STEP (TURN / 48)  /* the head's advance per frame */
#define TRACK 22          /* the track's brightness, 0..255 */
#define FRAME_MICROSECONDS 33000

/* atan of z/4096 for 0 <= z <= 4096, in sixteenths of a degree, from
   atan(z) ~ 45z + 15.64z(1 - z) degrees, within a quarter degree. */
static INT64 atan_octant(INT64 z)
{
    return (720 * z + 250 * z * (4096 - z) / 4096) / 4096;
}

/* The angle of (dx, dy), with y growing downward, clockwise from up. */
static INT64 clock_angle(INT64 dx, INT64 dy)
{
    INT64 ex = dx, ny = -dy;
    INT64 ax = ex < 0 ? -ex : ex, ay = ny < 0 ? -ny : ny;
    INT64 base, theta;

    if (ax == 0 && ay == 0)
        return 0;
    base = ax >= ay ? atan_octant(ay * 4096 / ax)
                    : TURN / 4 - atan_octant(ax * 4096 / ay);
    if (ex >= 0)
        theta = ny >= 0 ? base : TURN - base;
    else
        theta = ny >= 0 ? TURN / 2 - base : TURN / 2 + base;
    return ((TURN / 4 - theta) % TURN + TURN) % TURN;
}

static void draw_spinner(const SCREEN *screen, INT64 cx, INT64 cy,
                         INT64 radius, INT64 width, INT64 head)
{
    SHAPE ring;
    INT64 x, y, reach = (radius + width) / SUBPIXEL + 2;

    ring.kind = RING;
    ring.ax = cx * SUBPIXEL + SUBPIXEL / 2;
    ring.ay = cy * SUBPIXEL + SUBPIXEL / 2;
    ring.bx = radius;
    ring.r = width;
    for (y = cy - reach; y <= cy + reach; y++)
        for (x = cx - reach; x <= cx + reach; x++) {
            INT64 behind = ((head - clock_angle(x - cx, y - cy)) % TURN +
                            TURN) % TURN;
            INT64 brightness = TRACK;

            if (behind < TAIL) {
                INT64 t = (TAIL - behind) * 255 / TAIL;

                brightness = TRACK + (255 - TRACK) * t * t / (255 * 255);
            }
            put(screen, x, y, brightness, coverage(&ring, 1, x, y));
        }
}

/* ------------------------------------------------------------------------ */

/* Switch to the largest mode within MAX_WIDTH x MAX_HEIGHT that has a
   linear 32-bit framebuffer, if it is larger than the current one. */
static void largest_mode(EFI_BOOT_SERVICES_PREFIX *boot,
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

EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
    EFI_BOOT_SERVICES_PREFIX *boot = SystemTable->BootServices;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info;
    SCREEN screen;
    INT64 h, top, x, y, cx, cy, head;
    EFI_STATUS status;

    (void)ImageHandle;
    /* The boot manager arms a five-minute watchdog before it starts an
       application (UEFI 2.10, 3.1.2); this one waits on a person. */
    boot->SetWatchdogTimer(0, 0, 0, 0);

    status = boot->LocateProtocol(&graphics_output_guid, 0, (VOID **)&gop);
    if (status != EFI_SUCCESS)
        return status;
    largest_mode(boot, gop);
    info = gop->Mode->Info;
    if (info->PixelFormat != PIXEL_RGB && info->PixelFormat != PIXEL_BGR)
        return EFI_SUCCESS;

    screen.pixels = (UINT32 *)gop->Mode->FrameBufferBase;
    screen.width = info->HorizontalResolution;
    screen.height = info->VerticalResolution;
    screen.stride = info->PixelsPerScanLine;
    screen.format = info->PixelFormat;

    for (y = 0; y < screen.height; y++)
        for (x = 0; x < screen.width; x++)
            screen.pixels[y * screen.stride + x] = 0;

    h = screen.width * SUBPIXEL / 22;
    top = screen.height * SUBPIXEL * 44 / 100 - h / 2;
    draw_wordmark(&screen, (screen.width * SUBPIXEL - wordmark_width(h)) / 2,
                  top, h);

    cx = screen.width / 2;
    cy = (top + h) / SUBPIXEL + h * 13 / 10 / SUBPIXEL;
    for (head = 0; boot->CheckEvent(SystemTable->ConIn->WaitForKey) !=
                   EFI_SUCCESS;
         head = (head + STEP) % TURN) {
        draw_spinner(&screen, cx, cy, h * 26 / 100, h * 3 / 100 + 1, head);
        boot->Stall(FRAME_MICROSECONDS);
    }
    return EFI_SUCCESS;
}
