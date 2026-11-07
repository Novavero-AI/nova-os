/* Checks the fade's schedule (gfx/fade.h) on the host. Each step is
   shown from its due time on, and the weights rise from none of Limb to
   all of it by ever smaller steps. Fades are run through the schedule as
   background.c runs them, for every frame length from none to twice the
   fade's, in steps of COST_STEP_NS, each frame as long as the others, and
   with the timer's ticks at TICK_OFFSETS offsets from the fade's start;
   a sleep ends on the first tick at or after its deadline. In each, every
   frame shows a later step than the one before, and the fade ends no
   sooner than FADE_MS and no later than a frame and a tick after it, or
   after the first frame, which has no frame before it to judge its
   length by, if that ends later. While a frame and a tick fit in the
   shortest step, every step is shown and the fade ends within a tick of
   FADE_MS. Prints each failure and a count, and exits nonzero if any
   check fails.

   cc -std=c99 -Wall -Wextra -pedantic -I include -I kernel -I gfx \
     -o check-fade tools/check-fade.c && ./check-fade */

#include <stdio.h>

#include "fade.h"
#include "x86_64/arch.h"

#define TICK_NS (NS_PER_SECOND / TIMER_HZ)
#define TICK_OFFSETS 8
#define COST_STEP_NS (10 * NS_PER_US)
#define COST_MAX_NS (2 * FADE_NS)
#define FAILURES_SHOWN 10

static unsigned failures;

static void check(int passed, const char *what)
{
    if (!passed && failures++ < FAILURES_SHOWN)
        printf("failed: %s\n", what);
}

static void check_run(int passed, const char *what, UINT64 cost,
                      UINT64 offset)
{
    if (!passed && failures++ < FAILURES_SHOWN)
        printf("failed: %s, frames of %llu ns, ticks %llu ns in\n", what,
               cost, offset);
}

static UINT64 later(UINT64 a, UINT64 b)
{
    return a > b ? a : b;
}

/* When a sleep from now to deadline ends, with the ticks offset in from
   the fade's start. */
static UINT64 wake(UINT64 now, UINT64 deadline, UINT64 offset)
{
    if (deadline <= now)
        return now;
    if (deadline <= offset)
        return offset;
    return offset + (deadline - offset + TICK_NS - 1) / TICK_NS * TICK_NS;
}

static void check_steps(void)
{
    INT64 step;

    check(fade_due(FADE_STEPS) == FADE_NS, "the last step is due at FADE_NS");
    check(fade_weight(0) == 0 && fade_weight(FADE_STEPS) == FADE_WEIGHT,
          "the weights run from none to FADE_WEIGHT");
    for (step = 0; step <= FADE_STEPS; step++) {
        check(fade_step(fade_due(step), 0) == step,
              "a step is shown once due");
        if (step == 0)
            continue;
        check(fade_step(fade_due(step) - 1, 0) == step - 1,
              "a step is not shown before it is due");
        check(fade_weight(step) > fade_weight(step - 1),
              "each step's weight is above the one before");
        if (step > 1)
            check(fade_weight(step) - fade_weight(step - 1) <
                      fade_weight(step - 1) - fade_weight(step - 2),
                  "each step adds less weight than the one before");
    }
}

/* The shortest time from one step's due time to the next. */
static UINT64 shortest_step(void)
{
    UINT64 shortest = FADE_NS;
    INT64 step;

    for (step = 1; step <= FADE_STEPS; step++)
        if (fade_due(step) - fade_due(step - 1) < shortest)
            shortest = fade_due(step) - fade_due(step - 1);
    return shortest;
}

/* A fade whose every frame takes cost, as background.c's fade runs it,
   with the ticks offset in from its start. */
static void check_fade(UINT64 cost, UINT64 offset, UINT64 shortest)
{
    FADE_SCHEDULE schedule;
    UINT64 now = 0, first_end = 0;
    INT64 frames = 0;

    fade_begin(&schedule);
    while (schedule.step < FADE_STEPS) {
        INT64 before = schedule.step;

        now = wake(now, fade_next(&schedule), offset);
        if (fade_show(&schedule, now) <= before) {
            check_run(0, "each frame shows a later step", cost, offset);
            return;
        }
        now += cost;
        if (frames == 0)
            first_end = now;
        fade_took(&schedule, cost);
        frames++;
    }
    check_run(cost + TICK_NS > shortest ||
                  (frames == FADE_STEPS && now <= FADE_NS + TICK_NS),
              "while a frame and a tick fit in a step, every step is shown "
              "and the fade ends within a tick of FADE_NS",
              cost, offset);
    check_run(now >= FADE_NS, "the fade ends no sooner than FADE_NS", cost,
              offset);
    check_run(now <= later(FADE_NS, first_end) + cost + TICK_NS,
              "the fade ends within a frame and a tick of FADE_NS or of "
              "its first frame",
              cost, offset);
}

int main(void)
{
    UINT64 shortest = shortest_step(), cost, offset;

    check_steps();
    for (cost = 0; cost <= COST_MAX_NS; cost += COST_STEP_NS)
        for (offset = 0; offset < TICK_NS; offset += TICK_NS / TICK_OFFSETS)
            check_fade(cost, offset, shortest);
    printf("%u failures\n", failures);
    return failures != 0;
}
