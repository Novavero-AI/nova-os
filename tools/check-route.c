/* Host check of lib/route.c, how a pointing device's reports reach the
   window: docs/spec/display.md D10 rule 4 (an absolute axis's place on the
   screen), D11 rules 3 and 6 (what the window is sent, where the pointer
   is held and that a window learns only places relative to itself) and
   D19's phase 3 arrangement, rules 4 and 6; and of what lib/backlog.c does
   with the events it gives (D11, rule 7).

   Four kinds of case. The axis is compared with the spec's formula in
   128-bit integers, for every value of small ranges and of the tablet's,
   and for random wide ones. A table of reports, each with the events the
   spec's rules give written out by hand, covers the edges: a pixel at each
   edge of the window, a drag out and back, a button pressed over nothing,
   two buttons, two sources that have the same button, a stray release, a
   source that goes away with a button down, the wheel, a window that
   appears, moves or closes, a press and a move in one report. Random
   sessions of reports from three sources (two tablets and a mouse, all
   with the same five buttons, some reports stray or repeated) among
   windows that appear, move, change size and close, and a screen that
   changes size, run beside a model of the rules written another way (the
   pointer's place in 256ths compared with the window's rectangle in
   256ths, each button remembered by its source and code and counted by
   code, the hold kept as a target) and beside checks on the stream of
   events alone: entries and leaves alternate, a window is sent a press only
   of a button it sees up and a release only of one it was sent a press of,
   it has the pointer whenever it is sent a button as held, and a window
   with the pointer and no button down is under it. The events of each
   session are also queued for a window whose channel refuses for long
   stretches, and what the spec guarantees of what the window then reads
   is checked: order kept, the limit kept, a drop told once, and no button
   or entry stuck once every button is up and the queue is drained. What
   it does not guarantee is that a window that was told of a drop knows
   its buttons and where the pointer is, which is why the window must
   treat them as unknown then. Random cases come from a fixed seed, so a
   failure is the same one every run.

   cc -std=c11 -I include -I lib -I gfx tools/check-route.c lib/route.c \
      lib/backlog.c */

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "backlog.h"
#include "route.h"

#define FAILURES_SHOWN 12

#define SEED 0x9e3779b97f4a7c15ull

#define SESSIONS 4000
#define SESSION_STEPS 240

/* The buttons a source has: evdev's BTN_LEFT and the four after it. Every
   source has all of them, so a code is more than one source's and a
   window's view of a code is every source's. */
#define BUTTON_FIRST 0x110
#define BUTTON_CODES 5

/* The sources: a tablet, a mouse and a second tablet. The caller's numbers
   for them are any values, so they are not 0, 1 and 2. */
#define SOURCES 3
#define SOURCE_TABLET 0
#define SOURCE_MOUSE 1
#define SOURCE_SPARE 2

static const UINT32 source_number[SOURCES] = {0, 7, 0xFFFFFFFFu};

#define GRAB_NONE 0

_Static_assert(ROUTE_HELD_MAX == INPUT_REPORT_BUTTONS,
               "a report's changes all fit the table of held buttons");

#define POSITION_ONE 256ll
#define WHEEL_DETENT 120ll
#define INT32_GREATEST 0x7FFFFFFFll
#define EXTENT_LIMIT 0x7FFFFFll

#define TIME_STEP 1000u

/* A channel room that never runs out. */
#define UNLIMITED 0xFFFFFFFFu

__extension__ typedef unsigned __int128 UINT128;

static unsigned failures;
static unsigned long long checks;

static void check(int passed, const char *what, long long a, long long b)
{
    checks++;
    if (!passed && failures++ < FAILURES_SHOWN)
        printf("failed: %s (%lld, %lld)\n", what, a, b);
}

static unsigned long long random_state = SEED;

static unsigned long long next_random(void)
{
    random_state ^= random_state << 13;
    random_state ^= random_state >> 7;
    random_state ^= random_state << 17;
    return random_state;
}

static unsigned long long below(unsigned long long limit)
{
    return next_random() % limit;
}

static long long between(long long low, long long high)
{
    return low + (long long)below((unsigned long long)(high - low) + 1);
}

/* ------------------------------------------------------------------------
   The axis */

/* D10, rule 4, in 128 bits: the value's distance above the least, times
   the extent, in 256ths, over the range's size; a value outside the range
   is the nearest end of it. */
static long long model_axis(long long value, long long min, long long max,
                            long long extent)
{
    long long clamped = value < min ? min : value > max ? max : value;
    long long wide = extent > EXTENT_LIMIT ? EXTENT_LIMIT : extent;
    UINT128 numerator =
        (UINT128)(clamped - min) * (UINT128)wide * (UINT128)POSITION_ONE;

    return (long long)(numerator / (UINT128)(max - min + 1));
}

/* The spec's pixel: (value - min) * extent / (max - min + 1). */
static long long spec_pixel(long long value, long long min, long long max,
                            long long extent)
{
    UINT128 numerator = (UINT128)(value - min) * (UINT128)extent;

    return (long long)(numerator / (UINT128)(max - min + 1));
}

static UINT32 axis(long long value, long long min, long long max,
                   long long extent)
{
    INPUT_RANGE range;

    range.min = (INT32)min;
    range.max = (INT32)max;
    return route_axis((INT32)value, &range, (UINT32)extent);
}

/* Every value of a range, and a few beyond each end, against the model
   and the spec's pixel; and the laws: the place never falls as the value
   rises, and is below the extent's pixels. */
static void check_axis_range(long long min, long long max, long long extent)
{
    long long value, before = -1;

    for (value = min - 3; value <= max + 3; value++) {
        long long got = axis(value, min, max, extent);
        long long want = model_axis(value, min, max, extent);

        check(got == want, "an axis is placed as the spec's formula gives",
              value, got);
        check(got >= before, "the place never falls as the value rises",
              value, got);
        check(got < extent * POSITION_ONE, "the place is within the screen",
              value, got);
        if (value >= min && value <= max)
            check(got / POSITION_ONE == spec_pixel(value, min, max, extent),
                  "its pixel is (value - min) * extent / (max - min + 1)",
                  value, got);
        before = got;
    }
}

static void check_axes(void)
{
    static const long long extents[] = {1, 2, 3, 7, 1280, 2560, 2880, 7680};
    long long min, max, extent;
    unsigned i;

    for (min = -3; min <= 3; min++)
        for (max = min + 1; max <= min + 9; max++)
            for (extent = 1; extent <= 20; extent++)
                check_axis_range(min, max, extent);
    for (i = 0; i < sizeof extents / sizeof extents[0]; i++) {
        check_axis_range(0, 0x7FFF, extents[i]);
        check_axis_range(-40, 1000, extents[i]);
    }
    for (i = 0; i < 4000; i++) {
        long long low = between(-2000000000, 2000000000);
        long long high = between(low + 1, 2147483647);
        long long wide = between(1, 9000);
        long long value = between(low - 5, high + 5);
        long long got;

        if (value < -2147483648ll)
            value = -2147483648ll;
        if (value > INT32_GREATEST)
            value = INT32_GREATEST;
        got = axis(value, low, high, wide);
        check(got == model_axis(value, low, high, wide),
              "a wide range is placed as the formula gives", value, got);
    }

    /* The least and the greatest value, the widest range, and the
       extents at and past the limit. */
    check(axis(-2147483648ll, -2147483648ll, INT32_GREATEST, 1280) == 0,
          "the least value of the widest range is 0", 0, 0);
    check(axis(INT32_GREATEST, -2147483648ll, INT32_GREATEST, 1280) ==
              model_axis(INT32_GREATEST, -2147483648ll, INT32_GREATEST, 1280),
          "the greatest value of the widest range", 0, 0);
    check(axis(INT32_GREATEST, -2147483648ll, INT32_GREATEST, EXTENT_LIMIT) ==
              model_axis(INT32_GREATEST, -2147483648ll, INT32_GREATEST,
                         EXTENT_LIMIT),
          "the widest range on the widest screen", 0, 0);
    check(axis(5, 0, 10, EXTENT_LIMIT) == model_axis(5, 0, 10, EXTENT_LIMIT),
          "an extent at the limit", 0, 0);
    check(route_axis(5, &(INPUT_RANGE){0, 10}, 0xFFFFFFFFu) ==
              model_axis(5, 0, 10, EXTENT_LIMIT),
          "an extent past the limit is the limit", 0, 0);
    check(axis(5, 0, 10, EXTENT_LIMIT + 1) ==
              model_axis(5, 0, 10, EXTENT_LIMIT),
          "an extent one past the limit is the limit", 0, 0);
    check(route_axis(5, &(INPUT_RANGE){10, 0}, 100) == 0,
          "a range whose maximum is below its minimum gives 0", 0, 0);
    check(route_axis(5, &(INPUT_RANGE){0, 10}, 0) == 0,
          "an extent of 0 gives 0", 0, 0);
    check(axis(7, 7, 8, 1) == 0 && axis(8, 7, 8, 1) == 128,
          "a range of two on one pixel", 0, 0);
}

/* ------------------------------------------------------------------------
   Devices, windows and reports */

static INPUT_DEVICE make_device(UINT32 absolute, long long x_min,
                                long long x_max, long long y_min,
                                long long y_max)
{
    INPUT_DEVICE device;

    memset(&device, 0, sizeof device);
    device.type = INPUT_TYPE_DEVICE;
    device.kind = INPUT_DEVICE_POINTER;
    device.absolute = absolute;
    device.x.min = (INT32)x_min;
    device.x.max = (INT32)x_max;
    device.y.min = (INT32)y_min;
    device.y.max = (INT32)y_max;
    return device;
}

/* A device whose value on each axis is the place in 256ths itself, on a
   screen of width x height pixels. */
static INPUT_DEVICE identity_device(long long width, long long height)
{
    return make_device(INPUT_AXIS_X | INPUT_AXIS_Y, 0,
                       width * POSITION_ONE - 1, 0,
                       height * POSITION_ONE - 1);
}

static WINDOW_RECORD make_window(UINT32 x, UINT32 y, UINT32 width,
                                 UINT32 height, UINT32 shown)
{
    WINDOW_RECORD window;

    memset(&window, 0, sizeof window);
    window.x = x;
    window.y = y;
    window.width = width;
    window.height = height;
    window.shown = shown;
    return window;
}

typedef struct {
    UINT32 code;
    UINT32 value;
} CHANGE;

/* A report that sets the axes given, to the values given, with the button
   changes. */
static INPUT_POINTER make_report(UINT32 absolute, long long x, long long y,
                                 long long wheel, const CHANGE *changes,
                                 UINT32 count, UINT64 time)
{
    INPUT_POINTER report;
    UINT32 i;

    memset(&report, 0, sizeof report);
    report.type = INPUT_TYPE_SYN;
    report.code = INPUT_SYN_REPORT;
    report.absolute = absolute;
    report.x = (INT32)x;
    report.y = (INT32)y;
    report.wheel = (INT32)wheel;
    report.time = time;
    report.buttons = count;
    for (i = 0; i < count && i < INPUT_REPORT_BUTTONS; i++) {
        report.button[i].code = (UINT16)changes[i].code;
        report.button[i].value = (UINT16)changes[i].value;
    }
    return report;
}

