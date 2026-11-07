/* Signals and waits. A waiting thread links one WAIT_ITEM per object into
   that object's list of watchers and blocks; a change of signals walks
   the list and wakes the waiters it satisfies, and the waiter unlinks its
   items when it runs again. Every step runs with interrupts disabled, so
   on one processor no signal can change between a wait's check of the
   signals and its blocking, and no wakeup is lost. */

#include "wait.h"
#include "thread.h"
#include "cpu.h"
#include "../x86_64/clock.h"

void waitable_init(WAITABLE *object)
{
    object->signals = 0;
    object->watchers = 0;
}

void waitable_update(WAITABLE *object, UINT32 clear, UINT32 set)
{
    UINT64 flags = save_and_disable_interrupts();
    WAIT_ITEM *item;

    object->signals = (object->signals & ~clear) | set;
    for (item = object->watchers; item; item = item->next) {
        item->observed |= object->signals;
        if (object->signals & item->signals)
            sched_wake(item->waiter);
    }
    restore_interrupts(flags);
}

static int satisfied(const WAIT_ITEM *items, UINT32 count)
{
    UINT32 i;

    for (i = 0; i < count; i++)
        if (items[i].observed & items[i].signals)
            return 1;
    return 0;
}

static void watch(WAIT_ITEM *item, THREAD *waiter)
{
    WAITABLE *object = item->object;

    item->waiter = waiter;
    item->previous = 0;
    item->next = object->watchers;
    if (object->watchers)
        object->watchers->previous = item;
    object->watchers = item;
}

static void unwatch(WAIT_ITEM *item)
{
    if (item->previous)
        item->previous->next = item->next;
    else
        item->object->watchers = item->next;
    if (item->next)
        item->next->previous = item->previous;
}

STATUS wait_many(WAIT_ITEM *items, UINT32 count, UINT64 deadline)
{
    UINT64 flags;
    STATUS status;
    UINT32 i;

    if (count > WAIT_MANY_MAX)
        return ERR_OUT_OF_RANGE;
    flags = save_and_disable_interrupts();
    for (i = 0; i < count; i++)
        items[i].observed = items[i].object->signals;
    if (satisfied(items, count)) {
        status = OK;
    } else if (deadline <= clock_monotonic()) {
        status = ERR_TIMED_OUT;
    } else {
        for (i = 0; i < count; i++)
            watch(&items[i], thread_current());
        status = sched_block(deadline);
        for (i = 0; i < count; i++) {
            unwatch(&items[i]);
            items[i].observed |= items[i].object->signals;
        }
    }
    restore_interrupts(flags);
    return status;
}

STATUS wait_one(WAITABLE *object, UINT32 signals, UINT64 deadline,
                UINT32 *observed)
{
    WAIT_ITEM item;
    STATUS status;

    item.object = object;
    item.signals = signals;
    status = wait_many(&item, 1, deadline);
    *observed = item.observed;
    return status;
}

void sleep_until(UINT64 deadline)
{
    wait_many(0, 0, deadline);
}

void event_init(EVENT *event)
{
    waitable_init(&event->waitable);
}

STATUS event_signal(EVENT *event, UINT32 clear, UINT32 set)
{
    if ((clear | set) & ~USER_SIGNALS)
        return ERR_INVALID_ARGS;
    waitable_update(&event->waitable, clear, set);
    return OK;
}
