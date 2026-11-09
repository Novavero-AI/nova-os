/* How a pointing device's reports reach the window (docs/spec/display.md
   D10 rule 4, D11 rules 3 and 6, D19's phase 3 arrangement), as decisions
   over a report's fields and a ROUTE. See route.h. */

#include "route.h"

#define BOTH_AXES (INPUT_AXIS_X | INPUT_AXIS_Y)

#define INT32_GREATEST 0x7FFFFFFF
#define INT32_LEAST (-INT32_GREATEST - 1)

/* ------------------------------------------------------------------------
   Where the pointer is. */

UINT32 route_axis(INT32 value, const INPUT_RANGE *range, UINT32 extent)
{
    INT64 least = range->min;
    INT64 greatest = range->max;
    INT64 clamped = value;
    UINT64 along, span;

    if (greatest < least || extent == 0)
        return 0;
    if (extent > ROUTE_EXTENT_MAX)
        extent = ROUTE_EXTENT_MAX;
    if (clamped < least)
        clamped = least;
    if (clamped > greatest)
        clamped = greatest;
    /* along is below 2^32, extent below 2^23 and the unit 2^8, so the
       product fits 64 bits. */
    along = (UINT64)(clamped - least);
    span = (UINT64)(greatest - least) + 1;
    return (UINT32)(along * extent * DISPLAY_POSITION_ONE / span);
}

UINT32 route_placed(const ROUTE *route) { return route->known == BOTH_AXES; }

/* Set the place of each axis the report sets and the device has. */
static void place(ROUTE *route, const INPUT_DEVICE *device, UINT32 width,
                  UINT32 height, const INPUT_POINTER *report)
{
    UINT32 axes = report->absolute & device->absolute;

    if (axes & INPUT_AXIS_X) {
        route->x = route_axis(report->x, &device->x, width);
        route->known |= INPUT_AXIS_X;
    }
    if (axes & INPUT_AXIS_Y) {
        route->y = route_axis(report->y, &device->y, height);
        route->known |= INPUT_AXIS_Y;
    }
}

/* Whether the pixel the pointer's place lies in is one of window's. */
static UINT32 over(const ROUTE *route, const WINDOW_RECORD *window)
{
    UINT64 column, row;

    if (!route_placed(route) || window == 0 ||
        window->shown == WINDOW_NO_BUFFER)
        return 0;
    column = route->x / DISPLAY_POSITION_ONE;
    row = route->y / DISPLAY_POSITION_ONE;
    return column >= window->x && column - window->x < window->width &&
           row >= window->y && row - window->y < window->height;
}

/* ------------------------------------------------------------------------
   What the window is sent. */

static INT32 saturated(INT64 value)
{
    return (INT32)(value > INT32_GREATEST ? INT32_GREATEST
                   : value < INT32_LEAST  ? INT32_LEAST
                                          : value);
}

/* position, a place on one axis, relative to origin, the window's edge
   on it in pixels. */
static INT32 relative(UINT32 position, UINT32 origin)
{
    return saturated((INT64)position - (INT64)origin * DISPLAY_POSITION_ONE);
}

/* Bytes, not a structure assignment or initializer: TinyCC turns those
   into calls to memmove and memset, and no program here links a C library
   (docs/spec/interface.md P2). */
static void clear(WINDOW_INPUT *input)
{
    UINT8 *bytes = (UINT8 *)input;
    UINT32 i;

    for (i = 0; i < sizeof *input; i++)
        bytes[i] = 0;
}

static void copy(WINDOW_INPUT *to, const WINDOW_INPUT *from)
{
    const UINT8 *source = (const UINT8 *)from;
    UINT8 *bytes = (UINT8 *)to;
    UINT32 i;

    for (i = 0; i < sizeof *to; i++)
        bytes[i] = source[i];
}

