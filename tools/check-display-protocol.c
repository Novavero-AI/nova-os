/* Checks include/display_protocol.h, which tools/mkinterface.c makes of
   interfaces/display.desc (docs/spec/interface.md P17, P12, docs/spec/
   display.md D5 to D7, D10 and D11), as a program includes it: the header
   compiles under the host's compiler with every warning on, a message of
   each kind and the shapes of the language the console's description has
   no use for (a reply's status, a structure tail, text, a counted array,
   a selector in an embedded structure, an event beside replies) encodes
   to the bytes its structure holds and passes its check, a byte that
   breaks a rule fails it, each channel's dispatcher names what it gets,
   and the settings' defaults agree with their counts. Prints each failure
   and a count, and exits nonzero if any check fails.

   cc -std=c11 -Wall -Wextra -pedantic -I include \
     -o check-display-protocol tools/check-display-protocol.c &&
     ./check-display-protocol */

#include <stdio.h>
#include <string.h>

#include "display_protocol.h"

static unsigned failures;

static void check(int passed, const char *what)
{
    if (passed)
        return;
    printf("failed: %s\n", what);
    failures++;
}

/* The bytes of a message, aligned to 8 bytes as the generated functions
   require, and a second buffer to build refused messages from. */
static union {
    UINT64 alignment;
    UINT8 bytes[CHANNEL_MAX_BYTES];
} buffer;

static const RECEIVED_HANDLE none[1] = {{0, 0, 0}};

#define CHANNEL_END_RIGHTS (CHANNEL_RIGHT_READ | CHANNEL_RIGHT_WRITE | RIGHT_WAIT)

/* A received channel end with the rights a connection or window needs. */
static RECEIVED_HANDLE channel_end(void)
{
    RECEIVED_HANDLE h;

    h.handle = 3;
    h.type = TYPE_CHANNEL;
    h.rights = CHANNEL_END_RIGHTS;
    return h;
}

static void poke32(UINT32 at, UINT32 value)
{
    buffer.bytes[at] = (UINT8)value;
    buffer.bytes[at + 1] = (UINT8)(value >> 8);
    buffer.bytes[at + 2] = (UINT8)(value >> 16);
    buffer.bytes[at + 3] = (UINT8)(value >> 24);
}

/* A message of the control channel: a connection's record and label. */
static UINT32 connect_with(UINT32 transaction, UINT32 mark, UINT32 layers,
                           const UINT8 *label, UINT32 label_count,
                           UINT32 *size)
{
    COMPOSITOR_CONNECT_REQUEST request;
    UINT32 handle = 3;
    HANDLE_TRANSFER transfer;

    memset(&request, 0, sizeof request);
    request.setup.transaction = transaction;
    request.mark = mark;
    request.layers = layers;
    request.rights = DISPLAY_RIGHT_MINT;
    return display_control_connect_encode(&request, label, label_count,
                                          &handle, 1, &transfer,
                                          buffer.bytes, size);
}

