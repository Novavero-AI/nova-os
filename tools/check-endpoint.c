/* Checks what lib/endpoint.c says of an input endpoint's messages
   (docs/spec/input.md K2 and K3, docs/spec/console.md C6) on the host:
   which messages are a description, a key event or a pointer report,
   the text of the line each description and report makes, that a run of
   motion is one line, and that no line is longer than the log takes.
   Prints each failure and a count, and exits nonzero if any check fails.

   cc -std=c11 -Wall -Wextra -pedantic -I include -I lib \
     -o check-endpoint tools/check-endpoint.c lib/endpoint.c \
     && ./check-endpoint */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "endpoint.h"

#define TRANSACTION_CALL 1
#define BYTES_MORE 1
#define KEY_A 30
#define BTN_MISC 0x100
#define BTN_LEFT 0x110
#define BTN_RIGHT 0x111
#define BTN_TASK 0x117
#define BTN_AFTER_TASK 0x118
#define AXES_BOTH (INPUT_AXIS_X | INPUT_AXIS_Y)
#define AXES_JUNK 0x4
#define VALUE_PAST_REPEAT 3
#define VALUE_PAST_PRESS 2

/* The lines a write function was given, in order. */
#define LINES_MAX 256
static char lines[LINES_MAX][DEBUG_WRITE_MAX_BYTES];
static unsigned line_count;
static unsigned failures;

static void check(int passed, const char *what)
{
    if (passed)
        return;
    printf("failed: %s\n", what);
    failures++;
}

static void collect(const char *line)
{
    if (line_count < LINES_MAX && strlen(line) < DEBUG_WRITE_MAX_BYTES)
        strcpy(lines[line_count++], line);
    else
        failures++;
}

/* ------------------------------------------------------------------------
   Descriptions */

static INPUT_DEVICE device(UINT16 kind, UINT32 absolute, INT32 x_min,
                           INT32 x_max, INT32 y_min, INT32 y_max)
{
    INPUT_DEVICE d;

    memset(&d, 0, sizeof d);
    d.type = INPUT_TYPE_DEVICE;
    d.kind = kind;
    d.absolute = absolute;
    d.x.min = x_min;
    d.x.max = x_max;
    d.y.min = y_min;
    d.y.max = y_max;
    return d;
}

/* Whether an axis the description names has a range with its maximum
   above its minimum; an axis it does not name has any. */
static int axis_wanted(UINT32 named, INPUT_RANGE range)
{
    return !named || range.max > range.min;
}

/* K3 for a description of the right size, stated as the rule reads. */
static int description_wanted(const INPUT_DEVICE *d)
{
    int keyboard = d->kind == INPUT_DEVICE_KEYBOARD && d->absolute == 0;
    int pointer = d->kind == INPUT_DEVICE_POINTER &&
                  (d->absolute & ~(UINT32)AXES_BOTH) == 0 &&
                  axis_wanted(d->absolute & INPUT_AXIS_X, d->x) &&
                  axis_wanted(d->absolute & INPUT_AXIS_Y, d->y);

    return d->transaction == 0 && d->type == INPUT_TYPE_DEVICE &&
           d->reserved == 0 && (keyboard || pointer);
}

/* Every description over these values of each field, and every size
   a message may have, is one exactly where K3 says. */
