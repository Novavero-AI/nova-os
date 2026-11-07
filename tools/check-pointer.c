/* Checks on the host how a pointer's driver gathers events into reports
   (drivers/pointer.c, docs/spec/input.md K2): a tablet's absolute axes
   and a mouse's relative ones, the wheel, buttons pressed and released,
   a report split where it would change one button twice or hold too
   many, what is not taken, and SYN_DROPPED, which QEMU's devices never
   send. Prints each failure and a count, and exits nonzero if any check
   fails.

   cc -std=c11 -Wall -Wextra -pedantic -I include -I drivers \
     -o check-pointer tools/check-pointer.c drivers/pointer.c &&
     ./check-pointer */

#include <stdio.h>

#include "pointer.h"

/* evdev's codes, as the Linux input header numbers them, that the
   checks send besides pointer.h's. */
#define EV_KEY 0x01
#define EV_SYN 0x00
#define SYN_REPORT 0
#define BTN_LEFT 0x110
#define BTN_RIGHT 0x111
#define BTN_GEAR_UP 0x151
#define BTN_TRIGGER_HAPPY 0x2c0
#define KEY_A 30
#define REL_HWHEEL 0x06
#define PRESSED 1
#define RELEASED 0
#define REPEATED 2

#define TABLET_MAX 0x7FFF
#define INT32_LARGEST 0x7fffffff

static POINTER pointer;
static INPUT_POINTER report;
static unsigned failures;

static void check(int passed, const char *what)
{
    if (passed)
        return;
    printf("failed: %s\n", what);
    failures++;
}

static void declare(UINT8 *bitmap, UINT32 code)
{
    bitmap[code / 8] |= (UINT8)(1u << code % 8);
}

/* A tablet as QEMU's declares itself: ABS_X and ABS_Y with a range, the
   wheel, and its buttons, BTN_GEAR_UP among them; or a mouse, which has
   REL_X and REL_Y in place of the absolute axes. */
static void start(int tablet)
{
    UINT32 i;

    for (i = 0; i < POINTER_BITMAP_BYTES; i++) {
        pointer.keys[i] = 0;
        pointer.rel[i] = 0;
    }
    declare(pointer.keys, BTN_LEFT);
    declare(pointer.keys, BTN_RIGHT);
    declare(pointer.keys, BTN_GEAR_UP);
    declare(pointer.rel, POINTER_REL_WHEEL);
    pointer.absolute = 0;
    if (tablet) {
        pointer.absolute = INPUT_AXIS_X | INPUT_AXIS_Y;
    } else {
        declare(pointer.rel, POINTER_REL_X);
        declare(pointer.rel, POINTER_REL_Y);
    }
    pointer_start(&pointer);
}

/* Take one event; whether it ended a report, now in report. */
static int take(UINT16 type, UINT16 code, UINT32 value)
{
    return pointer_take(&pointer, type, code, value, &report);
}

static int sync(void)
{
    return take(EV_SYN, SYN_REPORT, 0);
}

static int is_change(UINT32 i, UINT16 code, UINT16 value)
{
    return i < report.buttons && report.button[i].code == code &&
           report.button[i].value == value;
}

static void check_absolute(void)
{
    start(1);
    check(!take(POINTER_EV_ABS, POINTER_ABS_X, 13579) &&
              !take(POINTER_EV_ABS, POINTER_ABS_Y, 24680) && sync(),
          "a tablet's move ends at SYN_REPORT");
    check(report.type == INPUT_TYPE_SYN && report.code == INPUT_SYN_REPORT &&
              report.transaction == 0,
          "a report is tagged EV_SYN SYN_REPORT");
    check(report.absolute == (INPUT_AXIS_X | INPUT_AXIS_Y) &&
              report.x == 13579 && report.y == 24680,
          "a move carries the device's own values");
    check(report.buttons == 0 && report.dx == 0 && report.dy == 0 &&
              report.wheel == 0,
          "a move carries nothing else");
    check(!take(POINTER_EV_ABS, POINTER_ABS_X, TABLET_MAX) && sync() &&
              report.absolute == INPUT_AXIS_X && report.x == TABLET_MAX &&
              report.y == 0,
          "a report sets only the axes the device sent");
    check(!sync(), "a SYN_REPORT after nothing sends nothing");
    check(!take(POINTER_EV_REL, POINTER_REL_X, 5) && !sync(),
          "a relative axis the device did not declare is not taken");
    pointer.absolute = INPUT_AXIS_X;
    check(!take(POINTER_EV_ABS, POINTER_ABS_Y, 5) && !sync(),
          "an absolute axis the device has no range for is not taken");
}

