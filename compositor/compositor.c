/* The compositor, \nova\compose.exe (docs/spec/display.md D2, D3 and
   D19's phase 3 arrangement). The first process starts it, and sends on
   its control channel the options, then the kernel's background where
   there is one, then the display with a resource over the framebuffer
   (display_protocol.h); the platform driver keeps its bootstrap channel,
   the input drivers' client channel, and ends it by closing it.

   The wallpaper is the kernel's background where its label matches the
   display, mapped to read, and Limb computed into memory of its own
   where it does not and in a compositor started again, which gets none.
   The first frame is the wallpaper alone, written to the framebuffer in
   bands of TILE_ROWS (lib/compose.c): Limb as the kernel's fade left it,
   so the screen shows no change. Then it serves connections and their
   window (serve.c), until its channel from the first process closes.

   A compositor that the options tell to exit for a test, a disk that
   carries COMPFAIL, ends just after the first frame that shows a
   window's present (serve.c; display.md D19, rule 9). */

#include "compositor.h"
#include "display_protocol.h"
#include "limb.h"
#include "log.h"
#include "mapping.h"
#include "native.h"
#include "timing.h"

/* Its own memory: the framebuffer's mapping, the wallpaper and a band,
   each mapped at the next free address from here, far above its image
   and below its stack. */
#define MAPPINGS_BASE 0x0000000100000000ULL

#define PIXEL_BYTES 4
#define WAKE_SIGNALS (CHANNEL_READABLE | CHANNEL_PEER_CLOSED)

/* Where the wallpaper came from, as the display line says. */
#define WALLPAPER_SECTION "section"
#define WALLPAPER_COMPUTED "computed"

/* channel_write needs a valid array even for no handles. */
static const HANDLE_TRANSFER no_handles[1];

/* The bytes of a message on the control channel, aligned to 8 bytes as
   the generated checks require (docs/spec/interface.md P14). */
static union {
    UINT64 alignment;
    COMPOSITOR_SETUP setup;
    COMPOSITOR_SETUP_DISPLAY_MESSAGE display;
    COMPOSITOR_SETUP_BACKGROUND_MESSAGE background;
    COMPOSITOR_SETUP_OPTIONS_MESSAGE options;
    UINT8 bytes[DISPLAY_CONTROL_TO_SERVER_BYTES_MAX];
} message;

static MAPPING mapping = {HANDLE_INVALID, MAPPINGS_BASE};
static UINT32 control = HANDLE_INVALID;
static UINT32 options;
static UINT64 started;

/* The wallpaper: where its pixels lie and in what format, how it was
   made and how long that took, for a background kept until the display
   comes. The image is 0 until one is. */
static const UINT32 *image;
static UINT32 image_format;
static const char *wallpaper_source;
static UINT64 prepare_ns;

static int has_display;

/* The screen written, the wallpaper in the form a frame is composed from,
   and memory for a band, which the display makes. */
static SCREEN output;
static COMPOSE_WALLPAPER wallpaper;
static UINT32 *band;

void compositor_say_status(const char *event, UINT32 status)
{
    say("nova-compositor: ");
    say(event);
    say(" status=");
    say_number(status);
    say("\r\n");
}

/* The bootstrap message: the debug log, the compositor's process and its
   end of the control channel. A message that is not that ends it. */
static void take_bootstrap(UINT32 bootstrap)
{
    RECEIVED_HANDLE handles[COMPOSITOR_BOOTSTRAP_HANDLES];
    UINT32 byte_count, handle_count, observed, i;
    UINT32 status;

    for (;;) {
        status = channel_read(bootstrap, message.bytes, sizeof message.bytes,
                              handles, COMPOSITOR_BOOTSTRAP_HANDLES,
                              &byte_count, &handle_count);
        if (status != ERR_SHOULD_WAIT)
            break;
        object_wait_one(bootstrap, WAKE_SIGNALS, DEADLINE_INFINITE,
                        &observed);
    }
    if (status == OK &&
        display_bootstrap_start_check(message.bytes, byte_count, handles,
                                      handle_count) == OK) {
        say_open(handles[COMPOSITOR_BOOTSTRAP_DEBUG_LOG].handle);
        mapping.self = handles[COMPOSITOR_BOOTSTRAP_SELF].handle;
        control = handles[COMPOSITOR_BOOTSTRAP_CONTROL].handle;
        return;
    }
    for (i = 0; status == OK && i < handle_count; i++)
        handle_close(handles[i].handle);
    process_exit(COMPOSITOR_EXIT_FAILED);
}