static void check_control(void)
{
    COMPOSITOR_SETUP_OPTIONS_MESSAGE options;
    COMPOSITOR_CONNECTED_REPLY connected;
    COMPOSITOR_CLOSED_EVENT closed;
    RECEIVED_HANDLE end = channel_end();
    UINT32 size = 0, which = 0, status;

    /* The options: a setup of kind 3 and a mask. */
    memset(&options, 0, sizeof options);
    options.setup.transaction = 0;
    options.options = COMPOSITOR_OPTION_REDUCE_MOTION;
    status = display_control_options_encode(&options, buffer.bytes, &size);
    check(status == OK && size == sizeof options &&
              display_control_request_check(buffer.bytes, size, none, 0,
                                            &which) == OK &&
              which == COMPOSITOR_SETUP_OPTIONS,
          "the options encode, pass the dispatcher and are named");
    options.options = COMPOSITOR_OPTIONS;
    check(display_control_options_encode(&options, buffer.bytes, &size) ==
                  OK &&
              size == sizeof options,
          "every option bit together encodes");
    options.options = COMPOSITOR_OPTIONS + 1;
    check(display_control_options_encode(&options, buffer.bytes, &size) ==
                  ERR_INVALID_ARGS &&
              size == 0,
          "an option bit outside COMPOSITOR_OPTIONS is refused");

    /* A connection: its record, its label as a text tail and the
       compositor's end, with a transaction other than 0 since the channel
       holds replies and events. */
    status = connect_with(5, 1, DISPLAY_LAYER_SYSTEM,
                          (const UINT8 *)"console", 7, &size);
    check(status == OK && size == sizeof(COMPOSITOR_CONNECT_REQUEST) + 7,
          "a connection and its label encode as the record and the label");
    check(display_control_connect_label(buffer.bytes) ==
              buffer.bytes + sizeof(COMPOSITOR_CONNECT_REQUEST),
          "the label starts after the record");
    check(memcmp(display_control_connect_label(buffer.bytes), "console", 7) == 0,
          "the label is the text given");
    check(display_control_request_check(buffer.bytes, size, &end, 1, &which) ==
                  OK &&
              which == COMPOSITOR_CONNECT,
          "a connection passes the dispatcher and is named");
    check(connect_with(0, 0, DISPLAY_LAYER_APP, (const UINT8 *)"a", 1,
                       &size) == ERR_INVALID_ARGS,
          "a request with a reply on a channel with events needs a "
          "transaction other than 0");
    check(connect_with(1, 2, DISPLAY_LAYER_APP, (const UINT8 *)"a", 1,
                       &size) == ERR_INVALID_ARGS,
          "a mark outside 0 and 1 is refused");
    check(connect_with(1, 0, 0x20, (const UINT8 *)"a", 1, &size) ==
              ERR_INVALID_ARGS,
          "a layer outside DISPLAY_LAYERS is refused");
    check(connect_with(1, 0, DISPLAY_LAYER_APP, (const UINT8 *)"a",
                       DISPLAY_LABEL_MAX + 1, &size) == ERR_INVALID_ARGS,
          "a label over DISPLAY_LABEL_MAX is refused");
    connect_with(1, 0, DISPLAY_LAYER_APP, (const UINT8 *)"abc", 3, &size);
    buffer.bytes[sizeof(COMPOSITOR_CONNECT_REQUEST) + 1] = 0;
    check(display_control_connect_check(buffer.bytes, size, &end, 1) ==
              ERR_INVALID_ARGS,
          "a received label holding NUL is refused");
    connect_with(1, 0, DISPLAY_LAYER_APP, (const UINT8 *)"abc", 3, &size);
    poke32(offsetof(COMPOSITOR_CONNECT_REQUEST, label_length), 2);
    check(display_control_connect_check(buffer.bytes, size, &end, 1) ==
              ERR_INVALID_ARGS,
          "a received label shorter than its length says is refused");
    connect_with(1, 0, DISPLAY_LAYER_APP, (const UINT8 *)"abc", 3, &size);
    check(display_control_connect_check(buffer.bytes, size, none, 0) ==
              ERR_INVALID_ARGS,
          "a connection without its channel is refused");
    /* RFC 3629: a NUL, an overlong form, a surrogate and a value above
       U+10FFFF are refused; two, three and four byte forms pass. */
    check(connect_with(1, 0, DISPLAY_LAYER_APP, (const UINT8 *)"a\0b", 3,
                       &size) == ERR_INVALID_ARGS,
          "a label holding NUL is refused");
    check(connect_with(1, 0, DISPLAY_LAYER_APP, (const UINT8 *)"\xC0\x80", 2,
                       &size) == ERR_INVALID_ARGS,
          "an overlong form is refused");
    check(connect_with(1, 0, DISPLAY_LAYER_APP, (const UINT8 *)"\xED\xA0\x80",
                       3, &size) == ERR_INVALID_ARGS,
          "a surrogate is refused");
    check(connect_with(1, 0, DISPLAY_LAYER_APP,
                       (const UINT8 *)"\xF4\x90\x80\x80", 4, &size) ==
              ERR_INVALID_ARGS,
          "a value above U+10FFFF is refused");
    check(connect_with(1, 0, DISPLAY_LAYER_APP, (const UINT8 *)"\xC3\xA9", 2,
                       &size) == OK &&
              connect_with(1, 0, DISPLAY_LAYER_APP,
                           (const UINT8 *)"\xE2\x82\xAC", 3, &size) == OK &&
              connect_with(1, 0, DISPLAY_LAYER_APP,
                           (const UINT8 *)"\xF0\x9F\x98\x80", 4, &size) == OK,
          "two, three and four byte characters pass");
    check(connect_with(1, 0, DISPLAY_LAYER_APP, (const UINT8 *)"\xC3", 1,
                       &size) == ERR_INVALID_ARGS,
          "a character cut short is refused");

    /* The answer: a status, and with any but OK the body alone. */
    memset(&connected, 0, sizeof connected);
    connected.transaction = 5;
    connected.connection = 1;
    status = display_control_connected_encode(&connected, buffer.bytes, &size);
    check(status == OK && size == sizeof connected &&
              display_control_connected_check(buffer.bytes, size, none, 0) ==
                  OK,
          "an answer with status OK encodes and passes");
    connected.status = ERR_NO_MEMORY;
    status = display_control_connected_encode(&connected, buffer.bytes, &size);
    check(status == OK &&
              display_control_connected_check(buffer.bytes, size, none, 0) ==
                  OK,
          "an answer with another status is its body alone and passes");
    check(display_control_connected_check(buffer.bytes, size, &end, 1) ==
              ERR_INVALID_ARGS,
          "an answer with another status that carries a handle is refused");
    check(display_control_connected_check(buffer.bytes, size + 1, none, 0) ==
              ERR_INVALID_ARGS,
          "an answer with a byte more is refused");

    /* An event: a transaction of 0 alone. */
    memset(&closed, 0, sizeof closed);
    closed.connection = 2;
    status = display_control_closed_encode(&closed, buffer.bytes, &size);
    check(status == OK && size == sizeof closed &&
              display_control_event_check(buffer.bytes, size, none, 0,
                                          &which) == OK &&
              which == COMPOSITOR_CLOSED,
          "an event encodes, passes the dispatcher and is named");
    closed.setup.transaction = 1;
    check(display_control_closed_encode(&closed, buffer.bytes, &size) ==
              ERR_INVALID_ARGS,
          "an event with a transaction is refused");
    check(display_control_request_check(buffer.bytes, 7, none, 0, &which) ==
                  ERR_INVALID_ARGS &&
              which == DISPLAY_CONTROL_UNNAMED,
          "a message too short to hold its selector is unnamed");
    poke32(4, 0x1234);
    check(display_control_request_check(buffer.bytes, 12, none, 0, &which) ==
                  ERR_NOT_SUPPORTED &&
              which == 0x1234,
          "a value no request has is refused and named as it is");
}

