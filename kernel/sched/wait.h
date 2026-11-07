/* Signals and waits, the kernel side of docs/spec/objects.md O9. Every
   waitable object holds a WAITABLE: its 32-bit signal set and the waits
   watching it. A wait observes and never changes an object; it ends as
   soon as any signal it asks for is active, including at the call, when
   its deadline passes, or when a handle it waits through closes. Waits
   are made by threads other than the boot thread, never from an
   interrupt handler. */

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

/* One object of a wait. The caller sets object and signals, and the
   handle it waits through: the address of the handle table, which this
   layer only compares, and the handle's value; handle_table is 0 for a
   kernel wait through no handle. The wait sets observed to every signal
   the object asserted while it watched, active at the end or not, so the
   signal that ended a wait is there even if another thread cleared it
   before the waiter ran again, and HANDLE_CLOSED if waitable_cancel
   reached it. The other fields are the wait's own while it watches. */
struct WAIT_ITEM {
    WAITABLE *object;
    UINT32 signals;
    UINT32 observed;
    const void *handle_table;
    UINT32 handle;
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

/* End every wait on object made through the handle value handle in the
   table at handle_table, which is closing: each such item observes
   HANDLE_CLOSED, and its waiter, if still blocked, wakes with
   ERR_CANCELED (docs/spec/objects.md O9). */
void waitable_cancel(WAITABLE *object, const void *handle_table,
                     UINT32 handle);

/* Wait until a signal of interest is active on any of count objects or
   the deadline passes. Returns OK; ERR_TIMED_OUT; ERR_CANCELED if a
   handle waited through closed meanwhile; or ERR_OUT_OF_RANGE with
   nothing done if count is above WAIT_MANY_MAX. With no objects it
   sleeps until the deadline. A handle is checked by the caller, which
   keeps interrupts disabled from that check into this call, so that its
   close cannot fall between the two unseen. */
STATUS wait_many(WAIT_ITEM *items, UINT32 count, UINT64 deadline);

/* wait_many on one object, through no handle, reporting what it
   observed. */
STATUS wait_one(WAITABLE *object, UINT32 signals, UINT64 deadline,
                UINT32 *observed);

void sleep_until(UINT64 deadline);

/* As waitable_init. */
void event_init(EVENT *event);

/* Returns ERR_INVALID_ARGS, changing nothing, if either mask holds a bit
   that is not a user signal. */
STATUS event_signal(EVENT *event, UINT32 clear, UINT32 set);

#endif