/* The library's calls, with the events it fills in set to a byte it never
   writes first, so that a byte it leaves is seen. */
#define UNWRITTEN 0xA5

static void send_report(ROUTE *route, UINT32 source, const INPUT_DEVICE *device,
                        UINT32 width, UINT32 height,
                        const WINDOW_RECORD *window,
                        const INPUT_POINTER *report, ROUTED *routed)
{
    memset(routed, UNWRITTEN, sizeof *routed);
    route_report(route, source, device, width, height, window, report, routed);
}

static void send_gone(ROUTE *route, UINT32 source, const WINDOW_RECORD *window,
                      UINT64 time, ROUTED *routed)
{
    memset(routed, UNWRITTEN, sizeof *routed);
    route_source_gone(route, source, window, time, routed);
}

static void send_settle(ROUTE *route, const WINDOW_RECORD *window, UINT64 time,
                        ROUTED *routed)
{
    memset(routed, UNWRITTEN, sizeof *routed);
    route_settle(route, window, time, routed);
}

/* Whether the bytes of an event past the body its kind has are 0. */
static int tail_is_clear(const WINDOW_INPUT *input)
{
    const unsigned char *bytes = (const unsigned char *)input;
    size_t used = input->kind == WINDOW_ENTER     ? sizeof(WINDOW_ENTER_EVENT)
                  : input->kind == WINDOW_POINTER ? sizeof(WINDOW_POINTER_EVENT)
                                                  : 0;
    size_t i;

    for (i = offsetof(WINDOW_INPUT, body) + used; i < sizeof *input; i++)
        if (bytes[i] != 0)
            return 0;
    return 1;
}

/* ------------------------------------------------------------------------
   The table of reports: each is checked against the events written out. */

#define TABLE_SCREEN_WIDTH 1000
#define TABLE_SCREEN_HEIGHT 600

/* The events as a string, for a table to compare with: "E x y" for an
   entry, "P x y" for a pointer event, then " w=n" for a wheel and " b=
   code:value" for each button, "L" for a leave, joined by ";". */
static void describe(const ROUTED *routed, char *text, size_t size)
{
    UINT32 i, b;
    size_t at = 0;

    text[0] = 0;
    for (i = 0; i < routed->count && at + 1 < size; i++) {
        const WINDOW_INPUT *input = &routed->event[i];

        if (i != 0)
            at += (size_t)snprintf(text + at, size - at, ";");
        if (input->kind == WINDOW_ENTER)
            at += (size_t)snprintf(text + at, size - at, "E %d %d",
                                   input->body.enter.x, input->body.enter.y);
        else if (input->kind == WINDOW_LEAVE)
            at += (size_t)snprintf(text + at, size - at, "L");
        else if (input->kind == WINDOW_POINTER) {
            const WINDOW_POINTER_EVENT *p = &input->body.pointer;

            at += (size_t)snprintf(text + at, size - at, "P %d %d", p->x, p->y);
            if (p->wheel != 0)
                at += (size_t)snprintf(text + at, size - at, " w=%d", p->wheel);
            for (b = 0; b < p->buttons && b < INPUT_REPORT_BUTTONS; b++)
                at += (size_t)snprintf(text + at, size - at, " b=%x:%u",
                                       p->button[b].code, p->button[b].value);
        } else
            at += (size_t)snprintf(text + at, size - at, "?%u", input->kind);
    }
}

static void expect(const ROUTED *routed, const char *want, const char *what)
{
    char got[512];
    UINT32 i;

    describe(routed, got, sizeof got);
    for (i = 0; i < routed->count && i < ROUTE_EVENTS_MAX; i++)
        check(tail_is_clear(&routed->event[i]),
              "the bytes an event does not use are 0", i,
              routed->event[i].kind);
    checks++;
    if (strcmp(got, want) != 0 && failures++ < FAILURES_SHOWN)
        printf("failed: %s: got \"%s\", want \"%s\"\n", what, got, want);
}

typedef struct {
    ROUTE route;
    INPUT_DEVICE device;
    WINDOW_RECORD window;
    int have_window;
    UINT32 source; /* the source at_report sends from */
    UINT64 time;
} TABLE;

static void table_start(TABLE *t, UINT32 shown)
{
    memset(&t->route, 0, sizeof t->route);
    t->device = identity_device(TABLE_SCREEN_WIDTH, TABLE_SCREEN_HEIGHT);
    t->window = make_window(100, 50, 200, 100, shown);
    t->have_window = 1;
    t->source = source_number[SOURCE_TABLET];
    t->time = 0;
}

/* Send a report from t->source that sets both axes to the pixel (px, py)
   plus the fractions fx and fy in 256ths, with changes and a wheel turn of
   detents, and check the events. */
static void at_report(TABLE *t, long long px, long long py, UINT32 fx,
                      UINT32 fy, long long detents, const CHANGE *changes,
                      UINT32 count, const char *want, const char *what)
{
    INPUT_POINTER report =
        make_report(INPUT_AXIS_X | INPUT_AXIS_Y, px * POSITION_ONE + fx,
                    py * POSITION_ONE + fy, detents, changes, count,
                    ++t->time);
    ROUTED routed;

    send_report(&t->route, t->source, &t->device, TABLE_SCREEN_WIDTH,
                TABLE_SCREEN_HEIGHT, t->have_window ? &t->window : 0, &report,
                &routed);
    expect(&routed, want, what);
}

#define LEFT BUTTON_FIRST
#define RIGHT (BUTTON_FIRST + 1)

static const CHANGE press_left = {LEFT, INPUT_KEY_PRESSED};
static const CHANGE release_left = {LEFT, INPUT_KEY_RELEASED};
static const CHANGE press_right = {RIGHT, INPUT_KEY_PRESSED};
static const CHANGE release_right = {RIGHT, INPUT_KEY_RELEASED};
static const CHANGE press_middle = {BUTTON_FIRST + 2, INPUT_KEY_PRESSED};
static const CHANGE release_middle = {BUTTON_FIRST + 2, INPUT_KEY_RELEASED};
/* The first code past those a report can change at once. */
static const CHANGE press_ninth = {BUTTON_FIRST + INPUT_REPORT_BUTTONS,
                                   INPUT_KEY_PRESSED};
static const CHANGE release_ninth = {BUTTON_FIRST + INPUT_REPORT_BUTTONS,
                                     INPUT_KEY_RELEASED};

/* The pointer enters, moves and leaves, at the pixels of each edge. */
static void check_table_motion(void)
{
    TABLE t;

    table_start(&t, WINDOW_NO_BUFFER);
    at_report(&t, 150, 80, 0, 0, 0, 0, 0, "",
              "a window not shown has no pointer");
    t.window.shown = 1;
    at_report(&t, 150, 80, 0, 0, 0, 0, 0, "E 12800 7680",
              "the first report over a window shown enters it");
    at_report(&t, 150, 80, 0, 0, 0, 0, 0, "", "a report that moves nothing");
    at_report(&t, 151, 80, 3, 0, 0, 0, 0, "P 13059 7680", "a move inside");
    at_report(&t, 100, 50, 0, 0, 0, 0, 0, "P 0 0", "the top left pixel");
    at_report(&t, 299, 149, 255, 255, 0, 0, 0, "P 51199 25599",
              "the last fraction of the bottom right pixel");
    at_report(&t, 300, 149, 0, 0, 0, 0, 0, "L",
              "one pixel past the right edge");
    at_report(&t, 299, 149, 0, 0, 0, 0, 0, "E 50944 25344", "and back in");
    at_report(&t, 299, 150, 0, 0, 0, 0, 0, "L", "one pixel below the bottom");
    at_report(&t, 150, 149, 0, 0, 0, 0, 0, "E 12800 25344", "back in");
    at_report(&t, 150, 49, 255, 255, 0, 0, 0, "L",
              "the last fraction of the pixel above the top");
    at_report(&t, 99, 80, 255, 0, 0, 0, 0, "",
              "the last fraction left of the left edge");
    at_report(&t, 100, 80, 0, 0, 0, 0, 0, "E 0 7680", "the left edge's pixel");
    at_report(&t, 0, 0, 0, 0, 0, 0, 0, "L", "the screen's corner");
    at_report(&t, 999, 599, 255, 255, 0, 0, 0, "", "the screen's far corner");
}

/* The pointer has no place until both axes are set, and a relative report
   moves it nowhere. */
static void check_table_placement(void)
{
    TABLE t;
    INPUT_POINTER report;
    ROUTED routed;

    table_start(&t, 1);
    report = make_report(INPUT_AXIS_X, 150 * POSITION_ONE, 0, 0, 0, 0, 1);
    send_report(&t.route, t.source, &t.device, TABLE_SCREEN_WIDTH,
                TABLE_SCREEN_HEIGHT, &t.window, &report, &routed);
    expect(&routed, "", "one axis gives no place");
    check(!route_placed(&t.route), "one axis does not place the pointer", 0, 0);
    report = make_report(INPUT_AXIS_X, 150 * POSITION_ONE, 0, 0,
                         &press_left, 1, 2);
    send_report(&t.route, t.source, &t.device, TABLE_SCREEN_WIDTH,
                TABLE_SCREEN_HEIGHT, &t.window, &report, &routed);
    expect(&routed, "", "a press before the pointer has a place goes nowhere");
    report = make_report(INPUT_AXIS_Y, 0, 80 * POSITION_ONE, 0, 0, 0, 3);
    send_report(&t.route, t.source, &t.device, TABLE_SCREEN_WIDTH,
                TABLE_SCREEN_HEIGHT, &t.window, &report, &routed);
    expect(&routed, "", "the second axis places it, held to none");
    check(route_placed(&t.route), "both axes place the pointer", 0, 0);
    check(t.route.x == 150 * POSITION_ONE && t.route.y == 80 * POSITION_ONE,
          "the place is the axes'", (long long)t.route.x, (long long)t.route.y);
    report = make_report(0, 0, 0, 0, &release_left, 1, 4);
    send_report(&t.route, t.source, &t.device, TABLE_SCREEN_WIDTH,
                TABLE_SCREEN_HEIGHT, &t.window, &report, &routed);
    expect(&routed, "E 12800 7680",
           "the release lets the window have the pointer");

    /* A report that sets an axis the device lacks does not set it. */
    memset(&t.route, 0, sizeof t.route);
    t.device = make_device(INPUT_AXIS_X, 0, 255, 0, 0);
    report = make_report(INPUT_AXIS_X | INPUT_AXIS_Y, 5, 5, 0, 0, 0, 5);
    send_report(&t.route, t.source, &t.device, TABLE_SCREEN_WIDTH,
                TABLE_SCREEN_HEIGHT, &t.window, &report, &routed);
    check(t.route.known == INPUT_AXIS_X, "an axis the device lacks is not set",
          t.route.known, 0);

    /* A report of relative motion alone. */
    table_start(&t, 1);
    at_report(&t, 150, 80, 0, 0, 0, 0, 0, "E 12800 7680", "entered");
    report = make_report(0, 0, 0, 0, 0, 0, 9);
    report.dx = 500;
    report.dy = -500;
    send_report(&t.route, t.source, &t.device, TABLE_SCREEN_WIDTH,
                TABLE_SCREEN_HEIGHT, &t.window, &report, &routed);
    expect(&routed, "", "relative motion moves no pointer");
}