static void check_descriptions(void)
{
    static const UINT16 kinds[] = {0, INPUT_DEVICE_KEYBOARD,
                                   INPUT_DEVICE_POINTER, 3};
    static const INPUT_RANGE ranges[] = {{0, 0},          {0, 1},
                                         {1, 0},          {-5, 5},
                                         {INT32_MIN, INT32_MAX},
                                         {INT32_MAX, INT32_MIN}};
    static const UINT32 transactions[] = {0, TRANSACTION_CALL};
    static const UINT16 types[] = {INPUT_TYPE_DEVICE, INPUT_TYPE_KEY, 0};
    static const UINT32 reserved[] = {0, 1};
    unsigned k, a, x, y, t, p, r, wrong = 0, valid = 0, cases = 0;

    for (k = 0; k < sizeof kinds / sizeof kinds[0]; k++)
        for (a = 0; a <= (AXES_BOTH | AXES_JUNK); a++)
            for (x = 0; x < sizeof ranges / sizeof ranges[0]; x++)
                for (y = 0; y < sizeof ranges / sizeof ranges[0]; y++)
                    for (t = 0; t < 2; t++)
                        for (p = 0; p < 3; p++)
                            for (r = 0; r < 2; r++) {
                                INPUT_DEVICE d = device(
                                    kinds[k], a, ranges[x].min, ranges[x].max,
                                    ranges[y].min, ranges[y].max);

                                d.transaction = transactions[t];
                                d.type = types[p];
                                d.reserved = reserved[r];
                                cases++;
                                wrong += endpoint_is_description(
                                             &d, sizeof d) !=
                                         description_wanted(&d);
                                wrong += endpoint_is_description(
                                    &d, sizeof d - BYTES_MORE);
                                wrong += endpoint_is_description(
                                    &d, sizeof d + BYTES_MORE);
                                valid += description_wanted(&d);
                            }
    check(wrong == 0, "a description is one exactly where K3 says, of its "
                      "exact size");
    check(valid > 100 && valid < cases, "the descriptions checked include "
                                        "valid and invalid ones");
}

/* ------------------------------------------------------------------------
   Key events */

static void check_key_events(void)
{
    static const UINT16 types[] = {INPUT_TYPE_KEY, INPUT_TYPE_SYN,
                                   INPUT_TYPE_DEVICE};
    unsigned p, v, t, wrong = 0;

    for (p = 0; p < sizeof types / sizeof types[0]; p++)
        for (v = 0; v <= VALUE_PAST_REPEAT; v++)
            for (t = 0; t < 2; t++) {
                INPUT_EVENT e;
                int wanted = types[p] == INPUT_TYPE_KEY &&
                             v <= INPUT_KEY_REPEATED && t == 0;

                memset(&e, 0, sizeof e);
                e.transaction = t ? TRANSACTION_CALL : 0;
                e.type = types[p];
                e.code = KEY_A;
                e.value = v;
                wrong += endpoint_is_key_event(&e, sizeof e) != wanted;
                wrong += endpoint_is_key_event(&e, sizeof e - BYTES_MORE);
                wrong += endpoint_is_key_event(&e, sizeof e + BYTES_MORE);
            }
    check(wrong == 0, "a key event is one exactly where K2 says, of its "
                      "exact size");
}

/* ------------------------------------------------------------------------
   Pointer reports */

static INPUT_POINTER report_new(UINT32 absolute, INT32 dx, INT32 dy,
                                INT32 wheel)
{
    INPUT_POINTER r;

    memset(&r, 0, sizeof r);
    r.type = INPUT_TYPE_SYN;
    r.code = INPUT_SYN_REPORT;
    r.absolute = absolute;
    r.dx = dx;
    r.dy = dy;
    r.wheel = wheel;
    return r;
}

static void report_button(INPUT_POINTER *r, UINT16 code, UINT16 value)
{
    r->button[r->buttons].code = code;
    r->button[r->buttons].value = value;
    r->buttons++;
}