static void check_connection_and_window(void)
{
    DISPLAY_WINDOW_REQUEST window;
    WINDOW_BUFFER_REQUEST buffer_request;
    WINDOW_PRESENT_REQUEST present;
    WINDOW_RECT rectangles[2];
    WINDOW_KEY_EVENT key;
    WINDOW_POINTER_EVENT pointer;
    WINDOW_CONFIGURE_EVENT configure;
    DISPLAY_HEADER header;
    RECEIVED_HANDLE end = channel_end();
    RECEIVED_HANDLE sections[CHANNEL_MAX_HANDLES];
    UINT32 values[CHANNEL_MAX_HANDLES];
    HANDLE_TRANSFER transfers[CHANNEL_MAX_HANDLES];
    UINT32 size = 0, which = 0, status, i;

    /* A window asked for: a header, a size, a layer and a surface in
       their sets, and a handle. */
    memset(&window, 0, sizeof window);
    window.header.transaction = 9;
    window.width = 640;
    window.height = 480;
    window.layer = DISPLAY_LAYER_APP;
    window.surface = DISPLAY_SURFACE_PANEL;
    values[0] = 3;
    status = display_connection_window_encode(&window, values, 1, transfers,
                                              buffer.bytes, &size);
    check(status == OK && size == sizeof window &&
              transfers[0].rights == CHANNEL_END_RIGHTS &&
              display_connection_request_check(buffer.bytes, size, &end, 1,
                                               &which) == OK &&
              which == DISPLAY_WINDOW,
          "a window request encodes, passes and is named, its end travelling "
          "with READ, WRITE and WAIT");
    window.width = 0;
    check(display_connection_window_encode(&window, values, 1, transfers,
                                           buffer.bytes, &size) ==
              ERR_INVALID_ARGS,
          "a window of no width is refused");
    window.width = WINDOW_WIDTH_MAX + 1;
    check(display_connection_window_encode(&window, values, 1, transfers,
                                           buffer.bytes, &size) ==
              ERR_INVALID_ARGS,
          "a window wider than WINDOW_WIDTH_MAX is refused");
    window.width = 640;
    window.layer = 0x3;
    check(display_connection_window_encode(&window, values, 1, transfers,
                                           buffer.bytes, &size) ==
              ERR_INVALID_ARGS,
          "two layers at once are refused");
    window.layer = DISPLAY_LAYER_APP;
    window.header.transaction = 0;
    check(display_connection_window_encode(&window, values, 1, transfers,
                                           buffer.bytes, &size) ==
              ERR_INVALID_ARGS,
          "a window request with transaction 0 is refused");

    /* The screen: an event with a header, whose selector lies in the
       embedded header. */
    memset(&configure, 0, sizeof configure);
    memset(&header, 0, sizeof header);
    {
        DISPLAY_SCREEN_EVENT screen;

        memset(&screen, 0, sizeof screen);
        screen.width = 1280;
        screen.height = 800;
        screen.scale = DISPLAY_SCALE_ONE;
        status = display_connection_screen_encode(&screen, buffer.bytes,
                                                  &size);
        check(status == OK &&
                  display_connection_event_check(buffer.bytes, size, none, 0,
                                                 &which) == OK &&
                  which == DISPLAY_SCREEN,
              "the screen encodes, passes and is named");
        screen.width = 0;
        check(display_connection_screen_encode(&screen, buffer.bytes,
                                               &size) == ERR_INVALID_ARGS,
              "a screen of no width is refused");
    }

    /* A buffer: its numbers and a run of 1 to 64 sections. */
    memset(&buffer_request, 0, sizeof buffer_request);
    buffer_request.buffer = 1;
    buffer_request.width = 100;
    buffer_request.height = 100;
    buffer_request.stride = 100;
    for (i = 0; i < CHANNEL_MAX_HANDLES; i++)
        values[i] = 20 + i;
    status = display_window_buffer_encode(&buffer_request, values, 2,
                                          transfers, buffer.bytes, &size);
    check(status == OK && transfers[1].rights == SECTION_RIGHT_READ &&
              transfers[1].handle == 21,
          "a buffer with two sections encodes, each travelling with READ");
    for (i = 0; i < 2; i++) {
        sections[i].handle = 20 + i;
        sections[i].type = TYPE_SECTION;
        sections[i].rights = SECTION_RIGHT_READ | RIGHT_TRANSFER;
    }
    check(display_window_request_check(buffer.bytes, size, sections, 2,
                                       &which) == OK &&
              which == WINDOW_BUFFER,
          "a buffer passes and is named, its sections with more rights "
          "than READ passing");
    check(display_window_request_check(buffer.bytes, size, sections, 0,
                                       &which) == ERR_INVALID_ARGS,
          "a buffer with no section is refused");
    check(display_window_buffer_encode(&buffer_request, values,
                                       CHANNEL_MAX_HANDLES, transfers,
                                       buffer.bytes, &size) == OK,
          "a buffer of 64 sections is encoded");
    check(display_window_buffer_encode(&buffer_request, values,
                                       CHANNEL_MAX_HANDLES + 1, transfers,
                                       buffer.bytes, &size) ==
              ERR_INVALID_ARGS,
          "a buffer of 65 sections is refused");
    buffer_request.buffer = WINDOW_BUFFER_LAST + 1;
    check(display_window_buffer_encode(&buffer_request, values, 1, transfers,
                                       buffer.bytes, &size) ==
              ERR_INVALID_ARGS,
          "a buffer numbered past WINDOW_BUFFER_LAST is refused");

    /* A present: a structure tail counted by the body. */
    memset(&present, 0, sizeof present);
    status = display_window_present_encode(&present, rectangles, 0,
                                           buffer.bytes, &size);
    check(status == OK && size == sizeof present,
          "a present with no rectangle is its body");
    rectangles[0].x = 1;
    rectangles[0].y = 2;
    rectangles[0].width = 3;
    rectangles[0].height = 4;
    rectangles[1] = rectangles[0];
    status = display_window_present_encode(&present, rectangles, 2,
                                           buffer.bytes, &size);
    check(status == OK && size == sizeof present + 2 * sizeof rectangles[0] &&
              display_window_request_check(buffer.bytes, size, none, 0,
                                           &which) == OK &&
              which == WINDOW_PRESENT,
          "a present with two rectangles encodes, passes and is named");
    check(((const WINDOW_RECT *)display_window_present_damage(buffer.bytes))[1]
                  .height == 4,
          "the damage starts after the body");
    check(display_window_present_encode(&present, rectangles,
                                        WINDOW_DAMAGE_MAX + 1, buffer.bytes,
                                        &size) == ERR_INVALID_ARGS,
          "a present of more than WINDOW_DAMAGE_MAX rectangles is refused");
    rectangles[1].width = 0;
    check(display_window_present_encode(&present, rectangles, 2, buffer.bytes,
                                        &size) == ERR_INVALID_ARGS,
          "a rectangle of no width is refused");
    rectangles[1].width = 3;
    display_window_present_encode(&present, rectangles, 2, buffer.bytes,
                                  &size);
    check(display_window_present_check(buffer.bytes, size - 1, none, 0) ==
              ERR_INVALID_ARGS,
          "a present with a byte fewer than its rectangles is refused");
    poke32(offsetof(WINDOW_PRESENT_REQUEST, rectangles), 3);
    check(display_window_present_check(buffer.bytes, size, none, 0) ==
              ERR_INVALID_ARGS,
          "a present that counts more rectangles than it holds is refused");

    /* A key: text as a tail, a reserved field fixed at 0 and a value in
       a set. */
    memset(&key, 0, sizeof key);
    key.value = INPUT_KEY_PRESSED;
    status = display_window_key_encode(&key, (const UINT8 *)"\xC3\xA9", 2,
                                       buffer.bytes, &size);
    check(status == OK &&
              display_window_event_check(buffer.bytes, size, none, 0,
                                         &which) == OK &&
              which == WINDOW_KEY,
          "a key encodes, passes and is named");
    key.value = 3;
    check(display_window_key_encode(&key, (const UINT8 *)"a", 1, buffer.bytes,
                                    &size) == ERR_INVALID_ARGS,
          "a key value outside the set is refused");
    key.value = INPUT_KEY_RELEASED;
    key.reserved = 1;
    check(display_window_key_encode(&key, (const UINT8 *)"a", 1, buffer.bytes,
                                    &size) == ERR_INVALID_ARGS,
          "a reserved field that is not 0 is refused");
    key.reserved = 0;
    check(display_window_key_encode(&key, (const UINT8 *)"", WINDOW_KEY_TEXT_MAX + 1,
                                    buffer.bytes, &size) == ERR_INVALID_ARGS,
          "key text over WINDOW_KEY_TEXT_MAX is refused");

    /* A pointer report: a counted array, whose elements past the count
       are 0. */
    memset(&pointer, 0, sizeof pointer);
    pointer.buttons = 2;
    pointer.button[0].code = 272;
    pointer.button[0].value = INPUT_KEY_PRESSED;
    status = display_window_pointer_encode(&pointer, buffer.bytes, &size);
    check(status == OK && size == sizeof pointer &&
              display_window_event_check(buffer.bytes, size, none, 0,
                                         &which) == OK &&
              which == WINDOW_POINTER,
          "a pointer report encodes, passes and is named");
    poke32(offsetof(WINDOW_POINTER_EVENT, buttons), INPUT_REPORT_BUTTONS + 1);
    check(display_window_pointer_check(buffer.bytes, size, none, 0) ==
              ERR_INVALID_ARGS,
          "a received pointer report counting more buttons than it holds is "
          "refused");
    poke32(offsetof(WINDOW_POINTER_EVENT, buttons), 1);
    poke32(offsetof(WINDOW_POINTER_EVENT, button) + sizeof(INPUT_BUTTON),
           0x111);
    check(display_window_pointer_check(buffer.bytes, size, none, 0) ==
              ERR_INVALID_ARGS,
          "a received button past the count that is not 0 is refused");
    pointer.buttons = INPUT_REPORT_BUTTONS + 1;
    check(display_window_pointer_encode(&pointer, buffer.bytes, &size) ==
              ERR_INVALID_ARGS,
          "a pointer report counting more buttons than it holds is refused");
    pointer.buttons = 1;
    pointer.button[1].code = 273;
    check(display_window_pointer_encode(&pointer, buffer.bytes, &size) ==
              ERR_INVALID_ARGS,
          "a button past the count that is not 0 is refused");

    /* Events with a header alone, and one with constraints. */
    memset(&header, 0, sizeof header);
    check(display_window_dropped_encode(&header, buffer.bytes, &size) == OK &&
              display_window_event_check(buffer.bytes, size, none, 0,
                                         &which) == OK &&
              which == WINDOW_DROPPED,
          "a message of a header alone is named");
    memset(&configure, 0, sizeof configure);
    configure.serial = 1;
    configure.width = 10;
    configure.height = 10;
    configure.scale = DISPLAY_SCALE_ONE;
    configure.state = WINDOW_STATE_FOCUSED;
    check(display_window_configure_encode(&configure, buffer.bytes, &size) ==
              OK,
          "a configure encodes");
    configure.state = 0x2;
    check(display_window_configure_encode(&configure, buffer.bytes, &size) ==
              ERR_INVALID_ARGS,
          "a state outside WINDOW_STATES is refused");
    configure.state = 0;
    configure.serial = 0;
    check(display_window_configure_encode(&configure, buffer.bytes, &size) ==
              ERR_INVALID_ARGS,
          "a serial of 0 is refused");
}

