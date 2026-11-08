/* The type specimen (specimen.h, specimen_draw.h), started from /store
   with a view of its closure, which holds the fonts it draws with
   (docs/spec/storage.md F5, launch.h). Its bootstrap channel carries its
   launch message and then the display, as the console's control channel
   carries it: a CONSOLE_SETUP_DISPLAY_MESSAGE with an MMIO resource over
   the framebuffer (docs/spec/console.md C1, C2). It maps its fonts from
   /store, draws Limb and the specimen into memory of its own and copies
   it to the screen, logs how long that took and exits with
   SPECIMEN_EXIT_PASSED, or SPECIMEN_EXIT_FAILED at the first step that
   fails.

   FONTS_PATH, the fonts' directory in /store, is defined when it is
   built, so the program records the path, which makes the fonts' store
   path a reference of its own (storage.md, fact 5). */

#include "console_protocol.h"
#include "launch.h"
#include "limb.h"
#include "log.h"
#include "mapping.h"
#include "native.h"
#include "path.h"
#include "specimen.h"
#include "specimen_draw.h"

/* Its own memory: the fonts, the framebuffer's mapping and the canvas,
   each mapped at the next free address from here, far above its image
   and below its stack. */
#define MAPPINGS_BASE 0x0000000100000000ULL

#define PIXEL_BYTES 4
#define PATH_BUFFER 512
#define NS_PER_MS 1000000ULL
#define TIMES_TEN_PER_MS 10
#define CHANNEL_BITS 8
#define CHANNEL_MASK 0xFFu

static const char fonts_path[] = FONTS_PATH;
static UINT8 launch[CHANNEL_MAX_BYTES];
static VIEW view;
static MAPPING mapping = {HANDLE_INVALID, MAPPINGS_BASE};
static OUTLINE outline;
static TYPEFACE faces[SPECIMEN_FACES];
static SCREEN screen;
static CANVAS canvas;

static void check(int passed, const char *what)
{
    if (passed)
        return;
    say("nova-specimen: check failed reason=");
    say_value(what);
    say("\r\n");
    process_exit(SPECIMEN_EXIT_FAILED);
}

static UINT64 now(void)
{
    UINT64 nanoseconds = 0;

    clock_monotonic(&nanoseconds);
    return nanoseconds;
}

/* The launch message: the debug log, the process itself and the view,
   with no arguments. */
static void read_launch(UINT32 bootstrap)
{
    RECEIVED_HANDLE handles[LAUNCH_FIRST_DIRECTORY + LAUNCH_VIEW_MAX];
    const LAUNCH_HEADER *header = (const LAUNCH_HEADER *)launch;
    const LAUNCH_VIEW_ENTRY *entries =
        (const LAUNCH_VIEW_ENTRY *)(launch + sizeof *header);
    char prefix[LAUNCH_PREFIX_MAX + 1];
    UINT32 bytes, count, i, j;

    if (channel_read(bootstrap, launch, sizeof launch, handles,
                     LAUNCH_FIRST_DIRECTORY + LAUNCH_VIEW_MAX, &bytes,
                     &count) != OK ||
        count < LAUNCH_FIRST_DIRECTORY ||
        handles[LAUNCH_DEBUG_LOG].type != TYPE_RESOURCE)
        process_exit(SPECIMEN_EXIT_FAILED);
    say_open(handles[LAUNCH_DEBUG_LOG].handle);
    mapping.self = handles[LAUNCH_SELF].handle;
    check(bytes >= sizeof *header && header->view_count <= LAUNCH_VIEW_MAX &&
              count == LAUNCH_FIRST_DIRECTORY + header->view_count &&
              header->argument_bytes == 0 &&
              bytes == sizeof *header + header->view_count * sizeof *entries,
          "the launch message holds a view and no arguments");
    for (i = 0; i < header->view_count; i++) {
        check(entries[i].length <= LAUNCH_PREFIX_MAX,
              "a view prefix fits its entry");
        for (j = 0; j < entries[i].length; j++)
            prefix[j] = (char)entries[i].prefix[j];
        prefix[j] = 0;
        check(view_add(&view, prefix,
                       handles[LAUNCH_FIRST_DIRECTORY + i].handle) == OK,
              "a view entry is a prefix");
    }
}