static void check_reports(void)
{
    INPUT_POINTER r = report_new(INPUT_AXIS_X, 1, -1, 1);
    unsigned i, wrong = 0;

    report_button(&r, BTN_LEFT, INPUT_KEY_PRESSED);
    check(endpoint_is_report(&r, sizeof r, AXES_BOTH),
          "a report with motion, a wheel and a button is one");
    check(!endpoint_is_report(&r, sizeof r - BYTES_MORE, AXES_BOTH) &&
              !endpoint_is_report(&r, sizeof r + BYTES_MORE, AXES_BOTH),
          "a report is of its exact size");

    r.transaction = TRANSACTION_CALL;
    check(!endpoint_is_report(&r, sizeof r, AXES_BOTH),
          "a report answers no call");
    r.transaction = 0;
    r.type = INPUT_TYPE_KEY;
    check(!endpoint_is_report(&r, sizeof r, AXES_BOTH),
          "a report is tagged EV_SYN");
    r.type = INPUT_TYPE_SYN;
    r.code = INPUT_SYN_REPORT + 1;
    check(!endpoint_is_report(&r, sizeof r, AXES_BOTH),
          "a report is tagged SYN_REPORT");
    r.code = INPUT_SYN_REPORT;
    r.reserved = 1;
    check(!endpoint_is_report(&r, sizeof r, AXES_BOTH),
          "a report's reserved field is 0");
    r.reserved = 0;

    /* A report sets only axes its pointer has. */
    for (i = 0; i <= (AXES_BOTH | AXES_JUNK); i++) {
        UINT32 axes;

        for (axes = 0; axes <= AXES_BOTH; axes++) {
            INPUT_POINTER s = report_new(i, 0, 0, 0);

            wrong += endpoint_is_report(&s, sizeof s, axes) !=
                     ((i & ~axes) == 0);
        }
    }
    check(wrong == 0, "a report sets no absolute axis its pointer lacks");

    /* At most INPUT_REPORT_BUTTONS changes, each a press or a release. */
    for (i = 0; i <= INPUT_REPORT_BUTTONS + 1; i++) {
        INPUT_POINTER s = report_new(0, 0, 0, 0);
        UINT32 j;

        for (j = 0; j < i && j < INPUT_REPORT_BUTTONS; j++)
            report_button(&s, BTN_LEFT, INPUT_KEY_RELEASED);
        s.buttons = i;
        wrong += endpoint_is_report(&s, sizeof s, 0) !=
                 (i <= INPUT_REPORT_BUTTONS);
    }
    check(wrong == 0, "a report holds at most INPUT_REPORT_BUTTONS buttons");

    for (i = 0; i <= VALUE_PAST_PRESS + 1; i++) {
        INPUT_POINTER s = report_new(0, 0, 0, 0);

        report_button(&s, BTN_LEFT, (UINT16)i);
        wrong += endpoint_is_report(&s, sizeof s, 0) !=
                 (i == INPUT_KEY_PRESSED || i == INPUT_KEY_RELEASED);
    }
    check(wrong == 0, "each button change is a press or a release");

    /* The last of eight is checked as the first is. */
    {
        INPUT_POINTER s = report_new(0, 0, 0, 0);

        for (i = 0; i < INPUT_REPORT_BUTTONS; i++)
            report_button(&s, BTN_LEFT, INPUT_KEY_PRESSED);
        s.button[INPUT_REPORT_BUTTONS - 1].value = VALUE_PAST_PRESS;
        check(!endpoint_is_report(&s, sizeof s, 0),
              "the last button's value is checked");
    }
}

/* ------------------------------------------------------------------------
   Lines */

static void check_description_lines(void)
{
    INPUT_DEVICE keyboard = device(INPUT_DEVICE_KEYBOARD, 0, 0, 0, 0, 0);
    INPUT_DEVICE tablet = device(INPUT_DEVICE_POINTER, AXES_BOTH, 0, 32767,
                                 0, 32767);
    INPUT_DEVICE x_only = device(INPUT_DEVICE_POINTER, INPUT_AXIS_X, -5,
                                 100, 7, 7);
    INPUT_DEVICE y_only = device(INPUT_DEVICE_POINTER, INPUT_AXIS_Y, 9, 9,
                                 -32768, -1);
    INPUT_DEVICE mouse = device(INPUT_DEVICE_POINTER, 0, 0, 0, 0, 0);
    INPUT_DEVICE widest = device(INPUT_DEVICE_POINTER, AXES_BOTH, INT32_MIN,
                                 INT32_MAX, INT32_MIN, INT32_MAX);

    line_count = 0;
    endpoint_describe("nova-console", &keyboard, collect);
    endpoint_describe("nova-console", &tablet, collect);
    endpoint_describe("nova-compositor", &tablet, collect);
    endpoint_describe("nova-console", &x_only, collect);
    endpoint_describe("nova-console", &y_only, collect);
    endpoint_describe("nova-console", &mouse, collect);
    endpoint_describe("nova-console", &widest, collect);
    check(line_count == 7, "a description makes one line");
    check(!strcmp(lines[0], "nova-console: input kind=keyboard\r\n"),
          "a keyboard's line");
    check(!strcmp(lines[1], "nova-console: input kind=pointer x=0..32767 "
                            "y=0..32767\r\n"),
          "a tablet's line gives its ranges");
    check(!strcmp(lines[2], "nova-compositor: input kind=pointer x=0..32767 "
                            "y=0..32767\r\n"),
          "the line begins with the component given");
    check(!strcmp(lines[3], "nova-console: input kind=pointer x=-5..100\r\n"),
          "a pointer with the x axis alone gives its range");
    check(!strcmp(lines[4],
                  "nova-console: input kind=pointer y=-32768..-1\r\n"),
          "a pointer with the y axis alone gives its range");
    check(!strcmp(lines[5], "nova-console: input kind=pointer\r\n"),
          "a relative pointer gives no range");
    check(!strcmp(lines[6],
                  "nova-console: input kind=pointer x=-2147483648.."
                  "2147483647 y=-2147483648..2147483647\r\n"),
          "the widest ranges are written whole");
}