static void add_enter(ROUTED *routed, const ROUTE *route,
                      const WINDOW_RECORD *window, UINT64 time)
{
    WINDOW_INPUT *input = &routed->event[routed->count++];

    clear(input);
    input->kind = WINDOW_ENTER;
    input->body.enter.x = relative(route->x, window->x);
    input->body.enter.y = relative(route->y, window->y);
    input->body.enter.time = time;
}

static void add_leave(ROUTED *routed)
{
    WINDOW_INPUT *input = &routed->event[routed->count++];

    clear(input);
    input->kind = WINDOW_LEAVE;
}

/* Whether the window has the pointer, given whether it is over the
   window: a held button decides, and else the place does. */
static UINT32 has_pointer(const ROUTE *route, UINT32 is_over)
{
    return route->held ? route->grab == ROUTE_GRAB_WINDOW : is_over;
}

/* ------------------------------------------------------------------------
   The hold. */

/* The index in route->button of the button of source and code, or
   route->held where it is not held. */
static UINT32 find(const ROUTE *route, UINT32 source, UINT32 code)
{
    UINT32 i;

    for (i = 0; i < route->held; i++)
        if (route->button[i].source == source && route->button[i].code == code)
            return i;
    return route->held;
}

/* Whether a button of code is held, by any source. */
static UINT32 held_code(const ROUTE *route, UINT32 code)
{
    UINT32 i;

    for (i = 0; i < route->held; i++)
        if (route->button[i].code == code)
            return 1;
    return 0;
}

/* Put a change of a button in pointer, which has room for as many as a
   report changes. */
static void add_change(WINDOW_POINTER_EVENT *pointer, UINT32 code, UINT32 value)
{
    pointer->button[pointer->buttons].code = (UINT16)code;
    pointer->button[pointer->buttons].value = (UINT16)value;
    pointer->buttons++;
}

/* A press, over the window or not as is_over says. A press with nothing
   held starts the hold, to the window if the pointer is over it. It goes
   to the window if the hold is to the window and no source held the code
   already. A press of a button held, or with no room to hold another, is
   ignored. */
static void press(ROUTE *route, UINT32 source, UINT32 code, UINT32 is_over,
                  WINDOW_POINTER_EVENT *pointer)
{
    if (find(route, source, code) != route->held ||
        route->held == ROUTE_HELD_MAX)
        return;
    if (route->held == 0)
        route->grab = is_over ? ROUTE_GRAB_WINDOW : ROUTE_GRAB_OUTSIDE;
    if (route->grab == ROUTE_GRAB_WINDOW && !held_code(route, code))
        add_change(pointer, code, INPUT_KEY_PRESSED);
    route->button[route->held].source = source;
    route->button[route->held].code = code;
    route->held++;
}

/* A release. It goes to the window if the hold, as the release finds it,
   is to the window and no source holds the code still, so the last release
   of a hold goes to the window before the hold ends. A release of a button
   not held is ignored. */
static void release(ROUTE *route, UINT32 source, UINT32 code,
                    WINDOW_POINTER_EVENT *pointer)
{
    UINT32 i = find(route, source, code);

    if (i == route->held)
        return;
    route->held--;
    for (; i < route->held; i++) {
        route->button[i].source = route->button[i + 1].source;
        route->button[i].code = route->button[i + 1].code;
    }
    if (route->grab == ROUTE_GRAB_WINDOW && !held_code(route, code))
        add_change(pointer, code, INPUT_KEY_RELEASED);
    if (route->held == 0)
        route->grab = 0;
}

/* Take each button change of report, from source, in order. */
static void take_buttons(ROUTE *route, UINT32 source,
                         const INPUT_POINTER *report, UINT32 is_over,
                         WINDOW_POINTER_EVENT *pointer)
{
    UINT32 count = report->buttons < INPUT_REPORT_BUTTONS
                       ? report->buttons
                       : INPUT_REPORT_BUTTONS;
    UINT32 i;

    for (i = 0; i < count; i++) {
        const INPUT_BUTTON *change = &report->button[i];

        if (change->value == INPUT_KEY_PRESSED)
            press(route, source, change->code, is_over, pointer);
        else
            release(route, source, change->code, pointer);
    }
}

