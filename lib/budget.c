/* What the compositor's frames cost (budget.h; docs/spec/display.md D19's
   phase 3 arrangement, rule 8): the change a frame is counted under, the
   tally of each kind, and the line a full tally writes. A tally keeps
   the count of its frames, the most pixels one composed, their times
   together and the longest, and is zeroed by the line that reports it,
   so each line is of the BUDGET_FRAMES frames since the one before. */

#include "budget.h"

/* A time is written in milliseconds to the tenth, rounded down
   (docs/spec/log.md T1): the nanoseconds in a millisecond, and the
   tenths in one. */
#define NS_PER_MS 1000000ULL
#define TENTHS_PER_MS 10

/* The names of the two kinds, as the line's change key gives them. */
#define CHANGE_POINTER "pointer"
#define CHANGE_PRESENT "present"

/* Add nanoseconds to the line as milliseconds with one decimal, rounded
   down, as 12.3. */
static void line_ms(LINE *l, UINT64 nanoseconds)
{
    UINT64 tenths = nanoseconds / (NS_PER_MS / TENTHS_PER_MS);

    line_number(l, tenths / TENTHS_PER_MS);
    line_add(l, ".");
    line_number(l, tenths % TENTHS_PER_MS);
}

UINT32 budget_change(UINT32 presented, UINT32 rectangle, UINT32 pointer)
{
    if (presented)
        return BUDGET_CHANGE_PRESENT;
    if (pointer && !rectangle)
        return BUDGET_CHANGE_POINTER;
    return BUDGET_CHANGE_NONE;
}

/* Write the line of the kind named name whose frames tally counted. */
static void write_line(const char *name, const BUDGET_TALLY *tally,
                       LINE_WRITE write)
{
    LINE l;

    l.length = 0;
    line_add(&l, "nova-compositor: budget change=");
    line_add(&l, name);
    line_add(&l, " frames=");
    line_number(&l, tally->frames);
    line_add(&l, " pixels=");
    line_number(&l, tally->pixels);
    line_add(&l, " total_ms=");
    line_ms(&l, tally->total_ns);
    line_add(&l, " max_ms=");
    line_ms(&l, tally->max_ns);
    line_write(&l, write);
}

void budget_frame(BUDGET *budget, UINT32 change, UINT64 pixels,
                  UINT64 nanoseconds, LINE_WRITE write)
{
    BUDGET_TALLY *tally;
    const char *name;

    if (change == BUDGET_CHANGE_POINTER) {
        tally = &budget->pointer;
        name = CHANGE_POINTER;
    } else if (change == BUDGET_CHANGE_PRESENT) {
        tally = &budget->present;
        name = CHANGE_PRESENT;
    } else {
        return;
    }
    tally->frames++;
    if (pixels > tally->pixels)
        tally->pixels = pixels;
    tally->total_ns += nanoseconds;
    if (nanoseconds > tally->max_ns)
        tally->max_ns = nanoseconds;
    if (tally->frames < BUDGET_FRAMES)
        return;
    write_line(name, tally, write);
    /* Fields, not a structure assignment: TinyCC turns that into a call
       to memmove, and no program here links a C library
       (docs/spec/interface.md P2). */
    tally->frames = 0;
    tally->pixels = 0;
    tally->total_ns = 0;
    tally->max_ns = 0;
}
