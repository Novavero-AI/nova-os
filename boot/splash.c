/* A UEFI application that draws nova-os's boot screen through the Graphics
   Output Protocol: a dithered gradient with a soft glow, the wordmark
   "nova" in anti-aliased strokes, and a spinner that turns until a key is
   pressed, then returns EFI_SUCCESS.

   Types and layouts are written out from the UEFI Specification 2.10:
     2.3.1  common data types; 2.3.4 x64 calling convention
     4.3.1  EFI_SYSTEM_TABLE; 4.4.1 EFI_BOOT_SERVICES
     7.1.5  WaitForEvent; 7.1.7 CheckEvent; 7.3.16 LocateProtocol
     7.5.1  SetWatchdogTimer; 7.5.2 Stall
     12.3.1 EFI_SIMPLE_TEXT_INPUT_PROTOCOL
     12.9   EFI_GRAPHICS_OUTPUT_PROTOCOL
   A table member this program never calls is declared VOID * so the offsets
   of the members it does call stay right; the static assertions pin those
   offsets to the specification's.

   Everything is integer arithmetic: the image links no C runtime, and
   TinyCC compiles floating-point conversions as calls into one. Geometry
   is in eighths of a pixel. A pixel's coverage is the share of 16 sample
   points, on a 4x4 grid, that fall inside a stroke; inside tests compare
   squared distances, so no square root is taken. */

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

typedef EFI_STATUS (*EFI_CHECK_EVENT)(EFI_EVENT Event);
typedef EFI_STATUS (*EFI_STALL)(UINTN Microseconds);
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

_Static_assert(offsetof(EFI_BOOT_SERVICES_PREFIX, CheckEvent) == 0x78,
               "CheckEvent is the thirteenth service after the header");
_Static_assert(offsetof(EFI_BOOT_SERVICES_PREFIX, Stall) == 0xF8,
               "Stall is the twenty-ninth service after the header");
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

/* ------------------------------------------------------------------------
   The screen and the background */

typedef struct {
    UINT32 *pixels;
    INT64 width;
    INT64 height;
    INT64 stride;
    UINT32 format;
    INT64 glow_x;      /* the glow's centre, in pixels */
    INT64 glow_y;
    INT64 glow_radius; /* in pixels */
} SCREEN;

typedef struct {
    INT64 r, g, b; /* each 0..255 */
} RGB;