/* A press inside holds the pointer to the window. */
static void check_table_drag(void)
{
    TABLE t;
    CHANGE both[2];

    table_start(&t, 1);
    at_report(&t, 150, 80, 0, 0, 0, 0, 0, "E 12800 7680", "in");
    at_report(&t, 150, 80, 0, 0, 0, &press_left, 1,
              "P 12800 7680 b=110:1", "a press inside");
    at_report(&t, 400, 300, 0, 0, 0, 0, 0, "P 76800 64000",
              "a drag out of the window is sent its place");
    at_report(&t, 0, 0, 0, 0, 0, 0, 0, "P -25600 -12800",
              "and a place left of and above it");
    at_report(&t, 999, 599, 0, 0, 0, 0, 0, "P 230144 140544",
              "and at the screen's far corner");
    at_report(&t, 999, 599, 0, 0, 0, &press_right, 1,
              "P 230144 140544 b=111:1", "a second button pressed outside");
    at_report(&t, 999, 599, 0, 0, 0, &release_left, 1,
              "P 230144 140544 b=110:0", "the first released, one still held");
    at_report(&t, 999, 598, 0, 0, 0, 0, 0, "P 230144 140288",
              "still held to the window");
    at_report(&t, 999, 598, 0, 0, 0, &release_right, 1,
              "P 230144 140288 b=111:0;L",
              "the last release goes to the window, then it is left");

    /* A drag out and back in, released inside: no leave, no entry. */
    table_start(&t, 1);
    at_report(&t, 150, 80, 0, 0, 0, &press_left, 1,
              "E 12800 7680;P 12800 7680 b=110:1",
              "a press in the first report enters, then is sent");
    at_report(&t, 500, 80, 0, 0, 0, 0, 0, "P 102400 7680", "out");
    at_report(&t, 160, 90, 0, 0, 0, 0, 0, "P 15360 10240", "back in");
    at_report(&t, 160, 90, 0, 0, 0, &release_left, 1, "P 15360 10240 b=110:0",
              "released inside");

    /* Two buttons pressed in one report, and released in one. */
    table_start(&t, 1);
    both[0] = press_left;
    both[1] = press_right;
    at_report(&t, 150, 80, 0, 0, 0, both, 2,
              "E 12800 7680;P 12800 7680 b=110:1 b=111:1", "two pressed");
    both[0] = release_right;
    both[1] = release_left;
    at_report(&t, 500, 80, 0, 0, 0, both, 2,
              "P 102400 7680 b=111:0 b=110:0;L", "two released, outside");
}

/* A report that claims more buttons than a report has room for is read as
   far as the room goes. */
static void check_table_button_count(void)
{
    struct {
        INPUT_POINTER report;
        INPUT_BUTTON more[INPUT_REPORT_BUTTONS];
    } wide;
    CHANGE presses[INPUT_REPORT_BUTTONS];
    TABLE t;
    ROUTED routed;
    UINT32 i;

    table_start(&t, 1);
    for (i = 0; i < INPUT_REPORT_BUTTONS; i++) {
        presses[i].code = BUTTON_FIRST + i;
        presses[i].value = INPUT_KEY_PRESSED;
    }
    memset(&wide, 0, sizeof wide);
    wide.report = make_report(INPUT_AXIS_X | INPUT_AXIS_Y, 150 * POSITION_ONE,
                              80 * POSITION_ONE, 0, presses,
                              INPUT_REPORT_BUTTONS, 1);
    /* The changes past the room are releases of the buttons the room
       presses, so that reading them would be seen. */
    for (i = 0; i < INPUT_REPORT_BUTTONS; i++) {
        wide.more[i].code = (UINT16)(BUTTON_FIRST + i);
        wide.more[i].value = INPUT_KEY_RELEASED;
    }
    wide.report.buttons = INPUT_REPORT_BUTTONS + 4;
    send_report(&t.route, t.source, &t.device, TABLE_SCREEN_WIDTH,
                TABLE_SCREEN_HEIGHT, &t.window, &wide.report, &routed);
    expect(&routed,
           "E 12800 7680;P 12800 7680 b=110:1 b=111:1 b=112:1 b=113:1 "
           "b=114:1 b=115:1 b=116:1 b=117:1",
           "a report that claims more buttons than it holds");
    check(t.route.held == INPUT_REPORT_BUTTONS,
          "only the buttons it can hold are taken", t.route.held, 0);

    /* With ROUTE_HELD_MAX held, another press is ignored, and so is its
       release, which keeps the window's view whole. */
    t.source = source_number[SOURCE_MOUSE];
    at_report(&t, 150, 80, 0, 0, 0, &press_ninth, 1, "",
              "a press with ROUTE_HELD_MAX held is ignored");
    check(t.route.held == ROUTE_HELD_MAX, "and is not held", t.route.held, 0);
    t.source = source_number[SOURCE_TABLET];
    at_report(&t, 150, 80, 0, 0, 0, &release_left, 1, "P 12800 7680 b=110:0",
              "a release makes room");
    t.source = source_number[SOURCE_MOUSE];
    at_report(&t, 150, 80, 0, 0, 0, &release_ninth, 1, "",
              "the release of the press that was ignored is ignored");
    at_report(&t, 150, 80, 0, 0, 0, &press_ninth, 1, "P 12800 7680 b=118:1",
              "and a press that fits is sent");
}

/* A button pressed over no window holds the pointer to none. */
static void check_table_outside(void)
{
    TABLE t;

    table_start(&t, 1);
    at_report(&t, 500, 400, 0, 0, 0, &press_left, 1, "",
              "a press over no window");
    at_report(&t, 150, 80, 0, 0, 0, 0, 0, "", "dragged over the window");
    at_report(&t, 150, 80, 0, 0, 3, 0, 0, "", "its wheel goes nowhere");
    at_report(&t, 160, 80, 0, 0, 0, &press_right, 1, "",
              "a button pressed over the window while that is held");
    at_report(&t, 160, 80, 0, 0, 0, &release_right, 1, "",
              "and released");
    at_report(&t, 160, 80, 0, 0, 0, &release_left, 1, "E 15360 7680",
              "the last release: the window has the pointer, told nothing "
              "of the buttons");
    at_report(&t, 170, 80, 0, 0, 0, 0, 0, "P 17920 7680", "and then motion");

    /* Released over nothing: nothing is told. */
    table_start(&t, 1);
    at_report(&t, 500, 400, 0, 0, 0, &press_left, 1, "", "pressed outside");
    at_report(&t, 600, 400, 0, 0, 0, &release_left, 1, "", "released outside");
}

/* A release of a button nothing holds changes nothing: the window is sent
   no release, and the next press is a press. */
static void check_table_stray_release(void)
{
    TABLE t;

    table_start(&t, 1);
    at_report(&t, 150, 80, 0, 0, 0, &release_left, 1, "E 12800 7680",
              "a release with nothing held, over the window, is not sent");
    at_report(&t, 150, 80, 0, 0, 0, &press_left, 1, "P 12800 7680 b=110:1",
              "the next press is sent");
    at_report(&t, 500, 80, 0, 0, 0, 0, 0, "P 102400 7680",
              "and holds the pointer to the window");
    at_report(&t, 500, 80, 0, 0, 0, &release_left, 1,
              "P 102400 7680 b=110:0;L", "until its release");

    table_start(&t, 1);
    at_report(&t, 500, 400, 0, 0, 0, &release_left, 1, "",
              "a release with nothing held, over no window, is not sent");
    at_report(&t, 150, 80, 0, 0, 0, &press_left, 1,
              "E 12800 7680;P 12800 7680 b=110:1",
              "a press over the window after it is sent");
}

/* A button is a source's and a code, and the window is sent a code: two
   sources that both have the left button give it one press and one
   release. */
static void check_table_sources(void)
{
    TABLE t;
    CHANGE both[2];
    UINT32 tablet = source_number[SOURCE_TABLET];
    UINT32 mouse = source_number[SOURCE_MOUSE];
    UINT32 spare = source_number[SOURCE_SPARE];

    table_start(&t, 1);
    t.source = tablet;
    at_report(&t, 150, 80, 0, 0, 0, &press_left, 1,
              "E 12800 7680;P 12800 7680 b=110:1", "the tablet's press");
    t.source = mouse;
    at_report(&t, 150, 80, 0, 0, 0, &press_left, 1, "",
              "the mouse's press of the same code is not a second press");
    at_report(&t, 150, 80, 0, 0, 0, &release_left, 1, "",
              "its release leaves the code held by the tablet");
    at_report(&t, 500, 80, 0, 0, 0, 0, 0, "P 102400 7680",
              "so the hold to the window goes on");
    t.source = tablet;
    at_report(&t, 500, 80, 0, 0, 0, &release_left, 1,
              "P 102400 7680 b=110:0;L",
              "the tablet's release is the last, and ends the hold");

    table_start(&t, 1);
    t.source = tablet;
    at_report(&t, 150, 80, 0, 0, 0, &press_left, 1,
              "E 12800 7680;P 12800 7680 b=110:1", "the tablet's press");
    t.source = mouse;
    at_report(&t, 150, 80, 0, 0, 0, &press_left, 1, "", "the mouse's press");
    t.source = tablet;
    at_report(&t, 150, 80, 0, 0, 0, &release_left, 1, "",
              "the tablet's release, the mouse still holding the code");
    t.source = mouse;
    at_report(&t, 150, 80, 0, 0, 0, &release_left, 1, "P 12800 7680 b=110:0",
              "the mouse's release is the last, after the tablet's");

    /* Three sources, one code, and the numbers that are no bound. */
    table_start(&t, 1);
    t.source = spare;
    at_report(&t, 150, 80, 0, 0, 0, &press_left, 1,
              "E 12800 7680;P 12800 7680 b=110:1",
              "a source numbered 0xFFFFFFFF");
    t.source = tablet;
    at_report(&t, 150, 80, 0, 0, 0, &press_left, 1, "", "a source numbered 0");
    t.source = mouse;
    at_report(&t, 150, 80, 0, 0, 0, &press_left, 1, "", "and a third");
    t.source = spare;
    at_report(&t, 150, 80, 0, 0, 0, &release_left, 1, "", "one lets go");
    t.source = mouse;
    at_report(&t, 150, 80, 0, 0, 0, &release_left, 1, "", "and another");
    t.source = tablet;
    at_report(&t, 150, 80, 0, 0, 0, &release_left, 1, "P 12800 7680 b=110:0",
              "the last is the window's release");

    /* Different codes of different sources are different buttons. */
    table_start(&t, 1);
    t.source = tablet;
    at_report(&t, 150, 80, 0, 0, 0, &press_left, 1,
              "E 12800 7680;P 12800 7680 b=110:1", "left, on the tablet");
    t.source = mouse;
    at_report(&t, 150, 80, 0, 0, 0, &press_right, 1, "P 12800 7680 b=111:1",
              "right, on the mouse");
    at_report(&t, 150, 80, 0, 0, 0, &release_right, 1, "P 12800 7680 b=111:0",
              "the mouse's release is the last of its code");
    t.source = tablet;
    at_report(&t, 150, 80, 0, 0, 0, &release_left, 1, "P 12800 7680 b=110:0",
              "and the tablet's of its own");

    /* A release of a button another source holds is a release of one not
       held. */
    table_start(&t, 1);
    t.source = tablet;
    at_report(&t, 150, 80, 0, 0, 0, &press_left, 1,
              "E 12800 7680;P 12800 7680 b=110:1", "held by the tablet");
    t.source = mouse;
    at_report(&t, 150, 80, 0, 0, 0, &release_left, 1, "",
              "the mouse's release of it changes nothing");
    at_report(&t, 500, 80, 0, 0, 0, 0, 0, "P 102400 7680", "the hold is kept");

    /* A stray release of one code while another is held. */
    table_start(&t, 1);
    at_report(&t, 150, 80, 0, 0, 0, &press_left, 1,
              "E 12800 7680;P 12800 7680 b=110:1", "left held");
    at_report(&t, 150, 80, 0, 0, 0, &release_right, 1, "",
              "a stray release of right gives nothing");
    at_report(&t, 500, 80, 0, 0, 0, 0, 0, "P 102400 7680",
              "and keeps the hold");
    at_report(&t, 500, 80, 0, 0, 0, &release_left, 1, "P 102400 7680 b=110:0;L",
              "which left's release ends");

    /* A press of a button held is ignored: one release lets it go. */
    table_start(&t, 1);
    at_report(&t, 150, 80, 0, 0, 0, &press_left, 1,
              "E 12800 7680;P 12800 7680 b=110:1", "pressed");
    at_report(&t, 150, 80, 0, 0, 0, &press_left, 1, "", "pressed again");
    at_report(&t, 150, 80, 0, 0, 0, &release_left, 1, "P 12800 7680 b=110:0",
              "one release lets go");
    at_report(&t, 500, 80, 0, 0, 0, 0, 0, "L", "nothing holds the pointer");

    /* A press and its release in one report. */
    table_start(&t, 1);
    both[0] = press_left;
    both[1] = release_left;
    at_report(&t, 150, 80, 0, 0, 0, both, 2,
              "E 12800 7680;P 12800 7680 b=110:1 b=110:0",
              "a press and its release in one report are both sent");
    at_report(&t, 500, 80, 0, 0, 0, 0, 0, "L", "and hold nothing");
}