/* The text a report's line has after the component, "pointer " and
   what follows, for each of the button codes the cases use. */
static void check_pointer_lines(void)
{
    static const struct {
        UINT16 code;
        const char *name;
    } names[] = {{BTN_LEFT, "left"},       {BTN_RIGHT, "right"},
                 {0x112, "middle"},        {0x113, "side"},
                 {0x114, "extra"},         {0x115, "forward"},
                 {0x116, "back"},          {BTN_TASK, "task"},
                 {BTN_AFTER_TASK, "280"},  {BTN_MISC, "256"},
                 {0x10f, "271"},           {0xff, "key"},
                 {KEY_A, "key"}};
    ENDPOINT_NOTES notes;
    INPUT_POINTER r;
    char want[DEBUG_WRITE_MAX_BYTES];
    unsigned i, wrong = 0;

    for (i = 0; i < sizeof names / sizeof names[0]; i++) {
        notes.noted = 0;
        line_count = 0;
        r = report_new(0, 0, 0, 0);
        report_button(&r, names[i].code, INPUT_KEY_PRESSED);
        report_button(&r, names[i].code, INPUT_KEY_RELEASED);
        endpoint_pointer(&notes, "nova-console", &r, collect);
        snprintf(want, sizeof want, "nova-console: pointer press button=%s\r\n",
                 names[i].name);
        wrong += line_count != 2 || strcmp(lines[0], want);
        snprintf(want, sizeof want,
                 "nova-console: pointer release button=%s\r\n", names[i].name);
        wrong += line_count != 2 || strcmp(lines[1], want);
    }
    check(wrong == 0, "a button is named by evdev's name from BTN_LEFT to "
                      "BTN_TASK, by number from BTN_MISC, else key");

    notes.noted = 0;
    line_count = 0;
    r = report_new(AXES_BOTH, 0, 0, 0);
    endpoint_pointer(&notes, "nova-console", &r, collect);
    r = report_new(INPUT_AXIS_X, 0, 0, 0);
    endpoint_pointer(&notes, "nova-console", &r, collect);
    notes.noted = 0;
    endpoint_pointer(&notes, "nova-console", &r, collect);
    r = report_new(INPUT_AXIS_Y, 0, 0, 0);
    notes.noted = 0;
    endpoint_pointer(&notes, "nova-console", &r, collect);
    check(line_count == 3 &&
              !strcmp(lines[0], "nova-console: pointer move mode=absolute "
                                "axes=xy\r\n") &&
              !strcmp(lines[1], "nova-console: pointer move mode=absolute "
                                "axes=x\r\n") &&
              !strcmp(lines[2], "nova-console: pointer move mode=absolute "
                                "axes=y\r\n"),
          "an absolute move names the axes it sets");

    notes.noted = 0;
    line_count = 0;
    r = report_new(0, 3, 4, 0);
    endpoint_pointer(&notes, "nova-compositor", &r, collect);
    notes.noted = 0;
    r = report_new(0, -3, 0, 0);
    endpoint_pointer(&notes, "nova-compositor", &r, collect);
    notes.noted = 0;
    r = report_new(0, 0, 4, 0);
    endpoint_pointer(&notes, "nova-compositor", &r, collect);
    check(line_count == 3 &&
              !strcmp(lines[0], "nova-compositor: pointer move "
                                "mode=relative axes=xy\r\n") &&
              !strcmp(lines[1], "nova-compositor: pointer move "
                                "mode=relative axes=x\r\n") &&
              !strcmp(lines[2], "nova-compositor: pointer move "
                                "mode=relative axes=y\r\n"),
          "a relative move names the axes it moves");

    notes.noted = 0;
    line_count = 0;
    r = report_new(0, 0, 0, 2);
    endpoint_pointer(&notes, "nova-console", &r, collect);
    r = report_new(0, 0, 0, -1);
    endpoint_pointer(&notes, "nova-console", &r, collect);
    check(line_count == 2 &&
              !strcmp(lines[0],
                      "nova-console: pointer wheel direction=up\r\n") &&
              !strcmp(lines[1],
                      "nova-console: pointer wheel direction=down\r\n"),
          "a positive wheel is a turn up, a negative one down");

    notes.noted = 0;
    line_count = 0;
    r = report_new(0, 0, 0, 0);
    endpoint_pointer(&notes, "nova-console", &r, collect);
    check(line_count == 0 && notes.noted == 0,
          "a report that does nothing makes no line");
}

