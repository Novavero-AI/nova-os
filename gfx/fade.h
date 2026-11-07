/* The schedule of the fade into the desktop background (background.c):
   FADE_STEPS steps over FADE_MS, eased out, and when each frame starts
   and which step it shows. Times are nanoseconds from the fade's start.
   It is arithmetic alone, so that tools/check-fade.c runs fades through
   it on the host. */

#ifndef NOVA_FADE_H
#define NOVA_FADE_H

#include "types.h"
#include "x86_64/clock.h"

/* A step per refresh of a 60 Hz display, which shows no more than that,
   over four tenths of a second. */
#define FADE_STEPS 24
#define FADE_MS 400
#define FADE_NS (FADE_MS * NS_PER_MS)

/* Weights toward Limb are out of FADE_WEIGHT, so that fade_weight is
   exact. */
#define FADE_WEIGHT (FADE_STEPS * FADE_STEPS * FADE_STEPS)

/* Step's weight, 1 - (1 - step / FADE_STEPS)^3 exactly: an ease-out,
   quick at first and slowing into Limb. */
static inline INT64 fade_weight(INT64 step)
{
    INT64 rest = FADE_STEPS - step;

    return FADE_WEIGHT - rest * rest * rest;
}

/* When step comes due: step / FADE_STEPS of FADE_NS, rounded up, so
   that a frame done then shows step, and one done any earlier the step
   before. */
static inline UINT64 fade_due(INT64 step)
{
    return ((UINT64)step * FADE_NS + FADE_STEPS - 1) / FADE_STEPS;
}

/* The step a frame that starts at start and takes cost shows: the latest
   due when it is done, and the last once FADE_NS has passed. */
static inline INT64 fade_step(UINT64 start, UINT64 cost)
{
    UINT64 done = start + cost;

    return done < FADE_NS ? (INT64)(done * FADE_STEPS / FADE_NS) : FADE_STEPS;
}

/* A fade under way: the step the last frame showed, and the longest a
   frame has taken, by which the next is judged. */
typedef struct {
    INT64 step;
    UINT64 longest;
} FADE_SCHEDULE;

/* Starts a fade, with no step shown and no frame timed. */
static inline void fade_begin(FADE_SCHEDULE *schedule)
{
    schedule->step = 0;
    schedule->longest = 0;
}

/* When the next frame starts: so that, if it takes as long as the
   longest, it is done as the step after the last one shown comes due, or
   at once if that is too late. */
static inline UINT64 fade_next(const FADE_SCHEDULE *schedule)
{
    UINT64 due = fade_due(schedule->step + 1);

    return due > schedule->longest ? due - schedule->longest : 0;
}

/* The step a frame that starts at start shows, noted as the last shown. */
static inline INT64 fade_show(FADE_SCHEDULE *schedule, UINT64 start)
{
    schedule->step = fade_step(start, schedule->longest);
    return schedule->step;
}

/* Notes that a frame took length. */
static inline void fade_took(FADE_SCHEDULE *schedule, UINT64 length)
{
    if (length > schedule->longest)
        schedule->longest = length;
}

#endif