static void check_relative(void)
{
    start(0);
    check(!take(POINTER_EV_ABS, POINTER_ABS_X, 5) && !sync(),
          "a mouse's absolute axis is not taken");
    check(!take(POINTER_EV_REL, POINTER_REL_X, 3) &&
              !take(POINTER_EV_REL, POINTER_REL_X, 4) &&
              !take(POINTER_EV_REL, POINTER_REL_Y, (UINT32)-2) && sync() &&
              report.dx == 7 && report.dy == -2 && report.absolute == 0,
          "relative motion is summed, signed");
    check(!take(POINTER_EV_REL, POINTER_REL_X, INT32_LARGEST) &&
              !take(POINTER_EV_REL, POINTER_REL_X, 5) && sync() &&
              report.dx == INT32_LARGEST,
          "a sum is held within a signed 32-bit integer");
    check(!take(POINTER_EV_REL, POINTER_REL_X, 5) &&
              !take(POINTER_EV_REL, POINTER_REL_X, (UINT32)-5) && !sync(),
          "motion that sums to none sends nothing");
    check(!take(POINTER_EV_REL, POINTER_REL_WHEEL, (UINT32)-1) &&
              !take(POINTER_EV_REL, POINTER_REL_WHEEL, (UINT32)-2) &&
              sync() && report.wheel == -3,
          "the wheel's detents are summed");
    check(!take(POINTER_EV_REL, REL_HWHEEL, 1) && !sync(),
          "a relative axis the driver does not take is not taken");
}

static void check_buttons(void)
{
    start(1);
    check(!take(EV_KEY, BTN_LEFT, PRESSED) && sync() &&
              report.buttons == 1 && is_change(0, BTN_LEFT, PRESSED),
          "a press is a change");
    check(!take(EV_KEY, BTN_LEFT, PRESSED) &&
              !take(EV_KEY, BTN_LEFT, REPEATED) && !sync(),
          "a press of a button held, and a repeat, change nothing");
    check(!take(EV_KEY, BTN_LEFT, RELEASED) && sync() &&
              is_change(0, BTN_LEFT, RELEASED),
          "a release is a change");
    check(!take(EV_KEY, BTN_GEAR_UP, RELEASED) && !sync(),
          "a release of a button not held, as QEMU's wheel sends, is none");
    check(!take(EV_KEY, KEY_A, PRESSED) && !sync(),
          "a key the device did not declare is not taken");
    check(!take(EV_KEY, BTN_LEFT, PRESSED) &&
              !take(EV_KEY, BTN_RIGHT, PRESSED) && sync() &&
              report.buttons == 2 && is_change(0, BTN_LEFT, PRESSED) &&
              is_change(1, BTN_RIGHT, PRESSED),
          "changes come in the order the device made them");
    check(!take(POINTER_EV_ABS, POINTER_ABS_X, 1) &&
              take(EV_KEY, BTN_LEFT, RELEASED) == 0 &&
              take(EV_KEY, BTN_LEFT, PRESSED) && report.buttons == 1 &&
              is_change(0, BTN_LEFT, RELEASED) &&
              report.absolute == INPUT_AXIS_X,
          "a second change of one button ends the report before it");
    check(sync() && report.buttons == 1 && is_change(0, BTN_LEFT, PRESSED) &&
              report.absolute == 0,
          "and starts the next with it");
}

static void check_full(void)
{
    UINT32 i;
    int ended = 0;

    start(1);
    for (i = 0; i <= INPUT_REPORT_BUTTONS; i++)
        declare(pointer.keys, BTN_TRIGGER_HAPPY + i);
    for (i = 0; i < INPUT_REPORT_BUTTONS; i++)
        ended |= take(EV_KEY, (UINT16)(BTN_TRIGGER_HAPPY + i), PRESSED);
    check(!ended, "a report holds INPUT_REPORT_BUTTONS changes");
    check(take(EV_KEY, (UINT16)(BTN_TRIGGER_HAPPY + INPUT_REPORT_BUTTONS),
               PRESSED) &&
              report.buttons == INPUT_REPORT_BUTTONS,
          "one change more ends the report full");
    check(sync() && report.buttons == 1 &&
              is_change(0, (UINT16)(BTN_TRIGGER_HAPPY + INPUT_REPORT_BUTTONS),
                        PRESSED),
          "and starts the next with it");
}

static void check_dropped(void)
{
    start(1);
    check(!take(POINTER_EV_ABS, POINTER_ABS_X, 100) &&
              !take(EV_KEY, BTN_LEFT, PRESSED) &&
              !take(EV_SYN, POINTER_SYN_DROPPED, 0),
          "SYN_DROPPED sends nothing");
    check(!take(POINTER_EV_ABS, POINTER_ABS_Y, 200) &&
              !take(EV_KEY, BTN_RIGHT, PRESSED) && !sync(),
          "the events up to the next SYN_REPORT, and it, send nothing");
    check(!take(EV_KEY, BTN_LEFT, RELEASED) && !sync(),
          "a dropped press leaves the button released to the client");
    check(!take(POINTER_EV_ABS, POINTER_ABS_Y, 300) && sync() &&
              report.absolute == INPUT_AXIS_Y && report.y == 300 &&
              report.buttons == 0,
          "the report after the next SYN_REPORT holds its own events");
}

int main(void)
{
    check_absolute();
    check_relative();
    check_buttons();
    check_full();
    check_dropped();
    printf("%u failures\n", failures);
    return failures != 0;
}