/* ------------------------------------------------------------------------
   A run is one line */

/* A generator of numbers that is the same on every run. */
static UINT32 state = 0x2545F491u;

static UINT32 next(UINT32 below)
{
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state % below;
}

#define STREAMS 3000
#define STREAM_REPORTS 12
#define BUTTON_CODES 6
#define REPEAT_ONE_IN 3
#define MODEL_TEXT_MAX 64

/* A report that changes one thing: a motion of one kind, a button or a
   turn of the wheel, so that a repeat of it is a run. */
static INPUT_POINTER simple_report(void)
{
    static const UINT16 buttons[] = {BTN_LEFT, BTN_RIGHT, KEY_A};
    INPUT_POINTER r = report_new(0, 0, 0, 0);

    switch (next(5)) {
    case 0:
        return report_new(next(2) ? INPUT_AXIS_X : AXES_BOTH, 0, 0, 0);
    case 1:
        return report_new(0, 1 + (INT32)next(3), 0, 0);
    case 2:
        return report_new(0, 0, 0, next(2) ? 1 : -1);
    default:
        report_button(&r, buttons[next(3)],
                      (UINT16)(next(2) ? INPUT_KEY_PRESSED
                                       : INPUT_KEY_RELEASED));
        return r;
    }
}

/* The text after "pointer " that a report's changes make, one for each,
   in the order C6 lists them, built apart from the library. Returns how
   many. */
static unsigned model_lines(const INPUT_POINTER *r,
                            char out[][MODEL_TEXT_MAX])
{
    unsigned n = 0, i;

    if (r->absolute)
        snprintf(out[n++], MODEL_TEXT_MAX, "move mode=absolute axes=%s",
                 r->absolute == AXES_BOTH ? "xy"
                 : r->absolute == INPUT_AXIS_X ? "x" : "y");
    if (r->dx || r->dy)
        snprintf(out[n++], MODEL_TEXT_MAX, "move mode=relative axes=%s",
                 r->dx && r->dy ? "xy" : r->dx ? "x" : "y");
    for (i = 0; i < r->buttons; i++) {
        UINT32 code = r->button[i].code;
        char name[16];

        if (code >= BTN_LEFT && code <= BTN_TASK) {
            static const char *const by_offset[] = {
                "left", "right", "middle", "side",
                "extra", "forward", "back", "task"};

            snprintf(name, sizeof name, "%s", by_offset[code - BTN_LEFT]);
        } else if (code >= BTN_MISC) {
            snprintf(name, sizeof name, "%u", (unsigned)code);
        } else {
            snprintf(name, sizeof name, "key");
        }
        snprintf(out[n++], MODEL_TEXT_MAX, "%s button=%s",
                 r->button[i].value == INPUT_KEY_PRESSED ? "press" : "release",
                 name);
    }
    if (r->wheel)
        snprintf(out[n++], MODEL_TEXT_MAX, "wheel direction=%s",
                 r->wheel > 0 ? "up" : "down");
    return n;
}