/* The settings' defaults agree with their counts (P12). */
static const INT32 body_points[] = THEME_POINTER_BODY_DEFAULT_POINTS;
static const UINT8 body_flags[] = THEME_POINTER_BODY_DEFAULT_FLAGS;
static const UINT32 body_ends[] = THEME_POINTER_BODY_DEFAULT_ENDS;
static const INT32 edge_points[] = THEME_POINTER_EDGE_DEFAULT_POINTS;
static const UINT8 edge_flags[] = THEME_POINTER_EDGE_DEFAULT_FLAGS;
static const UINT32 edge_ends[] = THEME_POINTER_EDGE_DEFAULT_ENDS;

_Static_assert(sizeof body_points / sizeof body_points[0] ==
                   2 * THEME_POINTER_BODY_DEFAULT_POINT_COUNT,
               "x and y for each point of the pointer's body");
_Static_assert(sizeof body_flags / sizeof body_flags[0] ==
                   THEME_POINTER_BODY_DEFAULT_POINT_COUNT,
               "a flag for each point of the pointer's body");
_Static_assert(sizeof body_ends / sizeof body_ends[0] ==
                   THEME_POINTER_BODY_DEFAULT_CONTOURS,
               "an end for each contour of the pointer's body");
_Static_assert(sizeof edge_points / sizeof edge_points[0] ==
                   2 * THEME_POINTER_EDGE_DEFAULT_POINT_COUNT,
               "x and y for each point of the pointer's edge");