/* The display, after the launch message: map its framebuffer. */
static void take_display(UINT32 bootstrap)
{
    CONSOLE_SETUP_DISPLAY_MESSAGE message;
    RECEIVED_HANDLE resource;
    UINT32 bytes, count, status;

    check(channel_read(bootstrap, &message, sizeof message, &resource, 1,
                       &bytes, &count) == OK &&
              bytes == sizeof message && count == 1 &&
              message.setup.kind == CONSOLE_SETUP_DISPLAY &&
              resource.type == TYPE_RESOURCE,
          "the display follows the launch message");
    handle_close(bootstrap);
    status = mapping_display(&mapping, resource.handle, &message.display,
                             &screen);
    handle_close(resource.handle);
    check(status == OK, "the framebuffer maps");
}

static UINT64 length_of(const char *text)
{
    UINT64 n = 0;

    while (text[n])
        n++;
    return n;
}

/* Map the font face's file from FONTS_PATH, read-only, and load it. */
static void load_face(int face)
{
    const char *file = specimen_face_file(face);
    UINT64 at = length_of(fonts_path), size, i;
    char path[PATH_BUFFER];
    IO_ATTRIBUTES attributes;
    RECEIVED_HANDLE section;
    UINT32 channel, status;
    const UINT8 *bytes;

    check(at + 1 + length_of(file) < PATH_BUFFER, "a font's path fits");
    for (i = 0; i < at; i++)
        path[i] = fonts_path[i];
    path[at++] = '/';
    for (i = 0; file[i]; i++)
        path[at++] = file[i];
    path[at] = 0;
    check(view_open(&view, path, IO_RIGHT_READ, &channel, &attributes) ==
                  OK &&
              attributes.kind == IO_KIND_FILE,
          "a font opens from /store");
    status = io_get_section(channel, IO_RIGHT_READ, &section, &size);
    handle_close(channel);
    check(status == OK && size == attributes.size, "a font's section comes");
    bytes = mapping_section(&mapping, section.handle, size, MAP_READ, &status);
    handle_close(section.handle);
    check(bytes != 0, "a font maps");
    check(typeface_load(&faces[face], bytes, size), "a font loads whole");
}

/* Limb on the canvas, then the specimen, then the canvas on the screen
   in its pixel format. */
static void draw(void)
{
    UINT32 status;
    LIMB limb;
    INT64 x, y;

    canvas.width = screen.width;
    canvas.height = screen.height;
    canvas.stride = screen.width;
    canvas.pixels = mapping_memory(
        &mapping, (UINT64)(canvas.width * canvas.height) * PIXEL_BYTES,
        &status);
    check(canvas.pixels != 0, "memory for the canvas");
    check(limb_init(&limb, canvas.width, canvas.height),
          "Limb draws at the screen's size");
    for (y = 0; y < canvas.height; y++)
        for (x = 0; x < canvas.width; x++)
            canvas.pixels[y * canvas.stride + x] = limb_pixel(&limb, x, y);
    check(specimen_draw(&canvas, faces, &outline), "the specimen draws");
    for (y = 0; y < canvas.height; y++)
        for (x = 0; x < canvas.width; x++) {
            UINT32 p = canvas.pixels[y * canvas.stride + x];

            screen.pixels[y * screen.stride + x] =
                gfx_pixel(&screen, p >> 2 * CHANNEL_BITS & CHANNEL_MASK,
                          p >> CHANNEL_BITS & CHANNEL_MASK, p & CHANNEL_MASK);
        }
    gfx_flush();
}

_Noreturn void specimen_main(UINT32 bootstrap, UINT64 argument)
{
    UINT64 started, tenths;
    int face;

    (void)argument;
    read_launch(bootstrap);
    take_display(bootstrap);
    started = now();
    for (face = 0; face < SPECIMEN_FACES; face++)
        load_face(face);
    draw();
    tenths = (now() - started) * TIMES_TEN_PER_MS / NS_PER_MS;
    say("nova-specimen: drawn screen=");
    say_number((UINT64)screen.width);
    say("x");
    say_number((UINT64)screen.height);
    say(" draw_ms=");
    say_number(tenths / TIMES_TEN_PER_MS);
    say(".");
    say_number(tenths % TIMES_TEN_PER_MS);
    say("\r\n");
    process_exit(SPECIMEN_EXIT_PASSED);
}
