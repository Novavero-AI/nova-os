/* What a client does with an input endpoint's messages (docs/spec/input.md
   K2 and K3, docs/spec/console.md C6): the checks on a message and the
   log lines that say what it is. */

#include "endpoint.h"

#define BOTH_AXES (INPUT_AXIS_X | INPUT_AXIS_Y)

/* ------------------------------------------------------------------------
   Checks. */

int endpoint_is_description(const INPUT_DEVICE *d, UINT32 byte_count)
{
    if (byte_count != sizeof *d || d->transaction != 0 ||
        d->type != INPUT_TYPE_DEVICE || d->reserved != 0 ||
        (d->absolute & ~(UINT32)BOTH_AXES) != 0)
        return 0;
    if (d->kind == INPUT_DEVICE_KEYBOARD)
        return d->absolute == 0;
    return d->kind == INPUT_DEVICE_POINTER &&
           (!(d->absolute & INPUT_AXIS_X) || d->x.max > d->x.min) &&
           (!(d->absolute & INPUT_AXIS_Y) || d->y.max > d->y.min);
}

int endpoint_is_key_event(const INPUT_EVENT *event, UINT32 byte_count)
{
    return byte_count == sizeof *event && event->transaction == 0 &&
           event->type == INPUT_TYPE_KEY && event->value <= INPUT_KEY_REPEATED &&
           event->reserved == 0;
}

int endpoint_is_report(const INPUT_POINTER *report, UINT32 byte_count,
                       UINT32 axes)
{
    UINT32 i;

    if (byte_count != sizeof *report || report->transaction != 0 ||
        report->type != INPUT_TYPE_SYN || report->code != INPUT_SYN_REPORT ||
        report->reserved != 0 || (report->absolute & ~axes) != 0 ||
        report->buttons > INPUT_REPORT_BUTTONS)
        return 0;
    for (i = 0; i < report->buttons; i++)
        if (report->button[i].value != INPUT_KEY_PRESSED &&
            report->button[i].value != INPUT_KEY_RELEASED)
            return 0;
    return 1;
}

/* Bytes, not a structure assignment: TinyCC turns that into a call to
   memmove, and no program here links a C library (docs/spec/interface.md
   P2). */
static void keep(INPUT_DEVICE *device, const INPUT_DEVICE *description)
{
    const UINT8 *from = (const UINT8 *)description;
    UINT8 *to = (UINT8 *)device;
    UINT32 i;

    for (i = 0; i < sizeof *device; i++)
        to[i] = from[i];
}

UINT32 endpoint_take(INPUT_DEVICE *device, const void *message,
                     UINT32 byte_count)
{
    if (device->kind == 0) {
        if (!endpoint_is_description(message, byte_count))
            return ENDPOINT_REFUSED;
        keep(device, message);
        return ENDPOINT_DESCRIPTION;
    }
    if (device->kind == INPUT_DEVICE_KEYBOARD)
        return endpoint_is_key_event(message, byte_count) ? ENDPOINT_KEY
                                                          : ENDPOINT_REFUSED;
    return endpoint_is_report(message, byte_count, device->absolute)
               ? ENDPOINT_REPORT
               : ENDPOINT_REFUSED;
}

/* ------------------------------------------------------------------------
   Lines. */

/* Add n to the line, in decimal, after a minus sign if it is below 0. */
static void line_signed(LINE *l, INT32 n)
{
    if (n < 0)
        line_add(l, "-");
    line_number(l, n < 0 ? (UINT64)(-(INT64)n) : (UINT64)n);
}

static void line_range(LINE *l, const char *key, const INPUT_RANGE *r)
{
    line_add(l, key);
    line_signed(l, r->min);
    line_add(l, "..");
    line_signed(l, r->max);
}

void endpoint_describe(const char *component, const INPUT_DEVICE *d,
                       LINE_WRITE write)
{
    LINE l;

    l.length = 0;
    line_add(&l, component);
    line_add(&l, ": input kind=");
    if (d->kind == INPUT_DEVICE_KEYBOARD) {
        line_add(&l, "keyboard");
    } else {
        line_add(&l, "pointer");
        if (d->absolute & INPUT_AXIS_X)
            line_range(&l, " x=", &d->x);
        if (d->absolute & INPUT_AXIS_Y)
            line_range(&l, " y=", &d->y);
    }
    line_write(&l, write);
}