static void gone(TABLE *t, UINT32 source, UINT64 time, const char *want,
                 const char *what)
{
    ROUTED routed;

    send_gone(&t->route, source, t->have_window ? &t->window : 0, time,
              &routed);
    expect(&routed, want, what);
}

/* An input endpoint that closes releases its source's buttons. */
static void check_table_gone(void)
{
    TABLE t;
    ROUTED routed;
    CHANGE both[2];
    UINT32 tablet = source_number[SOURCE_TABLET];
    UINT32 mouse = source_number[SOURCE_MOUSE];

    /* Two sources hold the pointer to the window, dragged outside. */
    table_start(&t, 1);
    t.source = tablet;
    at_report(&t, 150, 80, 0, 0, 0, &press_left, 1,
              "E 12800 7680;P 12800 7680 b=110:1", "tablet: left");
    t.source = mouse;
    at_report(&t, 150, 80, 0, 0, 0, &press_right, 1, "P 12800 7680 b=111:1",
              "mouse: right");
    at_report(&t, 500, 400, 0, 0, 0, 0, 0, "P 102400 89600", "dragged out");
    gone(&t, mouse, 900, "P 102400 89600 b=111:0",
         "the mouse goes: its release, at the pointer's place");
    check(t.route.entered == 1 && t.route.held == 1,
          "the tablet still holds the pointer", t.route.entered, t.route.held);
    gone(&t, tablet, 901, "P 102400 89600 b=110:0;L",
         "the tablet goes: its release, then the leave");
    check(t.route.held == 0 && t.route.grab == GRAB_NONE &&
              t.route.entered == 0,
          "nothing is held", t.route.held, t.route.grab);

    /* A code another source holds still is not released. */
    table_start(&t, 1);
    t.source = tablet;
    at_report(&t, 150, 80, 0, 0, 0, &press_left, 1,
              "E 12800 7680;P 12800 7680 b=110:1", "tablet: left");
    t.source = mouse;
    at_report(&t, 150, 80, 0, 0, 0, &press_left, 1, "", "mouse: left");
    gone(&t, mouse, 902, "", "the mouse goes: the code is still held");
    gone(&t, tablet, 903, "P 12800 7680 b=110:0",
         "the tablet goes: the last of the code, inside, so no leave");

    /* Every code of a source, in the order they were pressed, in one event;
       and the entries after one that goes keep their place. */
    table_start(&t, 1);
    t.source = tablet;
    both[0] = press_right;
    both[1] = press_left;
    at_report(&t, 150, 80, 0, 0, 0, both, 2,
              "E 12800 7680;P 12800 7680 b=111:1 b=110:1",
              "tablet: right, left");
    t.source = mouse;
    at_report(&t, 150, 80, 0, 0, 0, &press_middle, 1, "P 12800 7680 b=112:1",
              "mouse: middle");
    gone(&t, tablet, 904, "P 12800 7680 b=111:0 b=110:0",
         "both of the tablet's, in the order it pressed them");
    at_report(&t, 500, 400, 0, 0, 0, 0, 0, "P 102400 89600",
              "the mouse's middle still holds the pointer");
    at_report(&t, 500, 400, 0, 0, 0, &release_middle, 1,
              "P 102400 89600 b=112:0;L", "and is released as itself");

    table_start(&t, 1);
    t.source = tablet;
    at_report(&t, 150, 80, 0, 0, 0, &press_left, 1,
              "E 12800 7680;P 12800 7680 b=110:1", "tablet: left");
    t.source = mouse;
    at_report(&t, 150, 80, 0, 0, 0, &press_right, 1, "P 12800 7680 b=111:1",
              "mouse: right");
    t.source = tablet;
    at_report(&t, 150, 80, 0, 0, 0, &press_middle, 1, "P 12800 7680 b=112:1",
              "tablet: middle");
    gone(&t, mouse, 905, "P 12800 7680 b=111:0",
         "the entry between the tablet's two goes");
    both[0] = release_left;
    both[1] = release_middle;
    at_report(&t, 150, 80, 0, 0, 0, both, 2, "P 12800 7680 b=110:0 b=112:0",
              "the tablet's two are held as they were");

    /* A hold to no window: no release, and the end of the hold is the
       window's entry. */
    table_start(&t, 1);
    t.source = tablet;
    at_report(&t, 500, 400, 0, 0, 0, &press_left, 1, "", "pressed over none");
    at_report(&t, 150, 80, 0, 0, 0, 0, 0, "", "dragged over the window");
    gone(&t, tablet, 906, "E 12800 7680",
         "the source goes: the window is entered");

    /* The stamp of what is sent is the time given. */
    table_start(&t, 1);
    at_report(&t, 150, 80, 0, 0, 0, &press_left, 1,
              "E 12800 7680;P 12800 7680 b=110:1", "held");
    send_gone(&t.route, tablet, &t.window, 12345, &routed);
    check(routed.count == 1 && routed.event[0].kind == WINDOW_POINTER &&
              routed.event[0].body.pointer.time == 12345 &&
              routed.event[0].body.pointer.x == 12800 &&
              routed.event[0].body.pointer.y == 7680 &&
              routed.event[0].body.pointer.wheel == 0,
          "the release is stamped, placed and turns no wheel", routed.count, 0);
    table_start(&t, 1);
    at_report(&t, 500, 400, 0, 0, 0, &press_left, 1, "", "held to none");
    at_report(&t, 150, 80, 0, 0, 0, 0, 0, "", "over the window");
    send_gone(&t.route, tablet, &t.window, 54321, &routed);
    check(routed.count == 1 && routed.event[0].body.enter.time == 54321,
          "the entry is stamped with the time given", routed.count, 0);

    /* A source that holds nothing changes nothing. */
    table_start(&t, 1);
    t.source = tablet;
    at_report(&t, 150, 80, 0, 0, 0, &press_left, 1,
              "E 12800 7680;P 12800 7680 b=110:1", "tablet: left");
    gone(&t, mouse, 908, "", "a source that holds nothing is gone");
    at_report(&t, 150, 80, 0, 0, 0, &release_left, 1, "P 12800 7680 b=110:0",
              "and the tablet's hold is as it was");
    gone(&t, tablet, 909, "", "a source that holds nothing, again");

    /* No window: nothing is sent, and the source's buttons are not held. */
    table_start(&t, 1);
    t.source = tablet;
    at_report(&t, 150, 80, 0, 0, 0, &press_left, 1,
              "E 12800 7680;P 12800 7680 b=110:1", "held");
    t.have_window = 0;
    gone(&t, tablet, 910, "", "the window is not passed in");
    check(t.route.held == 0 && t.route.grab == GRAB_NONE &&
              t.route.entered == 0,
          "the buttons are let go and the window forgotten", t.route.held,
          t.route.entered);
}

/* A report that releases the hold and starts another. */
static void check_table_hold_changes(void)
{
    TABLE t;
    CHANGE swap[2];

    table_start(&t, 1);
    at_report(&t, 150, 80, 0, 0, 0, &press_left, 1,
              "E 12800 7680;P 12800 7680 b=110:1", "held to the window");
    swap[0] = release_left;
    swap[1] = press_right;
    at_report(&t, 500, 400, 0, 0, 0, swap, 2, "P 102400 89600 b=110:0;L",
              "released and another pressed outside, in one report");
    at_report(&t, 150, 80, 0, 0, 0, 0, 0, "",
              "the new hold is to none, over the window");
    at_report(&t, 150, 80, 0, 0, 0, &release_right, 1, "E 12800 7680",
              "released over it");

    table_start(&t, 1);
    at_report(&t, 500, 400, 0, 0, 0, &press_left, 1, "", "held to none");
    swap[0] = release_left;
    swap[1] = press_right;
    at_report(&t, 150, 80, 0, 0, 0, swap, 2,
              "E 12800 7680;P 12800 7680 b=111:1",
              "released and another pressed over the window, in one report");
    at_report(&t, 500, 400, 0, 0, 0, 0, 0, "P 102400 89600", "held to it");
}

/* The wheel is sent in 120ths of a detent, to the window that has the
   pointer. */
