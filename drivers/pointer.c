/* A pointer's events gathered into reports (pointer.h). */

#include "pointer.h"

#define BITS_PER_BYTE 8
#define BITMAP_CODES (POINTER_BITMAP_BYTES * BITS_PER_BYTE)

/* The bounds of a signed 32-bit integer, the type of evdev's values,
   within which a report's sums of motion are held rather than wrap. */
#define INT32_LARGEST 0x7fffffffLL
#define INT32_SMALLEST (-INT32_LARGEST - 1)

int pointer_has(const UINT8 *bitmap, UINT32 code)
{
    return code < BITMAP_CODES &&
           (bitmap[code / BITS_PER_BYTE] >> code % BITS_PER_BYTE & 1);
}

static void set_held(POINTER *p, UINT32 code, UINT32 value)
{
    UINT8 bit = (UINT8)(1u << code % BITS_PER_BYTE);

    if (value == INPUT_KEY_PRESSED)
        p->held[code / BITS_PER_BYTE] |= bit;
    else
        p->held[code / BITS_PER_BYTE] &= (UINT8)~bit;
}

static void clear(INPUT_POINTER *r)
{
    UINT32 i;

    r->transaction = 0;
    r->type = INPUT_TYPE_SYN;
    r->code = INPUT_SYN_REPORT;
    r->absolute = 0;
    r->buttons = 0;
    r->x = 0;
    r->y = 0;
    r->dx = 0;
    r->dy = 0;
    r->wheel = 0;
    r->reserved = 0;
    r->time = 0;
    for (i = 0; i < INPUT_REPORT_BUTTONS; i++) {
        r->button[i].code = 0;
        r->button[i].value = 0;
    }
}

void pointer_start(POINTER *p)
{
    UINT32 i;

    for (i = 0; i < POINTER_BITMAP_BYTES; i++)
        p->held[i] = 0;
    p->dropping = 0;
    clear(&p->report);
}

/* End the report so far, if it sets an axis, moves or changes a button:
   its button changes become what the client knows held, and it goes to
   *out. Returns whether it did. */
static int finish(POINTER *p, INPUT_POINTER *out)
{
    INPUT_POINTER *r = &p->report;
    UINT32 i;

    if (!r->absolute && !r->buttons && !r->dx && !r->dy && !r->wheel)
        return 0;
    for (i = 0; i < r->buttons; i++)
        set_held(p, r->button[i].code, r->button[i].value);
    *out = *r;
    clear(r);
    return 1;
}

/* sum + value, held within a signed 32-bit integer. */
static INT32 add(INT32 sum, INT32 value)
{
    INT64 total = (INT64)sum + value;

    return (INT32)(total > INT32_LARGEST    ? INT32_LARGEST
                   : total < INT32_SMALLEST ? INT32_SMALLEST
                                            : total);
}

/* A press or release of a declared button. It changes the button's
   state as the report so far leaves it, or is not taken; a button the
   report changes already, or a report that holds INPUT_REPORT_BUTTONS
   changes, ends the report first. */
static int button(POINTER *p, UINT16 code, UINT32 value, INPUT_POINTER *out)
{
    INPUT_POINTER *r = &p->report;
    UINT32 i, state = (UINT32)pointer_has(p->held, code);
    int changed = 0, ended = 0;

    for (i = 0; i < r->buttons; i++)
        if (r->button[i].code == code) {
            state = r->button[i].value;
            changed = 1;
        }
    if (state == value)
        return 0;
    if (changed || r->buttons == INPUT_REPORT_BUTTONS)
        ended = finish(p, out);
    r->button[r->buttons].code = code;
    r->button[r->buttons].value = (UINT16)value;
    r->buttons++;
    return ended;
}

int pointer_take(POINTER *p, UINT16 type, UINT16 code, UINT32 value,
                 INPUT_POINTER *out)
{
    INPUT_POINTER *r = &p->report;

    if (type == INPUT_TYPE_SYN && code == POINTER_SYN_DROPPED) {
        clear(r);
        p->dropping = 1;
        return 0;
    }
    if (type == INPUT_TYPE_SYN && code == INPUT_SYN_REPORT) {
        if (!p->dropping)
            return finish(p, out);
        p->dropping = 0;
        return 0;
    }
    if (p->dropping)
        return 0;
    if (type == POINTER_EV_ABS && code == POINTER_ABS_X &&
        (p->absolute & INPUT_AXIS_X)) {
        r->x = (INT32)value;
        r->absolute |= INPUT_AXIS_X;
    } else if (type == POINTER_EV_ABS && code == POINTER_ABS_Y &&
               (p->absolute & INPUT_AXIS_Y)) {
        r->y = (INT32)value;
        r->absolute |= INPUT_AXIS_Y;
    } else if (type == POINTER_EV_REL && pointer_has(p->rel, code)) {
        if (code == POINTER_REL_X)
            r->dx = add(r->dx, (INT32)value);
        else if (code == POINTER_REL_Y)
            r->dy = add(r->dy, (INT32)value);
        else if (code == POINTER_REL_WHEEL)
            r->wheel = add(r->wheel, (INT32)value);
    } else if (type == INPUT_TYPE_KEY && pointer_has(p->keys, code) &&
               (value == INPUT_KEY_RELEASED || value == INPUT_KEY_PRESSED)) {
        return button(p, code, value, out);
    }
    return 0;
}