void compositor_close_received(const RECEIVED_HANDLE *handles, UINT32 count)
{
    UINT32 i;

    for (i = 0; i < count; i++)
        handle_close(handles[i].handle);
}

/* ------------------------------------------------------------------------
   The wallpaper. */

/* Compute Limb into memory of its own, as a PIXEL_BGR image, for a
   screen of the size display gives, which mapping_display accepted and
   Limb therefore takes. Returns the status of the call that failed if
   there is no memory for it. */
static UINT32 compute_wallpaper(const SCREEN *screen)
{
    UINT64 bytes = (UINT64)screen->width * screen->height * PIXEL_BYTES;
    UINT32 status = OK;
    UINT32 *pixels = mapping_memory(&mapping, bytes, &status);
    LIMB limb;
    INT64 x, y;

    if (!pixels)
        return status;
    if (!limb_init(&limb, screen->width, screen->height)) {
        mapping_unmap(&mapping, pixels, bytes);
        return ERR_NOT_SUPPORTED;
    }
    for (y = 0; y < screen->height; y++)
        for (x = 0; x < screen->width; x++)
            pixels[y * screen->width + x] = limb_pixel(&limb, x, y);
    image = pixels;
    image_format = PIXEL_BGR;
    return OK;
}

/* Answer a background request, whose transaction was transaction,
   once its wallpaper is kept or it is refused. The first process may
   have closed the channel by now, which the next read finds. Overwrites
   message. */
static void answer_background(UINT32 transaction)
{
    UINT32 size;
    UINT32 status = display_control_prepared_encode(transaction,
                                                    message.bytes, &size);

    if (status == OK)
        status = channel_write(control, message.bytes, size, no_handles, 0);
    if (status != OK && status != ERR_PEER_CLOSED)
        compositor_say_status("background answer failed", status);
}

/* The kernel's background, offered before the display in the message
   (display.md D2 rule 2): keep it as the wallpaper where its label
   matches the display the compositor is to be given, and close its
   sections either way, since a mapping outlives the handle it was made
   through. A background that does not match, that arrives after the
   display or that follows another is refused, logged, and Limb is
   computed when the display comes if none is kept. Either way it is
   answered. */
static void take_background(const COMPOSITOR_SETUP_BACKGROUND_MESSAGE *m,
                            const RECEIVED_HANDLE *handles, UINT32 count)
{
    const char *refusal;
    UINT64 mapped, begun = timing_now();
    UINT32 status = OK;
    UINT32 transaction = m->setup.transaction;

    if (has_display)
        refusal = "after the display";
    else if (image)
        refusal = "second background";
    else
        refusal = compose_wallpaper_refusal(&m->background, &m->display, count);
    if (!refusal) {
        UINT64 bytes = (UINT64)m->background.width * m->background.height *
                       BOOTSTRAP_BACKGROUND_PIXEL_BYTES;

        image = mapping_image(&mapping, handles, count, bytes, &mapped,
                              &status);
        if (image) {
            image_format = m->background.format;
            wallpaper_source = WALLPAPER_SECTION;
            prepare_ns = timing_now() - begun;
        } else {
            compositor_say_status("background map failed", status);
        }
    } else {
        say("nova-compositor: background refused reason=");
        say_value(refusal);
        say("\r\n");
    }
    compositor_close_received(handles, count);
    answer_background(transaction);
}

/* ------------------------------------------------------------------------
   The display. */

/* Take the display through resource and show the first frame: the
   wallpaper alone, which is the kernel's Limb where the wallpaper is the
   kernel's. Ends the compositor, FAILED, if the framebuffer cannot be
   mapped or there is no memory for the wallpaper or a band. */