static void check_table_wheel(void)
{
    TABLE t;

    table_start(&t, 1);
    at_report(&t, 150, 80, 0, 0, 1, 0, 0, "E 12800 7680;P 12800 7680 w=120",
              "a detent up, entering");
    at_report(&t, 150, 80, 0, 0, -3, 0, 0, "P 12800 7680 w=-360",
              "three detents down");
    at_report(&t, 150, 80, 0, 0, 0x7FFFFFFF, 0, 0, "P 12800 7680 w=2147483647",
              "a turn too large for the unit saturates");
    at_report(&t, 150, 80, 0, 0, -0x7FFFFFFF - 1, 0, 0,
              "P 12800 7680 w=-2147483648", "and one too far down");
    at_report(&t, 150, 80, 0, 0, 17895697, 0, 0, "P 12800 7680 w=2147483640",
              "the greatest that does not saturate");
    at_report(&t, 150, 80, 0, 0, 17895698, 0, 0, "P 12800 7680 w=2147483647",
              "and the least that does");
    at_report(&t, 500, 400, 0, 0, 2, 0, 0, "L",
              "outside, the wheel goes nowhere");
    at_report(&t, 500, 400, 0, 0, 2, 0, 0, "", "and again");

    /* The wheel goes to the window that has the pointer when the report is
       done. A press outside, then a report that moves over the window,
       releases and turns the wheel. */
    table_start(&t, 1);
    at_report(&t, 500, 400, 0, 0, 0, &press_left, 1, "", "pressed outside");
    at_report(
        &t, 150, 80, 0, 0, 2, &release_left, 1,
        "E 12800 7680;P 12800 7680 w=240",
        "released over the window, with a turn: the entry, then the turn");

    /* The mirror: held to the window, a report that moves outside,
       releases and turns the wheel. */
    table_start(&t, 1);
    at_report(&t, 150, 80, 0, 0, 0, &press_left, 1,
              "E 12800 7680;P 12800 7680 b=110:1", "pressed inside");
    at_report(
        &t, 500, 400, 0, 0, 2, &release_left, 1, "P 102400 89600 b=110:0;L",
        "released outside, with a turn: the release and the leave, no turn");

    /* A press over the window with a turn: the window has the pointer. */
    table_start(&t, 1);
    at_report(&t, 150, 80, 0, 0, 1, &press_left, 1,
              "E 12800 7680;P 12800 7680 w=120 b=110:1",
              "a press and a turn, entering");
    at_report(&t, 500, 400, 0, 0, 2, 0, 0, "P 102400 89600 w=240",
              "held to the window, a turn outside is the window's");

    /* A press over none, with a turn: the pointer is held to none. */
    table_start(&t, 1);
    at_report(&t, 500, 400, 0, 0, 2, &press_left, 1, "",
              "a press and a turn over none");
    at_report(&t, 150, 80, 0, 0, 2, 0, 0, "",
              "and a turn over the window, held");
}

/* A window that appears under the pointer, one that closes, and one not
   there. */
static void check_table_window(void)
{
    TABLE t;
    ROUTED routed;

    table_start(&t, WINDOW_NO_BUFFER);
    at_report(&t, 150, 80, 0, 0, 0, 0, 0, "", "no frame shown yet");
    send_settle(&t.route, &t.window, 77, &routed);
    expect(&routed, "", "settling before a frame shows changes nothing");
    t.window.shown = 0;
    send_settle(&t.route, &t.window, 78, &routed);
    expect(&routed, "E 12800 7680", "the first frame shows under the pointer");
    send_settle(&t.route, &t.window, 79, &routed);
    expect(&routed, "", "settling again changes nothing");
    check(routed.count == 0, "no event", routed.count, 0);

    /* Closed: told nothing, and its entry is forgotten. */
    route_window_closed(&t.route);
    t.have_window = 0;
    at_report(&t, 150, 80, 0, 0, 0, 0, 0, "", "no window");
    at_report(&t, 150, 80, 0, 0, 2, &press_left, 1, "",
              "a press with no window is held to none");
    t.window = make_window(120, 60, 100, 100, 0);
    t.have_window = 1;
    send_settle(&t.route, &t.window, 80, &routed);
    expect(&routed, "", "a new window under a held pointer is told nothing");
    at_report(&t, 150, 80, 0, 0, 0, &release_left, 1, "E 7680 5120",
              "and has the pointer once it is released");

    /* A window that closes while the pointer is held to it: the release
       that follows is for no window. */
    table_start(&t, 1);
    at_report(&t, 150, 80, 0, 0, 0, &press_left, 1,
              "E 12800 7680;P 12800 7680 b=110:1", "held to the window");
    route_window_closed(&t.route);
    check(t.route.entered == 0 && t.route.grab == ROUTE_GRAB_OUTSIDE &&
              t.route.held == 1,
          "a closed window's hold is to none", t.route.grab, t.route.held);
    t.window = make_window(100, 50, 200, 100, 0);
    send_settle(&t.route, &t.window, 81, &routed);
    expect(&routed, "", "a new window is not entered while a button is held");
    at_report(&t, 150, 80, 0, 0, 0, &release_left, 1, "E 12800 7680",
              "the release is not sent to it");

    /* A window that closes with no button held. */
    table_start(&t, 1);
    at_report(&t, 150, 80, 0, 0, 0, 0, 0, "E 12800 7680", "entered");
    route_window_closed(&t.route);
    check(t.route.entered == 0 && t.route.grab == GRAB_NONE,
          "a closed window has no pointer", t.route.entered, t.route.grab);
    check(route_placed(&t.route) && t.route.x == 150 * POSITION_ONE,
          "the place stays", (long long)t.route.x, 0);

    /* A window not passed in is no window: nothing is sent, which is not
       a place on the screen, and a hold to the window becomes a hold to
       none. */
    table_start(&t, 1);
    at_report(&t, 150, 80, 0, 0, 0, &press_left, 1,
              "E 12800 7680;P 12800 7680 b=110:1", "held");
    t.have_window = 0;
    at_report(&t, 160, 80, 0, 0, 0, 0, 0, "", "no window: nothing is sent");
    check(t.route.entered == 0 && t.route.grab == ROUTE_GRAB_OUTSIDE &&
              t.route.held == 1 && t.route.x == 160 * POSITION_ONE,
          "the window is forgotten, the hold is to none, the place moves",
          t.route.entered, t.route.grab);
    at_report(&t, 170, 80, 0, 0, 2, &release_left, 1, "",
              "a release and a turn with no window are sent nowhere");
    check(t.route.held == 0 && t.route.grab == GRAB_NONE,
          "and let the button go", t.route.held, t.route.grab);
    t.have_window = 1;
    at_report(&t, 170, 80, 0, 0, 0, 0, 0, "E 17920 7680",
              "a window that comes back is entered again");

    table_start(&t, 1);
    t.have_window = 0;
    at_report(&t, 150, 80, 0, 0, 3, &press_left, 1, "",
              "a press and a turn with no window");
    check(t.route.grab == ROUTE_GRAB_OUTSIDE && t.route.held == 1,
          "a press with no window is held to none", t.route.grab, t.route.held);
    t.have_window = 1;
    at_report(&t, 150, 80, 0, 0, 0, 0, 0, "",
              "and a window that appears is held off");
}

/* A window that changes under a pointer that has not moved: settling gives
   its entry or its leave, and none where a button holds the pointer. */
static void check_table_settle(void)
{
    TABLE t;
    ROUTED routed;

    table_start(&t, 1);
    at_report(&t, 150, 80, 0, 0, 0, 0, 0, "E 12800 7680", "entered");
    t.window = make_window(500, 300, 100, 100, 1);
    send_settle(&t.route, &t.window, 90, &routed);
    expect(&routed, "L",
           "the window moves away from under the pointer: a leave");
    check(t.route.entered == 0, "and it has not the pointer", t.route.entered,
          0);
    send_settle(&t.route, &t.window, 91, &routed);
    expect(&routed, "", "settling again leaves nothing to say");
    t.window = make_window(120, 60, 100, 100, 1);
    send_settle(&t.route, &t.window, 92, &routed);
    expect(&routed, "E 7680 5120", "it moves back under the pointer: an entry");
    check(routed.event[0].body.enter.time == 92, "stamped with the time given",
          routed.event[0].body.enter.time, 0);
    t.window.width = 20;
    send_settle(&t.route, &t.window, 93, &routed);
    expect(&routed, "L", "it shrinks to leave the pointer outside: a leave");
    t.window.width = 100;
    send_settle(&t.route, &t.window, 94, &routed);
    expect(&routed, "E 7680 5120", "it grows back over the pointer: an entry");
    send_settle(&t.route, &t.window, 95, &routed);
    expect(&routed, "", "settled");

    /* A window that shows no frame is under nothing. */
    table_start(&t, 1);
    at_report(&t, 150, 80, 0, 0, 0, 0, 0, "E 12800 7680", "entered");
    t.window.shown = WINDOW_NO_BUFFER;
    send_settle(&t.route, &t.window, 96, &routed);
    expect(&routed, "L", "a window that has shown no frame is left");

    /* A held pointer is not settled. */
    table_start(&t, 1);
    at_report(&t, 150, 80, 0, 0, 0, &press_left, 1,
              "E 12800 7680;P 12800 7680 b=110:1", "held");
    t.window = make_window(500, 300, 100, 100, 1);
    send_settle(&t.route, &t.window, 97, &routed);
    expect(&routed, "", "a window that moves from under a held pointer");
    check(t.route.entered == 1, "has the pointer still", t.route.entered, 0);

    /* No window: nothing is sent, not a leave, and a hold is to none. */
    table_start(&t, 1);
    at_report(&t, 150, 80, 0, 0, 0, &press_left, 1,
              "E 12800 7680;P 12800 7680 b=110:1", "held");
    send_settle(&t.route, 0, 98, &routed);
    expect(&routed, "", "settling with no window sends nothing");
    check(t.route.entered == 0 && t.route.grab == ROUTE_GRAB_OUTSIDE,
          "the window is forgotten, the hold is to none", t.route.entered,
          t.route.grab);
    table_start(&t, 1);
    at_report(&t, 150, 80, 0, 0, 0, 0, 0, "E 12800 7680", "entered");
    send_settle(&t.route, 0, 99, &routed);
    expect(&routed, "", "settling with no window sends no leave");
    send_settle(&t.route, &t.window, 100, &routed);
    expect(&routed, "E 12800 7680",
           "a window that comes back is entered again");
}

/* A window's place far from the origin, whose relative place would not
   fit 32 bits, saturates. */
static void check_table_relative(void)
{
    TABLE t;

    table_start(&t, 1);
    at_report(&t, 150, 80, 0, 0, 0, &press_left, 1,
              "E 12800 7680;P 12800 7680 b=110:1", "held");
    t.window.x = 0xFFFFFFFFu;
    t.window.y = 0x7FFFFFFFu;
    at_report(&t, 160, 80, 0, 0, 0, 0, 0, "P -2147483648 -2147483648",
              "a place that cannot be written is the nearest that can");
    t.window.x = 0;
    t.window.y = 0;
    at_report(&t, 161, 80, 0, 0, 0, 0, 0, "P 41216 20480", "and the origin");
}