/* Release every button source holds. */
static void drop_source(ROUTE *route, UINT32 source,
                        WINDOW_POINTER_EVENT *pointer)
{
    UINT32 i = 0;

    while (i < route->held) {
        if (route->button[i].source == source)
            release(route, source, route->button[i].code, pointer);
        else
            i++;
    }
}

/* ------------------------------------------------------------------------
   The reports. */

/* What a report, a settling or a source's going comes to before the
   window is told. */
typedef struct {
    WINDOW_INPUT sent;   /* the pointer event the window may be sent: the
                            changes of buttons it sees, so far */
    UINT32 was_entered;  /* the window had the pointer before */
    UINT32 moved;        /* the pointer's place changed */
    INT64 wheel;         /* the wheel's turn, in DISPLAY_WHEEL_DETENT units */
    UINT64 time;         /* the time to stamp what is sent with */
} TURN;

static void begin(TURN *turn, const ROUTE *route, UINT64 time)
{
    clear(&turn->sent);
    turn->sent.kind = WINDOW_POINTER;
    turn->was_entered = route->entered;
    turn->moved = 0;
    turn->wheel = 0;
    turn->time = time;
}

/* Set *routed to what the window is sent, once its buttons have changed:
   the entry, the pointer event and the leave, in that order. Whether the
   window has the pointer is decided here, after the buttons, and decides
   the wheel as it does the entry and the leave. A window that is not there
   has closed, and is sent nothing, so that no event carries a place on
   the screen. */
static void conclude(ROUTE *route, const WINDOW_RECORD *window, TURN *turn,
                     ROUTED *routed)
{
    WINDOW_POINTER_EVENT *pointer = &turn->sent.body.pointer;
    UINT32 inside;

    routed->count = 0;
    if (window == 0) {
        route_window_closed(route);
        return;
    }
    inside = has_pointer(route, over(route, window));
    if (inside && !turn->was_entered)
        add_enter(routed, route, window, turn->time);
    if (inside)
        pointer->wheel = saturated(turn->wheel);
    if (pointer->buttons != 0 ||
        (inside &&
         (pointer->wheel != 0 || (turn->moved && turn->was_entered)))) {
        pointer->x = relative(route->x, window->x);
        pointer->y = relative(route->y, window->y);
        pointer->time = turn->time;
        copy(&routed->event[routed->count], &turn->sent);
        routed->count++;
    }
    if (!inside && turn->was_entered)
        add_leave(routed);
    route->entered = inside;
}

void route_report(ROUTE *route, UINT32 source, const INPUT_DEVICE *device,
                  UINT32 width, UINT32 height, const WINDOW_RECORD *window,
                  const INPUT_POINTER *report, ROUTED *routed)
{
    UINT32 was_x = route->x;
    UINT32 was_y = route->y;
    TURN turn;

    begin(&turn, route, report->time);
    place(route, device, width, height, report);
    turn.moved = route->x != was_x || route->y != was_y;
    take_buttons(route, source, report, over(route, window),
                 &turn.sent.body.pointer);
    turn.wheel = (INT64)report->wheel * DISPLAY_WHEEL_DETENT;
    conclude(route, window, &turn, routed);
}

void route_settle(ROUTE *route, const WINDOW_RECORD *window, UINT64 time,
                  ROUTED *routed)
{
    TURN turn;

    begin(&turn, route, time);
    conclude(route, window, &turn, routed);
}

void route_source_gone(ROUTE *route, UINT32 source, const WINDOW_RECORD *window,
                       UINT64 time, ROUTED *routed)
{
    TURN turn;

    begin(&turn, route, time);
    drop_source(route, source, &turn.sent.body.pointer);
    conclude(route, window, &turn, routed);
}

void route_window_closed(ROUTE *route)
{
    route->entered = 0;
    if (route->held)
        route->grab = ROUTE_GRAB_OUTSIDE;
}
