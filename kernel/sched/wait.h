/* Signals and waits, the kernel side of docs/spec/objects.md O9. Every
   waitable object holds a WAITABLE: its 32-bit signal set and the waits
   watching it. A wait observes and never changes an object; it ends as
   soon as any signal it asks for is active, including at the call, or
   when its deadline passes. Waits are made by threads other than the boot
   thread, never from an interrupt handler. */

#ifndef NOVA_WAIT_H
#define NOVA_WAIT_H

#include "../status.h"

/* Bits 24 to 31 of a signal set, which only object_signal sets and
   clears; an event has no others (O9). */
#define USER_SIGNALS 0xff000000u
#define USER_SIGNAL_SHIFT 24
#define USER_SIGNAL(n) (1u << (USER_SIGNAL_SHIFT + (n)))

/* A deadline is a time on the monotonic clock (x86_64/clock.h);
   DEADLINE_INFINITE (native_abi.h) never passes, and 0 has always passed,
   so it polls (O9). WAIT_MANY_MAX is native_abi.h's too. */

typedef struct WAIT_ITEM WAIT_ITEM;

typedef struct {
    UINT32 signals;
    WAIT_ITEM *watchers;
} WAITABLE;

/* One object of a wait. The caller sets object and signals; the wait
   sets observed to every signal the object asserted while it watched,
   active at the end or not, so the signal that ended a wait is there even
   if another thread cleared it before the waiter ran again. The other
   fields are the wait's own while it watches. */
struct WAIT_ITEM {
    WAITABLE *object;
    UINT32 signals;
    UINT32 observed;
    struct THREAD *waiter;
    WAIT_ITEM *next;
    WAIT_ITEM *previous;
};

/* The kernel side of an event: eight user signals and nothing else. */
typedef struct {
    WAITABLE waitable;
} EVENT;

/* No signals and no watchers. An object is initialized before any wait
   on it and not again until every wait on it has returned. */
void waitable_init(WAITABLE *object);

/* Clear, then set, signals of object, and end the wait of every watcher
   that a signal now active satisfies. */
void waitable_update(WAITABLE *object, UINT32 clear, UINT32 set);

/* Wait until a signal of interest is active on any of count objects or
   the deadline passes. Returns OK, ERR_TIMED_OUT, or ERR_OUT_OF_RANGE
   with nothing done if count is above WAIT_MANY_MAX. With no objects it
   sleeps until the deadline. */
STATUS wait_many(WAIT_ITEM *items, UINT32 count, UINT64 deadline);

/* wait_many on one object, reporting what it observed. */
STATUS wait_one(WAITABLE *object, UINT32 signals, UINT64 deadline,
                UINT32 *observed);

void sleep_until(UINT64 deadline);

/* As waitable_init. */
void event_init(EVENT *event);

/* Returns ERR_INVALID_ARGS, changing nothing, if either mask holds a bit
   that is not a user signal. */
STATUS event_signal(EVENT *event, UINT32 clear, UINT32 set);

#endif