/* The screens and extents at the edges of what is served. */
static void check_table_screens(void)
{
    ROUTE route;
    INPUT_DEVICE device = identity_device(1, 1);
    WINDOW_RECORD window = make_window(0, 0, 1, 1, 0);
    INPUT_POINTER report;
    ROUTED routed;

    memset(&route, 0, sizeof route);
    report = make_report(INPUT_AXIS_X | INPUT_AXIS_Y, 255, 255, 0, 0, 0, 1);
    send_report(&route, source_number[SOURCE_TABLET], &device, 1, 1, &window,
                &report, &routed);
    expect(&routed, "E 255 255", "a screen of one pixel");
    report = make_report(INPUT_AXIS_X | INPUT_AXIS_Y, 0, 0, 0, 0, 0, 2);
    send_report(&route, source_number[SOURCE_TABLET], &device, 1, 1, &window,
                &report, &routed);
    expect(&routed, "P 0 0", "its other end");

    /* A window that fills a screen is entered at each pixel of it. */
    memset(&route, 0, sizeof route);
    device = identity_device(TABLE_SCREEN_WIDTH, TABLE_SCREEN_HEIGHT);
    window = make_window(0, 0, TABLE_SCREEN_WIDTH, TABLE_SCREEN_HEIGHT, 0);
    report = make_report(INPUT_AXIS_X | INPUT_AXIS_Y,
                         TABLE_SCREEN_WIDTH * POSITION_ONE - 1,
                         TABLE_SCREEN_HEIGHT * POSITION_ONE - 1, 0, 0, 0, 1);
    send_report(&route, source_number[SOURCE_TABLET], &device,
                TABLE_SCREEN_WIDTH, TABLE_SCREEN_HEIGHT, &window, &report,
                &routed);
    expect(&routed, "E 255999 153599", "a window the size of the screen");
}

static void check_tables(void)
{
    check_table_motion();
    check_table_placement();
    check_table_drag();
    check_table_button_count();
    check_table_outside();
    check_table_stray_release();
    check_table_sources();
    check_table_gone();
    check_table_hold_changes();
    check_table_wheel();
    check_table_window();
    check_table_settle();
    check_table_relative();
    check_table_screens();
}

/* ------------------------------------------------------------------------
   The model of the rules, written another way. */

typedef struct {
    int kind;
    long long x, y, wheel;
    unsigned long long time;
    unsigned buttons;
    unsigned code[INPUT_REPORT_BUTTONS], value[INPUT_REPORT_BUTTONS];
} EXPECTED_EVENT;

typedef struct {
    int count;
    EXPECTED_EVENT event[ROUTE_EVENTS_MAX];
} EXPECTED;

typedef struct {
    int has_x, has_y;
    long long x, y;                    /* the place, in 256ths */
    int down[SOURCES][BUTTON_CODES]; /* the buttons held, by source and code */
    long long order[SOURCES][BUTTON_CODES]; /* when each was pressed: which
                                               press it was */
    long long presses;                 /* how many presses were taken */
    int pairs;                         /* how many buttons are held */
    int sources[BUTTON_CODES];         /* how many sources hold each code */
    int target;                        /* while any is held: 1 the window,
                                          2 no window */
    int in;                            /* the window has been told it has the
                                          pointer */
} MODEL;

/* Whether the place is in the window's rectangle, in 256ths. */
static int model_over(const MODEL *m, const WINDOW_RECORD *w)
{
    if (!m->has_x || !m->has_y || w == 0 || w->shown == WINDOW_NO_BUFFER)
        return 0;
    return m->x >= (long long)w->x * POSITION_ONE &&
           m->x < ((long long)w->x + w->width) * POSITION_ONE &&
           m->y >= (long long)w->y * POSITION_ONE &&
           m->y < ((long long)w->y + w->height) * POSITION_ONE;
}

/* D11, rule 3: while a button is held the pointer is where the first press
   put it, the window's or none; with none held it is where it lies. */
static int model_has(const MODEL *m, int over)
{
    return m->pairs != 0 ? m->target == 1 : over;
}

static long long model_wheel(long long detents)
{
    long long scaled = detents * WHEEL_DETENT;

    return scaled > INT32_GREATEST        ? INT32_GREATEST
           : scaled < -INT32_GREATEST - 1 ? -INT32_GREATEST - 1
                                          : scaled;
}

static void expected_enter(EXPECTED *out, const MODEL *m,
                           const WINDOW_RECORD *w, unsigned long long time)
{
    EXPECTED_EVENT *e = &out->event[out->count++];

    memset(e, 0, sizeof *e);
    e->kind = WINDOW_ENTER;
    e->x = m->x - (long long)w->x * POSITION_ONE;
    e->y = m->y - (long long)w->y * POSITION_ONE;
    e->time = time;
}

static void expected_leave(EXPECTED *out)
{
    EXPECTED_EVENT *e = &out->event[out->count++];

    memset(e, 0, sizeof *e);
    e->kind = WINDOW_LEAVE;
}

static void expected_button(EXPECTED_EVENT *p, unsigned code, unsigned value)
{
    p->code[p->buttons] = code;
    p->value[p->buttons++] = value;
}

/* A button of source d changes. The window is sent the code's press when
   the first source takes it up and its release when the last lets go, if
   the hold is to the window. */
static void model_change(MODEL *m, int d, unsigned code, unsigned value,
                         int over, EXPECTED_EVENT *p)
{
    int c = (int)code - BUTTON_FIRST;

    if (value == INPUT_KEY_PRESSED) {
        if (m->down[d][c] || m->pairs == ROUTE_HELD_MAX)
            return;
        if (m->pairs == 0)
            m->target = over ? 1 : 2;
        m->down[d][c] = 1;
        m->order[d][c] = ++m->presses;
        m->pairs++;
        if (++m->sources[c] == 1 && m->target == 1)
            expected_button(p, code, INPUT_KEY_PRESSED);
    } else {
        if (!m->down[d][c])
            return;
        m->down[d][c] = 0;
        m->pairs--;
        if (--m->sources[c] == 0 && m->target == 1)
            expected_button(p, code, INPUT_KEY_RELEASED);
        if (m->pairs == 0)
            m->target = 0;
    }
}

static void model_closed(MODEL *m)
{
    m->in = 0;
    if (m->pairs != 0)
        m->target = 2;
}

/* What the window is sent once a report, a settling or a source's going has
   changed the buttons: p is the pointer event so far. The window has the
   pointer or has not as the report is done, and the wheel, the entry and the
   leave all follow from that. A window that is not there is sent nothing. */
static void model_conclude(MODEL *m, const WINDOW_RECORD *w, EXPECTED_EVENT *p,
                           int was_in, int moved, long long detents,
                           unsigned long long time, EXPECTED *out)
{
    int has;

    out->count = 0;
    if (w == 0)
        return;
    has = model_has(m, model_over(m, w));
    p->x = m->x - (long long)w->x * POSITION_ONE;
    p->y = m->y - (long long)w->y * POSITION_ONE;
    p->time = time;
    p->wheel = has ? model_wheel(detents) : 0;
    if (has && !was_in)
        expected_enter(out, m, w, time);
    if (p->buttons != 0 || (has && (p->wheel != 0 || (moved && was_in))))
        out->event[out->count++] = *p;
    if (!has && was_in)
        expected_leave(out);
    m->in = has;
}

static void model_begin(EXPECTED_EVENT *p)
{
    memset(p, 0, sizeof *p);
    p->kind = WINDOW_POINTER;
}

/* The events the spec's rules give for a report from source d. */
static void model_report(MODEL *m, int d, const INPUT_DEVICE *device,
                         long long width, long long height,
                         const WINDOW_RECORD *w, const INPUT_POINTER *r,
                         EXPECTED *out)
{
    long long was_x = m->x, was_y = m->y;
    int was_in, over;
    unsigned i;
    EXPECTED_EVENT p;

    if (w == 0)
        model_closed(m);
    was_in = m->in;
    if (r->absolute & device->absolute & INPUT_AXIS_X) {
        m->x = model_axis(r->x, device->x.min, device->x.max, width);
        m->has_x = 1;
    }
    if (r->absolute & device->absolute & INPUT_AXIS_Y) {
        m->y = model_axis(r->y, device->y.min, device->y.max, height);
        m->has_y = 1;
    }
    over = model_over(m, w);
    model_begin(&p);
    for (i = 0; i < r->buttons && i < INPUT_REPORT_BUTTONS; i++)
        model_change(m, d, r->button[i].code, r->button[i].value, over, &p);
    model_conclude(m, w, &p, was_in, m->x != was_x || m->y != was_y, r->wheel,
                   r->time, out);
}

static void model_settle(MODEL *m, const WINDOW_RECORD *w,
                         unsigned long long time, EXPECTED *out)
{
    EXPECTED_EVENT p;

    if (w == 0)
        model_closed(m);
    model_begin(&p);
    model_conclude(m, w, &p, m->in, 0, 0, time, out);
}

/* The source's buttons are let go, the earliest pressed first. */
static void model_source_gone(MODEL *m, int d, const WINDOW_RECORD *w,
                              unsigned long long time, EXPECTED *out)
{
    EXPECTED_EVENT p;
    int was_in;

    if (w == 0)
        model_closed(m);
    was_in = m->in;
    model_begin(&p);
    for (;;) {
        int c, first = -1;

        for (c = 0; c < BUTTON_CODES; c++)
            if (m->down[d][c] &&
                (first < 0 || m->order[d][c] < m->order[d][first]))
                first = c;
        if (first < 0)
            break;
        model_change(m, d, (unsigned)(BUTTON_FIRST + first), INPUT_KEY_RELEASED,
                     0, &p);
    }
    model_conclude(m, w, &p, was_in, 0, 0, time, out);
}

/* Whether the routed events are the expected ones, field by field, with
   the fields no kind uses left 0. */
static int same_events(const ROUTED *routed, const EXPECTED *want)
{
    UINT32 i, b;

    if (routed->count != (UINT32)want->count)
        return 0;
    for (i = 0; i < routed->count; i++) {
        const WINDOW_INPUT *got = &routed->event[i];
        const EXPECTED_EVENT *e = &want->event[i];

        if (got->kind != (UINT32)e->kind || !tail_is_clear(got))
            return 0;
        if (e->kind == WINDOW_ENTER) {
            const WINDOW_ENTER_EVENT *n = &got->body.enter;

            if (n->header.transaction != 0 || n->header.kind != 0 ||
                n->x != e->x || n->y != e->y || n->time != e->time)
                return 0;
        } else if (e->kind == WINDOW_POINTER) {
            const WINDOW_POINTER_EVENT *p = &got->body.pointer;

            if (p->header.transaction != 0 || p->header.kind != 0 ||
                p->x != e->x || p->y != e->y || p->wheel != e->wheel ||
                p->buttons != e->buttons || p->time != e->time)
                return 0;
            for (b = 0; b < INPUT_REPORT_BUTTONS; b++) {
                UINT32 code = b < e->buttons ? e->code[b] : 0;
                UINT32 value = b < e->buttons ? e->value[b] : 0;

                if (p->button[b].code != code || p->button[b].value != value)
                    return 0;
            }
        }
    }
    return 1;
}

/* ------------------------------------------------------------------------
   What the window sees, judged from the events alone. */

typedef struct {
    int in;                        /* an entry not yet left */
    int pressed[BUTTON_CODES];     /* each button the window was sent a press
                                      of and no release */
} WATCH;