/* Whether the line `now` is left out after `last`, which C6 states in
   the lines' own words: a line the same as the last, or a move of the
   mode the last line moved in, whatever its axes. */
static int model_leaves_out(const char *last, const char *now)
{
    static const char *const modes[] = {"move mode=absolute",
                                        "move mode=relative"};
    unsigned m;

    if (!strcmp(last, now))
        return 1;
    for (m = 0; m < 2; m++)
        if (!strncmp(last, modes[m], strlen(modes[m])) &&
            !strncmp(now, modes[m], strlen(modes[m])))
            return 1;
    return 0;
}

/* Random streams of valid reports make the lines the words of C6 give,
   with a run of one kind of motion or one turn or button change left out. */
static void check_runs(void)
{
    static const UINT16 codes[BUTTON_CODES] = {BTN_LEFT,  BTN_RIGHT, BTN_TASK,
                                               BTN_MISC,  BTN_AFTER_TASK,
                                               KEY_A};
    static const UINT32 axes[] = {0, INPUT_AXIS_X, INPUT_AXIS_Y, AXES_BOTH};
    static const INT32 motion[] = {0, 3, -3};
    static const INT32 wheels[] = {0, 2, -2};
    unsigned s, i, j, wrong = 0, invalid = 0, shortened = 0, total = 0;

    for (s = 0; s < STREAMS; s++) {
        ENDPOINT_NOTES notes = {0};
        char last[MODEL_TEXT_MAX] = "";
        char expected[STREAM_REPORTS * (INPUT_REPORT_BUTTONS + 3)]
                     [MODEL_TEXT_MAX];
        unsigned expected_count = 0, offered = 0;

        line_count = 0;
        INPUT_POINTER r = report_new(0, 0, 0, 0);

        for (i = 0; i < STREAM_REPORTS; i++) {
            char candidates[INPUT_REPORT_BUTTONS + 3][MODEL_TEXT_MAX];
            unsigned count;

            /* One report in REPEAT_ONE_IN is the one before, so that
               runs are as common as changes. */
            if (i == 0 || next(REPEAT_ONE_IN) != 0) {
                if (next(2)) {
                    r = report_new(axes[next(4)], motion[next(3)],
                                   motion[next(3)], wheels[next(3)]);
                    for (j = next(4); j > 0; j--)
                        report_button(&r, codes[next(BUTTON_CODES)],
                                      (UINT16)(next(2) ? INPUT_KEY_PRESSED
                                                       : INPUT_KEY_RELEASED));
                } else {
                    r = simple_report();
                }
            }
            invalid += !endpoint_is_report(&r, sizeof r, AXES_BOTH);
            endpoint_pointer(&notes, "c", &r, collect);
            count = model_lines(&r, candidates);
            offered += count;
            for (j = 0; j < count; j++) {
                if (model_leaves_out(last, candidates[j]))
                    continue;
                strcpy(expected[expected_count++], candidates[j]);
                strcpy(last, candidates[j]);
            }
        }
        total += expected_count;
        shortened += offered - expected_count;
        wrong += line_count != expected_count;
        for (j = 0; j < line_count && j < expected_count; j++) {
            char want[DEBUG_WRITE_MAX_BYTES];

            snprintf(want, sizeof want, "c: pointer %s\r\n", expected[j]);
            wrong += strcmp(lines[j], want) != 0;
        }
    }
    check(invalid == 0, "the generated reports are reports");
    check(wrong == 0, "the lines of a stream are C6's, with a run of one "
                      "kind of motion, or the same line again, left out");
    check(shortened > total / 10 && total > STREAMS,
          "the streams hold runs and distinct lines alike");
}

/* A run of N reports of one motion, or of turns one way, is one line,
   and the same N alternating between two kinds is N lines. */