_Static_assert(sizeof edge_flags / sizeof edge_flags[0] ==
                   THEME_POINTER_EDGE_DEFAULT_POINT_COUNT,
               "a flag for each point of the pointer's edge");
_Static_assert(sizeof edge_ends / sizeof edge_ends[0] ==
                   THEME_POINTER_EDGE_DEFAULT_CONTOURS,
               "an end for each contour of the pointer's edge");

static void check_settings(void)
{
    unsigned i;
    unsigned cubic = 0;
    int straight = 1;

    for (i = 0; i < THEME_POINTER_BODY_DEFAULT_POINT_COUNT; i++)
        if (body_flags[i] != 1)
            straight = 0;
    check(straight, "the pointer's body is a polygon of straight edges");
    check(body_ends[THEME_POINTER_BODY_DEFAULT_CONTOURS - 1] ==
              THEME_POINTER_BODY_DEFAULT_POINT_COUNT &&
              edge_ends[THEME_POINTER_EDGE_DEFAULT_CONTOURS - 1] ==
                  THEME_POINTER_EDGE_DEFAULT_POINT_COUNT,
          "the last contour ends one past the last point");
    check(body_points[0] == 0 && body_points[1] == 0 &&
              body_points[2] == 0 && body_points[3] == 4393,
          "the pointer's body starts at its hotspot and runs down");
    check(edge_points[0] == 170 && edge_points[1] == -192,
          "the pointer's edge starts at its first point");
    for (i = 0; i < THEME_POINTER_EDGE_DEFAULT_POINT_COUNT; i++)
        if (edge_flags[i] == 2)
            cubic++;
    check(cubic % 2 == 0 && edge_flags[0] == 1,
          "the edge's cubic controls come in twos and it starts on the curve");
    check(THEME_MATERIAL_PANEL_DEFAULT_TINT == 0x121620 &&
              THEME_MATERIAL_PANEL_DEFAULT_AMOUNT == 68 &&
              THEME_MATERIAL_WINDOW_DEFAULT_AMOUNT <= 100 &&
              THEME_MATERIAL_RAISED_DEFAULT_AMOUNT <= 100,
          "the materials' defaults are a colour and a percent");
    check(THEME_POINTER_BODY_INK_DEFAULT == 0xF2F2F6 &&
              THEME_POINTER_EDGE_INK_DEFAULT == 0 &&
              THEME_POINTER_EDGE_OPACITY_DEFAULT == 85,
          "the pointer's inks and the edge's opacity are look.md V15's");
}

int main(void)
{
    check_control();
    check_connection_and_window();
    check_settings();
    printf("%u failure%s\n", failures, failures == 1 ? "" : "s");
    return failures != 0;
}