static int watch_pressed(const WATCH *w)
{
    int c, any = 0;

    for (c = 0; c < BUTTON_CODES; c++)
        any |= w->pressed[c];
    return any;
}

static void watch_events(WATCH *w, const ROUTED *routed)
{
    UINT32 i, b;

    for (i = 0; i < routed->count; i++) {
        const WINDOW_INPUT *input = &routed->event[i];

        if (input->kind == WINDOW_ENTER) {
            check(!w->in, "an entry follows a leave, or is the first", i, 0);
            w->in = 1;
        } else if (input->kind == WINDOW_LEAVE) {
            check(w->in, "a leave follows an entry", i, 0);
            w->in = 0;
        } else {
            const WINDOW_POINTER_EVENT *p = &input->body.pointer;

            check(input->kind == WINDOW_POINTER, "the kinds are the three",
                  input->kind, 0);
            check(w->in,
                  "a pointer event is sent while the window has the pointer", i,
                  0);
            for (b = 0; b < p->buttons && b < INPUT_REPORT_BUTTONS; b++) {
                int c = (int)p->button[b].code - BUTTON_FIRST;

                if (c < 0 || c >= BUTTON_CODES) {
                    check(0, "a button is one the devices have", c, 0);
                    continue;
                }
                if (p->button[b].value == INPUT_KEY_PRESSED) {
                    check(!w->pressed[c],
                          "a press is of a button the window sees released",
                          c, 0);
                    w->pressed[c] = 1;
                } else {
                    check(w->pressed[c],
                          "a release is of a button the window was sent a "
                          "press of",
                          c, 0);
                    w->pressed[c] = 0;
                }
            }
        }
    }
}

/* ------------------------------------------------------------------------
   The window's channel, as the events of a session reach it */

/* The most events a session queues. */
#define PIPE_IDS 4096

/* A tag in the bytes of an event's body that the backlog carries and no
   input event uses: the header's transaction, which a pointer or entry
   event keeps 0 and a leave has no body for. It names the event, in the
   order queued. */
#define TAG_OFFSET \
    (offsetof(WINDOW_INPUT, body) + offsetof(DISPLAY_HEADER, transaction))

static void tag_set(WINDOW_INPUT *input, UINT32 id)
{
    memcpy((unsigned char *)input + TAG_OFFSET, &id, sizeof id);
}

static UINT32 tag_get(const WINDOW_INPUT *input)
{
    UINT32 id;

    memcpy(&id, (const unsigned char *)input + TAG_OFFSET, sizeof id);
    return id;
}

static int input_is_motion(const WINDOW_INPUT *input)
{
    return input->kind == WINDOW_POINTER && input->body.pointer.buttons == 0 &&
           input->body.pointer.wheel == 0;
}

/* The events a window is sent are queued in a BACKLOG and read by the
   window as its channel takes them, as the compositor does. What the spec
   guarantees is checked as they are read: they come in the order they were
   queued, no more than WINDOW_BACKLOG wait, an event that is lost is lost to
   a drop that is told before the next event, and a drop owed is told once.
   What it does not guarantee is the window's knowledge: after
   WINDOW_DROPPED the window must treat its buttons and where the pointer is
   as unknown, so the view kept here takes them as released and outside. */
typedef struct {
    BACKLOG backlog;
    UINT32 sequence;                /* the tag of the last event queued */
    /* whether the event tagged n was a motion */
    unsigned char motion[PIPE_IDS];
    UINT32 last;                    /* the tag of the last event read */
    int dropped_since;              /* a drop was told since the last event */
    unsigned owed;                  /* the times an event was dropped with no
                                       drop owed */
    unsigned told;                  /* the drops the window was told of */
    int in;                         /* the window's view: it has the pointer */
    int pressed[BUTTON_CODES];      /* and which buttons are down */
} PIPE;

typedef struct {
    PIPE *pipe;
    UINT32 room;
} PIPE_CHANNEL;

static void pipe_read(PIPE *p, const BACKLOG_MESSAGE *message)
{
    const WINDOW_INPUT *input = &message->body.input;
    UINT32 id, k, b;

    switch (message->kind) {
    case WINDOW_DROPPED:
        p->told++;
        check(p->told <= p->owed, "a drop owed is told once", p->told, p->owed);
        p->dropped_since = 1;
        p->in = 0;
        memset(p->pressed, 0, sizeof p->pressed);
        return;
    case WINDOW_ENTER:
        p->in = 1;
        break;
    case WINDOW_LEAVE:
        p->in = 0;
        break;
    case WINDOW_POINTER:
        for (b = 0; b < input->body.pointer.buttons && b < INPUT_REPORT_BUTTONS;
             b++) {
            int c = (int)input->body.pointer.button[b].code - BUTTON_FIRST;

            if (c >= 0 && c < BUTTON_CODES)
                p->pressed[c] =
                    input->body.pointer.button[b].value == INPUT_KEY_PRESSED;
        }
        break;
    default:
        check(0, "only input events and drops are queued", message->kind, 0);
        return;
    }
    id = tag_get(input);
    check(id > p->last, "events are read in the order they were queued", id,
          p->last);
    for (k = p->last + 1; k < id && k < PIPE_IDS; k++)
        if (!p->motion[k])
            check(p->dropped_since,
                  "an event lost, a motion apart, is told as a drop before the "
                  "next",
                  k, 0);
    p->last = id;
    p->dropped_since = 0;
}

static UINT32 pipe_send(void *context, const BACKLOG_MESSAGE *message)
{
    PIPE_CHANNEL *channel = (PIPE_CHANNEL *)context;

    if (channel->room == 0)
        return BACKLOG_SEND_FULL;
    if (channel->room != UNLIMITED)
        channel->room--;
    pipe_read(channel->pipe, message);
    return BACKLOG_SEND_TAKEN;
}

/* Let the channel take up to room messages. */
static UINT32 pipe_flush(PIPE *p, UINT32 room, UINT64 now)
{
    PIPE_CHANNEL channel;

    channel.pipe = p;
    channel.room = room;
    return backlog_flush(&p->backlog, pipe_send, &channel, now);
}

static void pipe_queue(PIPE *p, const ROUTED *routed)
{
    UINT32 i;

    for (i = 0; i < routed->count; i++) {
        WINDOW_INPUT input = routed->event[i];
        int was_owed = p->backlog.dropped != 0;

        if (p->sequence + 1 >= PIPE_IDS) {
            check(0, "a session queues fewer events than the check holds",
                  p->sequence, 0);
            return;
        }
        tag_set(&input, ++p->sequence);
        p->motion[p->sequence] = (unsigned char)input_is_motion(&input);
        backlog_push(&p->backlog, &input);
        if (!was_owed && p->backlog.dropped)
            p->owed++;
        check(p->backlog.count <= WINDOW_BACKLOG, "no more than the limit wait",
              p->backlog.count, 0);
    }
}

/* ------------------------------------------------------------------------
   Sessions */

#define MOOD_STINGY 0  /* the channel takes a little, now and then */
#define MOOD_EASY 1    /* it takes most of what there is, often */
#define MOOD_STARVED 2 /* it takes nothing until the session ends */
#define MOODS 3

typedef struct {
    ROUTE route;
    MODEL model;
    WATCH watch;
    PIPE pipe;
    INPUT_DEVICE device[SOURCES];
    long long width, height;
    WINDOW_RECORD window;
    int have_window;
    int down[SOURCES][BUTTON_CODES]; /* what each source's driver holds down */
    unsigned release_odds;           /* a held button is let go in one of this
                                        many reports that touch it */
    unsigned stray_odds;             /* 0, or one change in this many is the
                                        wrong one: a press repeated or a
                                        release of what is up */
    unsigned mood;                   /* of the window's channel */
    int churn;                       /* windows close and open anew */
    UINT64 time;
} SESSION;

static void random_range(long long *min, long long *max)
{
    switch (below(5)) {
    case 0:
        *min = 0;
        *max = 32767;
        break;
    case 1:
        *min = between(-1000, 1000);
        *max = *min + between(1, 40);
        break;
    case 2:
        *min = between(-2147483648ll, 0);
        *max = between(1, INT32_GREATEST);
        break;
    default:
        *min = between(-5000, 5000);
        *max = *min + between(1, 60000);
        break;
    }
}

/* A window within the screen, shown or made and not yet shown. */
static WINDOW_RECORD random_window(long long width, long long height)
{
    long long w = between(1, width);
    long long h = between(1, height);

    return make_window((UINT32)between(0, width - w),
                       (UINT32)between(0, height - h), (UINT32)w, (UINT32)h,
                       below(10) < 7 ? 0 : WINDOW_NO_BUFFER);
}

static void random_screen(SESSION *s)
{
    s->width = below(3) == 0 ? between(1, 9000) : 1280 + 640 * between(0, 3);
    s->height = below(3) == 0 ? between(1, 5000) : s->width * 5 / 8 + 1;
}

static void session_start(SESSION *s)
{
    static const unsigned release_odds[] = {1, 1, 2, 4, 12};
    long long x_min, x_max, y_min, y_max;

    memset(s, 0, sizeof *s);
    random_range(&x_min, &x_max);
    random_range(&y_min, &y_max);
    s->device[SOURCE_TABLET] =
        make_device(INPUT_AXIS_X | INPUT_AXIS_Y, x_min, x_max, y_min, y_max);
    s->device[SOURCE_MOUSE] = make_device(0, 0, 0, 0, 0);
    random_range(&x_min, &x_max);
    random_range(&y_min, &y_max);
    s->device[SOURCE_SPARE] =
        make_device(INPUT_AXIS_X | INPUT_AXIS_Y, x_min, x_max, y_min, y_max);
    random_screen(s);
    s->window = random_window(s->width, s->height);
    s->have_window = 1;
    s->release_odds =
        release_odds[below(sizeof release_odds / sizeof release_odds[0])];
    s->stray_odds = below(2) == 0 ? 0 : 4 + (unsigned)below(8);
    s->mood = (unsigned)below(MOODS);
    s->churn = below(3) == 0;
}

/* A raw value on an axis that puts the pointer near pixel, perturbed, so
   that edges are met often. */
static long long value_near(long long pixel, long long min, long long max,
                            long long extent)
{
    long long span = max - min + 1;
    long long value = min + pixel * span / extent + between(-1, 1);

    return value < min - 2 ? min - 2 : value;
}

/* A pixel on an axis: near an edge of the window or anywhere. */
static long long pixel_near(long long extent, long long origin, long long size)
{
    long long pixel;

    switch (below(4)) {
    case 0:
        pixel = origin + between(-2, 1);
        break;
    case 1:
        pixel = origin + size + between(-2, 1);
        break;
    case 2:
        pixel = origin + between(0, size - 1);
        break;
    default:
        pixel = between(0, extent - 1);
        break;
    }
    return pixel < 0 ? 0 : pixel >= extent ? extent - 1 : pixel;
}

/* Up to count changes of the buttons of source d, each button at most once:
   a press of one up, a release of one held when the session lets it go, and
   now and then the wrong one, as a driver gone wrong reports. */