static void take_display(UINT32 resource, const BOOTSTRAP_DISPLAY *d)
{
    UINT32 status = mapping_display(&mapping, resource, d, &output);
    UINT64 first_frame_ns;

    handle_close(resource);
    has_display = 1;
    if (status == ERR_NOT_SUPPORTED) {
        say("nova-compositor: display refused reason=unsupported\r\n");
        process_exit(COMPOSITOR_EXIT_FAILED);
    }
    if (status != OK) {
        compositor_say_status("framebuffer map failed", status);
        process_exit(COMPOSITOR_EXIT_FAILED);
    }
    if (!image) {
        UINT64 begun = timing_now();

        status = compute_wallpaper(&output);
        if (status != OK) {
            compositor_say_status("wallpaper failed", status);
            process_exit(COMPOSITOR_EXIT_FAILED);
        }
        wallpaper_source = WALLPAPER_COMPUTED;
        prepare_ns = timing_now() - begun;
    }
    band = mapping_memory(&mapping,
                          (UINT64)output.width * TILE_ROWS * PIXEL_BYTES,
                          &status);
    if (!band) {
        compositor_say_status("band memory failed", status);
        process_exit(COMPOSITOR_EXIT_FAILED);
    }
    wallpaper.pixels = image;
    wallpaper.stride = output.width;
    wallpaper.format = image_format;
    compose_wallpaper_frame(&output, &wallpaper, band, TILE_ROWS);
    gfx_flush();
    first_frame_ns = timing_now() - started;
    say("nova-compositor: display screen=");
    say_number((UINT64)output.width);
    say("x");
    say_number((UINT64)output.height);
    say(" wallpaper=");
    say(wallpaper_source);
    say(" prepare_ms=");
    timing_say_ms(prepare_ns);
    say(" first_frame_ms=");
    timing_say_ms(first_frame_ns);
    say("\r\n");
}

/* ------------------------------------------------------------------------
   The control channel. */

void compositor_refuse(UINT32 status, const RECEIVED_HANDLE *handles,
                       UINT32 count)
{
    if (status == OK)
        say("nova-compositor: control message refused reason=unserved\r\n");
    else
        compositor_say_status("control message refused", status);
    compositor_close_received(handles, count);
}

/* Take the first process's messages as they come until the display's,
   the options and the background having come before it. A message the
   description does not allow is dropped with the handles it carried
   closed, and one for a connection, which the display must come before,
   is dropped the same. The first process's close of the channel ends the
   compositor. */
static void take_setup(void)
{
    RECEIVED_HANDLE handles[BOOTSTRAP_BACKGROUND_SECTIONS_MAX];

    while (!has_display) {
        UINT32 byte_count, handle_count = 0, which = 0, observed;
        UINT32 read_status = channel_read(control, message.bytes,
                                          sizeof message.bytes, handles,
                                          BOOTSTRAP_BACKGROUND_SECTIONS_MAX,
                                          &byte_count, &handle_count);
        UINT32 status;

        if (read_status == ERR_SHOULD_WAIT) {
            object_wait_one(control, WAKE_SIGNALS, DEADLINE_INFINITE,
                            &observed);
            continue;
        }
        if (read_status == ERR_PEER_CLOSED)
            process_exit(COMPOSITOR_EXIT_DONE);
        if (read_status != OK) {
            compositor_say_status("control read failed", read_status);
            process_exit(COMPOSITOR_EXIT_FAILED);
        }
        status = display_control_request_check(message.bytes, byte_count,
                                               handles, handle_count, &which);
        if (status == OK && which == COMPOSITOR_SETUP_OPTIONS)
            options = message.options.options;
        else if (status == OK && which == COMPOSITOR_SETUP_BACKGROUND)
            take_background(&message.background, handles, handle_count);
        else if (status == OK && which == COMPOSITOR_SETUP_DISPLAY)
            take_display(handles[0].handle, &message.display.display);
        else
            compositor_refuse(status, handles, handle_count);
    }
}

_Noreturn void compositor_main(UINT32 bootstrap_channel, UINT64 argument)
{
    SERVE context;

    (void)argument;
    started = timing_now();
    take_bootstrap(bootstrap_channel);
    take_setup();
    context.mapping = &mapping;
    context.control = control;
    context.bootstrap = bootstrap_channel;
    context.screen = &output;
    context.wallpaper = &wallpaper;
    context.band = band;
    context.options = options;
    serve(&context);
}