/* A 4x4 ordered-dither matrix, in sixteenths of a colour step. */
static const UINT8 bayer[4][4] = {
    {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

#define SUBPIXEL 8   /* geometry units per pixel */
#define SAMPLES 4    /* sample points per pixel along each axis */
#define COVERAGE_MAX (SAMPLES * SAMPLES)

static void put(const SCREEN *screen, INT64 x, INT64 y, RGB c)
{
    UINT32 packed;

    if (x < 0 || y < 0 || x >= screen->width || y >= screen->height)
        return;
    if (screen->format == PIXEL_BGR)
        packed = (UINT32)(c.r << 16 | c.g << 8 | c.b);
    else
        packed = (UINT32)(c.b << 16 | c.g << 8 | c.r);
    screen->pixels[y * screen->stride + x] = packed;
}

/* One channel of the background, in sixteenths of a step: a vertical
   gradient from top to bottom, plus a glow that falls off with the square
   of the distance from its centre. */
static INT64 channel(INT64 top, INT64 bottom, INT64 glow, INT64 y,
                     INT64 height, INT64 strength)
{
    INT64 base = (top * (height - y) + bottom * y) * 16 / height;

    return base + glow * strength / 16;
}

static RGB background(const SCREEN *screen, INT64 x, INT64 y)
{
    INT64 dx = x - screen->glow_x, dy = y - screen->glow_y;
    INT64 r2 = screen->glow_radius * screen->glow_radius;
    INT64 d2 = dx * dx + dy * dy;
    INT64 strength = 0; /* 0..256 */
    INT64 dither = bayer[y & 3][x & 3];
    RGB c;

    if (d2 < r2) {
        strength = (r2 - d2) * 256 / r2;
        strength = strength * strength / 256;
    }
    c.r = (channel(9, 20, 46, y, screen->height, strength) + dither) / 16;
    c.g = (channel(10, 14, 20, y, screen->height, strength) + dither) / 16;
    c.b = (channel(22, 44, 96, y, screen->height, strength) + dither) / 16;
    if (c.r > 255) c.r = 255;
    if (c.g > 255) c.g = 255;
    if (c.b > 255) c.b = 255;
    return c;
}

/* Over the background, a colour at alpha (0..255) times coverage. */
static RGB blend(RGB under, RGB over, INT64 alpha, INT64 coverage)
{
    INT64 a = alpha * coverage / COVERAGE_MAX;
    RGB c;

    c.r = (under.r * (255 - a) + over.r * a) / 255;
    c.g = (under.g * (255 - a) + over.g * a) / 255;
    c.b = (under.b * (255 - a) + over.b * a) / 255;
    return c;
}

/* ------------------------------------------------------------------------
   Shapes, in eighths of a pixel */

#define CAPSULE 0 /* a segment from (ax, ay) to (bx, by), radius r */
#define RING 1    /* a circle of centre (ax, ay) and radius bx, half-width r */
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

/* How many of a pixel's 16 sample points fall inside any of the shapes. */
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

/* Paint the shapes over the background within a box, in pixels. */
static void paint(const SCREEN *screen, const SHAPE *shapes, int count,
                  INT64 x0, INT64 y0, INT64 x1, INT64 y1, RGB color,
                  INT64 alpha)
{
    INT64 x, y;

    for (y = y0; y <= y1; y++)
        for (x = x0; x <= x1; x++) {
            INT64 c = coverage(shapes, count, x, y);
            RGB under = background(screen, x, y);

            put(screen, x, y, c ? blend(under, color, alpha, c) : under);
        }
}

/* ------------------------------------------------------------------------
   The wordmark: "nova" in geometric strokes. h is the x-height, all in
   eighths of a pixel; each letter adds its shapes at (x, y), its top-left
   corner, and returns its width. */

#define MAX_SHAPES 8

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

/* The wordmark's width for x-height h, in the same units: the four
   letters (0.82h, h, 0.90h, h) and three gaps of 0.32h. */
static INT64 wordmark_width(INT64 h)
{
    return h * 82 / 100 + h + h * 90 / 100 + h + 3 * (h * 32 / 100);
}

/* ------------------------------------------------------------------------
   The spinner: twelve dots on a circle, a bright head and a fading trail. */

#define DOTS 12
#define FRAME_MICROSECONDS 66000

/* sin and cos of i * 30 degrees, times 1024. */
static const INT64 dot_sin[DOTS] = {0,     512,  887,  1024, 887,  512,
                                    0,     -512, -887, -1024, -887, -512};
static const INT64 dot_cos[DOTS] = {1024,  887,  512,  0,    -512, -887,
                                    -1024, -887, -512, 0,    512,  887};

typedef struct {
    INT64 cx, cy;   /* the circle's centre, in pixels */
    INT64 radius;   /* in pixels */
    INT64 dot;      /* each dot's radius, in pixels */
} SPINNER;

static void draw_spinner(const SCREEN *screen, const SPINNER *sp,
                         INT64 frame, RGB color)
{
    int i;

    for (i = 0; i < DOTS; i++) {
        INT64 x = sp->cx + sp->radius * dot_sin[i] / 1024;
        INT64 y = sp->cy - sp->radius * dot_cos[i] / 1024;
        INT64 age = ((frame - i) % DOTS + DOTS) % DOTS; /* 0 is the head */
        INT64 alpha = 255 - age * 255 / DOTS;
        SHAPE disc;

        disc.kind = CAPSULE;
        disc.ax = disc.bx = x * SUBPIXEL + SUBPIXEL / 2;
        disc.ay = disc.by = y * SUBPIXEL + SUBPIXEL / 2;
        disc.r = sp->dot * SUBPIXEL;
        paint(screen, &disc, 1, x - sp->dot - 1, y - sp->dot - 1,
              x + sp->dot + 1, y + sp->dot + 1, color,
              alpha * alpha / 255);
    }
}

/* ------------------------------------------------------------------------ */

/* The wordmark's and the spinner's colour. */
static const RGB ink = {236, 234, 255};

EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
    EFI_BOOT_SERVICES_PREFIX *boot = SystemTable->BootServices;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info;
    SCREEN screen;
    SPINNER spinner;
    INT64 h, r, x, y, left, top, width, frame;
    EFI_STATUS status;

    (void)ImageHandle;
    /* The boot manager arms a five-minute watchdog before it starts an
       application (UEFI 2.10, 3.1.2); this one waits on a person. */
    boot->SetWatchdogTimer(0, 0, 0, 0);

    status = boot->LocateProtocol(&graphics_output_guid, 0, (VOID **)&gop);
    if (status != EFI_SUCCESS)
        return status;
    info = gop->Mode->Info;
    if (info->PixelFormat != PIXEL_RGB && info->PixelFormat != PIXEL_BGR)
        return EFI_SUCCESS;

    screen.pixels = (UINT32 *)gop->Mode->FrameBufferBase;
    screen.width = info->HorizontalResolution;
    screen.height = info->VerticalResolution;
    screen.stride = info->PixelsPerScanLine;
    screen.format = info->PixelFormat;

    /* The wordmark spans about 30% of the screen's width. */
    h = screen.width * SUBPIXEL / 16;
    r = h / 13;
    width = wordmark_width(h);
    left = (screen.width * SUBPIXEL - width) / 2;
    top = screen.height * SUBPIXEL * 42 / 100 - h / 2;

    screen.glow_x = screen.width / 2;
    screen.glow_y = (top + h / 2) / SUBPIXEL;
    screen.glow_radius = screen.width * 35 / 100;

    for (y = 0; y < screen.height; y++)
        for (x = 0; x < screen.width; x++)
            put(&screen, x, y, background(&screen, x, y));

    x = left;
    x += letter_n(x, top, h, r) + h * 32 / 100;
    x += letter_o(x, top, h, r) + h * 32 / 100;
    x += letter_v(x, top, h, r) + h * 32 / 100;
    letter_a(x, top, h, r);
    paint(&screen, wordmark, wordmark_count, left / SUBPIXEL - 1,
          top / SUBPIXEL - 1, (left + width) / SUBPIXEL + 1,
          (top + h) / SUBPIXEL + 1, ink, 255);

    spinner.cx = screen.width / 2;
    spinner.cy = (top + h) / SUBPIXEL + h * 2 / SUBPIXEL;
    spinner.radius = h * 30 / 100 / SUBPIXEL;
    spinner.dot = h * 5 / 100 / SUBPIXEL;
    if (spinner.dot < 2)
        spinner.dot = 2;

    for (frame = 0; boot->CheckEvent(SystemTable->ConIn->WaitForKey) !=
                    EFI_SUCCESS;
         frame++) {
        draw_spinner(&screen, &spinner, frame, ink);
        boot->Stall(FRAME_MICROSECONDS);
    }
    return EFI_SUCCESS;
}