/* What a pointer line notes, as a kind and its detail: a motion,
   absolute or relative, with the axes it moves; a button pressed or
   released, with its code; the wheel turned up or down. */
#define NOTE_ABSOLUTE 0x10000
#define NOTE_RELATIVE 0x20000
#define NOTE_PRESSED 0x30000
#define NOTE_RELEASED 0x40000
#define NOTE_WHEEL 0x50000
#define NOTE_KIND 0xF0000
#define NOTE_DETAIL 0xFFFF
#define WHEEL_UP 0
#define WHEEL_DOWN 1

/* evdev's BTN_MISC, below which every key code is a keyboard's key, and
   BTN_LEFT, with its names for that code and the seven after it, up to
   BTN_TASK. */
#define BTN_MISC 0x100
#define BTN_LEFT 0x110
static const char *const button_names[] = {
    "left", "right", "middle", "side", "extra", "forward", "back", "task"};
#define BUTTON_NAMES (sizeof button_names / sizeof button_names[0])

static void line_axes(LINE *l, UINT32 axes)
{
    line_add(l, " axes=");
    if (axes == BOTH_AXES)
        line_add(l, "xy");
    else
        line_add(l, axes == INPUT_AXIS_X ? "x" : "y");
}

/* A button by its evdev name, a code past those by its number, and a
   keyboard's key by neither, since its code is what was typed. */
static void line_button(LINE *l, UINT32 code)
{
    line_add(l, " button=");
    if (code >= BTN_LEFT && code - BTN_LEFT < BUTTON_NAMES)
        line_add(l, button_names[code - BTN_LEFT]);
    else if (code >= BTN_MISC)
        line_number(l, code);
    else
        line_add(l, "key");
}

/* Whether note says what the note before it did: the same motion's
   kind, whatever axes it moves, or the same button change or turn. */
static int noted(const ENDPOINT_NOTES *notes, UINT32 note)
{
    UINT32 kind = note & NOTE_KIND;

    if (kind == NOTE_ABSOLUTE || kind == NOTE_RELATIVE)
        return kind == (notes->noted & NOTE_KIND);
    return note == notes->noted;
}

/* Log note unless it says what the one before did, so that a run of
   motion of one kind, or of turns one way, is one line. */
static void pointer_line(ENDPOINT_NOTES *notes, const char *component,
                         UINT32 note, LINE_WRITE write)
{
    UINT32 detail = note & NOTE_DETAIL;
    LINE l;

    if (noted(notes, note))
        return;
    notes->noted = note;
    l.length = 0;
    line_add(&l, component);
    line_add(&l, ": pointer ");
    switch (note & NOTE_KIND) {
    case NOTE_ABSOLUTE:
        line_add(&l, "move mode=absolute");
        line_axes(&l, detail);
        break;
    case NOTE_RELATIVE:
        line_add(&l, "move mode=relative");
        line_axes(&l, detail);
        break;
    case NOTE_PRESSED:
        line_add(&l, "press");
        line_button(&l, detail);
        break;
    case NOTE_RELEASED:
        line_add(&l, "release");
        line_button(&l, detail);
        break;
    default:
        line_add(&l, detail == WHEEL_UP ? "wheel direction=up"
                                        : "wheel direction=down");
        break;
    }
    line_write(&l, write);
}

void endpoint_pointer(ENDPOINT_NOTES *notes, const char *component,
                      const INPUT_POINTER *report, LINE_WRITE write)
{
    UINT32 i;

    if (report->absolute)
        pointer_line(notes, component, NOTE_ABSOLUTE | report->absolute,
                     write);
    if (report->dx || report->dy)
        pointer_line(notes, component,
                     NOTE_RELATIVE | (report->dx ? INPUT_AXIS_X : 0) |
                         (report->dy ? INPUT_AXIS_Y : 0),
                     write);
    for (i = 0; i < report->buttons; i++)
        pointer_line(notes, component,
                     (report->button[i].value == INPUT_KEY_PRESSED
                          ? NOTE_PRESSED
                          : NOTE_RELEASED) |
                         report->button[i].code,
                     write);
    if (report->wheel)
        pointer_line(notes, component,
                     NOTE_WHEEL | (report->wheel > 0 ? WHEEL_UP : WHEEL_DOWN),
                     write);
}