static void check_run_laws(void)
{
    unsigned n;
    unsigned wrong = 0;

    for (n = 1; n <= 20; n++) {
        ENDPOINT_NOTES notes = {0};
        unsigned i;

        line_count = 0;
        for (i = 0; i < n; i++) {
            INPUT_POINTER r = report_new(AXES_BOTH, 0, 0, 0);

            r.x = (INT32)i;
            endpoint_pointer(&notes, "c", &r, collect);
        }
        wrong += line_count != 1;

        notes.noted = 0;
        line_count = 0;
        for (i = 0; i < n; i++) {
            INPUT_POINTER r = report_new(0, 0, 0, 1);

            endpoint_pointer(&notes, "c", &r, collect);
        }
        wrong += line_count != 1;

        notes.noted = 0;
        line_count = 0;
        for (i = 0; i < n; i++) {
            INPUT_POINTER r = i % 2 ? report_new(0, 1, 1, 0)
                                    : report_new(AXES_BOTH, 0, 0, 0);

            endpoint_pointer(&notes, "c", &r, collect);
        }
        wrong += line_count != n;

        notes.noted = 0;
        line_count = 0;
        for (i = 0; i < n; i++) {
            INPUT_POINTER r = report_new(0, 0, 0, i % 2 ? 1 : -1);

            endpoint_pointer(&notes, "c", &r, collect);
        }
        wrong += line_count != n;
    }
    check(wrong == 0, "a run of one motion or turn is one line, and "
                      "alternating kinds make a line each");

    /* A run names the axes of its first report. */
    {
        ENDPOINT_NOTES notes = {0};
        INPUT_POINTER first = report_new(INPUT_AXIS_X, 0, 0, 0);
        INPUT_POINTER second = report_new(AXES_BOTH, 0, 0, 0);

        line_count = 0;
        endpoint_pointer(&notes, "c", &first, collect);
        endpoint_pointer(&notes, "c", &second, collect);
        check(line_count == 1 &&
                  !strcmp(lines[0], "c: pointer move mode=absolute axes=x\r\n"),
              "a run names the axes of its first report");
    }
}

/* ------------------------------------------------------------------------
   A line is one write */

/* Every line is NUL-terminated within what the log takes, ends in one
   CRLF and holds no other line end, however long the component. */
static void check_line_bounds(void)
{
    char component[DEBUG_WRITE_MAX_BYTES * 2];
    INPUT_DEVICE widest = device(INPUT_DEVICE_POINTER, AXES_BOTH, INT32_MIN,
                                 INT32_MAX, INT32_MIN, INT32_MAX);
    INPUT_POINTER r = report_new(AXES_BOTH, 1, 1, 1);
    ENDPOINT_NOTES notes = {0};
    unsigned i, length, wrong = 0;

    for (length = 0; length < sizeof component - 1; length += 37) {
        memset(component, 'a', length);
        component[length] = 0;
        line_count = 0;
        notes.noted = 0;
        endpoint_describe(component, &widest, collect);
        for (i = 0; i < INPUT_REPORT_BUTTONS; i++)
            report_button(&r, BTN_LEFT + (UINT16)i, INPUT_KEY_PRESSED);
        endpoint_pointer(&notes, component, &r, collect);
        r.buttons = 0;
        for (i = 0; i < line_count; i++) {
            size_t n = strlen(lines[i]);

            wrong += n >= DEBUG_WRITE_MAX_BYTES || n < 2 ||
                     strcmp(lines[i] + n - 2, "\r\n") != 0 ||
                     strchr(lines[i], '\r') != lines[i] + n - 2 ||
                     strchr(lines[i], '\n') != lines[i] + n - 1;
        }
    }
    check(wrong == 0, "every line, however long, ends in one CRLF within "
                      "the log's write size");

    memset(component, 'a', 300);
    component[300] = 0;
    line_count = 0;
    endpoint_describe(component, &widest, collect);
    check(line_count == 1 &&
              strlen(lines[0]) == DEBUG_WRITE_MAX_BYTES - 1,
          "a line longer than the buffer is cut to fill it");
}

int main(void)
{
    check_descriptions();
    check_key_events();
    check_reports();
    check_description_lines();
    check_pointer_lines();
    check_runs();
    check_run_laws();
    check_line_bounds();
    printf("%u failures\n", failures);
    return failures != 0;
}