static UINT32 random_changes(SESSION *s, int d, UINT32 count, CHANGE *changes)
{
    UINT32 made = 0, tries;
    int used[BUTTON_CODES];

    memset(used, 0, sizeof used);
    for (tries = 0; tries < count; tries++) {
        int c = (int)below(BUTTON_CODES);
        UINT32 value = s->down[d][c] ? INPUT_KEY_RELEASED : INPUT_KEY_PRESSED;

        if (used[c] || (s->down[d][c] && below(s->release_odds) != 0))
            continue;
        used[c] = 1;
        if (s->stray_odds != 0 && below(s->stray_odds) == 0)
            value = value == INPUT_KEY_PRESSED ? INPUT_KEY_RELEASED
                                               : INPUT_KEY_PRESSED;
        changes[made].code = (UINT32)(BUTTON_FIRST + c);
        changes[made].value = value;
        s->down[d][c] = value == INPUT_KEY_PRESSED;
        made++;
    }
    return made;
}

static long long random_wheel(void)
{
    switch (below(12)) {
    case 0:
        return 1;
    case 1:
        return -1;
    case 2:
        return between(-5, 5);
    case 3:
        return INT32_GREATEST;
    case 4:
        return -INT32_GREATEST - 1;
    case 5:
        return between(-20000000, 20000000);
    default:
        return 0;
    }
}

static INPUT_POINTER random_report(SESSION *s, int d)
{
    CHANGE changes[INPUT_REPORT_BUTTONS];
    const INPUT_DEVICE *device = &s->device[d];
    UINT32 absolute = 0, count;
    long long x = 0, y = 0;
    long long wheel = random_wheel();
    INPUT_POINTER report;

    if (device->absolute != 0) {
        long long px, py;

        switch (below(10)) {
        case 0:
            absolute = INPUT_AXIS_X;
            break;
        case 1:
            absolute = INPUT_AXIS_Y;
            break;
        default:
            absolute = INPUT_AXIS_X | INPUT_AXIS_Y;
            break;
        }
        px = pixel_near(s->width, s->window.x, s->window.width);
        py = pixel_near(s->height, s->window.y, s->window.height);
        x = value_near(px, device->x.min, device->x.max, s->width);
        y = value_near(py, device->y.min, device->y.max, s->height);
    }
    count = below(3) == 0 ? random_changes(s, d, 1 + (UINT32)below(3), changes)
                          : 0;
    report = make_report(absolute, x, y, wheel, changes, count,
                         s->time += TIME_STEP);
    report.dx = (INT32)between(-3, 3);
    report.dy = (INT32)between(-3, 3);
    return report;
}

static void session_compare(SESSION *s, const ROUTED *routed,
                            const EXPECTED *want, const char *what)
{
    check(same_events(routed, want), what, routed->count, want->count);
    watch_events(&s->watch, routed);
    /* A window that was sent a button as pressed has the pointer. */
    if (watch_pressed(&s->watch))
        check(s->watch.in, "a window with a button down has the pointer", 0, 0);
    check(s->watch.in == s->model.in, "the stream and the model agree on entry",
          s->watch.in, s->model.in);
    /* A window with the pointer and no button down is under it, with
       nothing held. */
    if (s->watch.in && !watch_pressed(&s->watch))
        check(s->model.pairs == 0 &&
                  model_over(&s->model, s->have_window ? &s->window : 0),
              "a window with the pointer and no button down is under it", 0,
              0);
    check(routed->count <= ROUTE_EVENTS_MAX, "at most two events",
          routed->count, 0);
    check(s->route.entered == (UINT32)s->model.in, "the route's entry state",
          s->route.entered, s->model.in);
    check(s->route.held == (UINT32)s->model.pairs,
          "the buttons held are the model's", s->route.held, s->model.pairs);
    pipe_queue(&s->pipe, routed);
}

static void session_report(SESSION *s, int d)
{
    INPUT_POINTER report = random_report(s, d);
    const WINDOW_RECORD *w = s->have_window ? &s->window : 0;
    EXPECTED want;
    ROUTED routed;

    send_report(&s->route, source_number[d], &s->device[d], (UINT32)s->width,
                (UINT32)s->height, w, &report, &routed);
    model_report(&s->model, d, &s->device[d], s->width, s->height, w, &report,
                 &want);
    session_compare(s, &routed, &want,
                    "a report gives the events the model gives");
}

static void session_settle(SESSION *s)
{
    const WINDOW_RECORD *w = s->have_window ? &s->window : 0;
    EXPECTED want;
    ROUTED routed;

    s->time += TIME_STEP;
    send_settle(&s->route, w, s->time, &routed);
    model_settle(&s->model, w, s->time, &want);
    session_compare(s, &routed, &want,
                    "settling gives the events the model gives");
}

static void session_gone(SESSION *s, int d)
{
    const WINDOW_RECORD *w = s->have_window ? &s->window : 0;
    EXPECTED want;
    ROUTED routed;

    s->time += TIME_STEP;
    send_gone(&s->route, source_number[d], w, s->time, &routed);
    model_source_gone(&s->model, d, w, s->time, &want);
    memset(s->down[d], 0, sizeof s->down[d]);
    session_compare(s, &routed, &want,
                    "a source that goes gives the events the model gives");
}

/* The window closes: what the window was sent is no more. */
static void session_close(SESSION *s)
{
    s->have_window = 0;
    memset(&s->watch, 0, sizeof s->watch);
    memset(&s->pipe, 0, sizeof s->pipe);
}

/* The window is moved, or resized, or the screen changes size and the window
   is placed again; and the compositor, now and then, settles the pointer. */
static void session_place(SESSION *s, int screen)
{
    WINDOW_RECORD *w = &s->window;
    UINT32 shown = w->shown;
    int how = screen ? 0 : (int)below(3);

    if (screen)
        random_screen(s);
    if (how == 0) {
        *w = random_window(s->width, s->height);
        w->shown = shown;
    } else if (how == 1) {
        w->x = (UINT32)between(0, s->width - w->width);
        w->y = (UINT32)between(0, s->height - w->height);
    } else {
        w->width = (UINT32)between(1, s->width - w->x);
        w->height = (UINT32)between(1, s->height - w->y);
    }
    if (below(2))
        session_settle(s);
}

/* The window's channel takes what it will, as its mood is. */
static void session_pace(SESSION *s)
{
    switch (s->mood) {
    case MOOD_STINGY:
        if (below(16) == 0)
            pipe_flush(&s->pipe, 1 + (UINT32)below(4), s->time);
        break;
    case MOOD_EASY:
        if (below(2) == 0)
            pipe_flush(&s->pipe,
                       below(3) == 0 ? UNLIMITED : 1 + (UINT32)below(6),
                       s->time);
        break;
    default:
        break;
    }
}

static void session_step(SESSION *s)
{
    switch (below(34)) {
    case 0:
        if (s->have_window && s->window.shown == WINDOW_NO_BUFFER) {
            s->window.shown = (UINT32)below(WINDOW_BUFFERS_MAX);
            session_settle(s);
        } else if (!s->have_window) {
            s->window = random_window(s->width, s->height);
            s->have_window = 1;
        } else {
            session_settle(s);
        }
        break;
    case 1:
        /* A window closes, and the compositor says so, or only stops
           passing it, which the next call takes for a close. */
        if (s->have_window && s->churn) {
            session_close(s);
            if (below(2)) {
                route_window_closed(&s->route);
                model_closed(&s->model);
            } else {
                session_settle(s);
            }
        }
        break;
    case 2:
    case 3:
        if (s->have_window)
            session_place(s, 0);
        break;
    case 4:
        if (s->have_window)
            session_place(s, 1);
        else
            random_screen(s);
        break;
    case 5:
        session_gone(s, (int)below(SOURCES));
        break;
    case 6:
    case 7:
    case 8:
    case 9:
        session_report(s, SOURCE_MOUSE);
        break;
    case 10:
    case 11:
    case 12:
    case 13:
        session_report(s, SOURCE_SPARE);
        break;
    default:
        session_report(s, SOURCE_TABLET);
        break;
    }
    session_pace(s);
}

/* After every button is let go and the queue drains: no drop is owed and
   each was told once, no button is down in what the window read, and its
   idea of the pointer is the route's, or, where a drop was told, is no more
   than that. */
static void session_drain(SESSION *s)
{
    PIPE *p = &s->pipe;
    UINT32 status = pipe_flush(p, UNLIMITED, s->time);
    int c;

    check(status == BACKLOG_IDLE && p->backlog.count == 0 &&
              !p->backlog.dropped,
          "the queue drains", status, p->backlog.count);
    check(p->told == p->owed, "a drop owed is told once", p->told, p->owed);
    for (c = 0; c < BUTTON_CODES; c++)
        check(!p->pressed[c], "no button is left down in what the window read",
              c, 0);
    if (p->told == 0)
        check(p->in == (int)s->route.entered,
              "with nothing dropped the window's idea of the pointer is the "
              "route's",
              p->in, s->route.entered);
    else
        check(!p->in || s->route.entered,
              "after a drop the window thinks the pointer in only where it is",
              p->in, s->route.entered);
}

/* After every button is released, and the pointer settled under a window
   that appeared while it stood, the window has the pointer exactly if the
   pointer is over it. */
static void session_end(SESSION *s)
{
    int d, c;

    for (d = 0; d < SOURCES; d++)
        for (c = 0; c < BUTTON_CODES; c++) {
            if (s->down[d][c]) {
                CHANGE release;
                INPUT_POINTER report;
                const WINDOW_RECORD *w = s->have_window ? &s->window : 0;
                EXPECTED want;
                ROUTED routed;

                release.code = (UINT32)(BUTTON_FIRST + c);
                release.value = INPUT_KEY_RELEASED;
                report = make_report(0, 0, 0, 0, &release, 1,
                                     s->time += TIME_STEP);
                send_report(&s->route, source_number[d], &s->device[d],
                            (UINT32)s->width, (UINT32)s->height, w, &report,
                            &routed);
                model_report(&s->model, d, &s->device[d], s->width, s->height,
                             w, &report, &want);
                s->down[d][c] = 0;
                session_compare(s, &routed, &want,
                                "a release gives the events the model gives");
            }
        }
    check(s->route.held == 0 && s->route.grab == 0,
          "every button released, nothing is held", s->route.held,
          s->route.grab);
    session_settle(s);
    check(s->watch.in == (model_over(&s->model, s->have_window ? &s->window : 0)
                              ? 1
                              : 0),
          "with no button held the window has the pointer if it is over it",
          s->watch.in, 0);
    check(!watch_pressed(&s->watch), "no button is left down in the window", 0,
          0);
    session_drain(s);
}

static void check_sessions(void)
{
    unsigned n, step;
    static SESSION s;

    for (n = 0; n < SESSIONS; n++) {
        session_start(&s);
        /* A channel that takes nothing until the end has long to fill. */
        for (step = 0; step < SESSION_STEPS * (s.mood == MOOD_STARVED ? 3 : 1);
             step++)
            session_step(&s);
        session_end(&s);
    }
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    check_axes();
    check_tables();
    check_sessions();
    printf("%llu checks, %u failed\n", checks, failures);
    return failures != 0;
}
