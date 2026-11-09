/* The boot-time self-test of threads, scheduling and waits. It runs in a
   thread of its own beside the spinner's and checks, in order: that busy
   threads which never block all run under preemption, and that their
   exits free their stacks; that sleepers wake no earlier than their
   deadlines and within the bound test_deadlines states; that a timed wait
   a signal ends early leaves the other deadlines in place; and that waits
   follow docs/spec/objects.md O9, a wait on several events waking for the
   one signalled. It runs on a test disk, and reports through
   selftest.h. */

#include "thread.h"
#include "cpu.h"
#include "memory_layout.h"
#include "../mm/page.h"
#include "../selftest.h"
#include "../x86_64/arch.h"
#include "../x86_64/clock.h"

/* Every thread the test starts ends well within this; one that does not
   has hung. */
#define JOIN_LIMIT_NS (5 * NS_PER_SECOND)

/* Busy threads that never block run together for BUSY_MS. Each must be
   switched to at least MIN_SWITCHES times, which only preemption can
   do: it never gives the processor up itself. */
#define BUSY_THREADS 4
#define BUSY_MS 200
#define MIN_SWITCHES 2

/* Each sleeper sleeps SLEEP_ROUNDS times on absolute deadlines its period
   apart. A deadline is noticed at the first timer tick at or after it,
   within the longest interval between ticks. The woken sleeper then waits
   behind the one thread that works meanwhile, the spinner, at most until
   the first tick after the spinner's slice ends, and behind the sleepers
   woken at the same tick, which sleep again long before another tick
   would pass. Its lateness is therefore at most a slice, two of the
   longest intervals between ticks the phase saw, and TICK_NS. */
#define SLEEPERS 4
#define SLEEP_ROUNDS 10
#define TICK_NS (NS_PER_SECOND / TIMER_HZ)

/* Events a wait-many watches, and a deadline for it that the test never
   lets pass. A wait that must time out is given TIMEOUT_MS; the test
   checks on a thread it waits for every POLL_MS. */
#define WAIT_EVENTS 3
#define WAIT_LIMIT_NS JOIN_LIMIT_NS
#define TIMEOUT_MS 5
#define POLL_MS 1

/* The early-wakeup test's waiters have deadlines EARLY_STEP_MS apart, so
   that the test ends three of them long before the first deadline: the
   step is long for a host that stalls the emulated processor between the
   waiters blocking and the signals. */
#define EARLY_WAITERS 4
#define EARLY_STEP_MS 500

/* Written to each freed guard page. */
#define GUARD_PROBE 0x6e6f7661ULL

typedef struct {
    THREAD thread;
    volatile UINT64 loops;
} BUSY;

typedef struct {
    THREAD thread;
    UINT64 start, period;
    UINT64 early, max_late;
} SLEEPER;

typedef struct {
    THREAD thread;
    EVENT event;
    UINT64 deadline;
    UINT64 ended;
    STATUS status;
} EARLY;

static EARLY early[EARLY_WAITERS];

static BUSY busy[BUSY_THREADS];
static volatile int busy_stop;

static const UINT64 sleep_periods_ms[SLEEPERS] = {3, 4, 5, 7};
static SLEEPER sleepers[SLEEPERS];

static EVENT events[WAIT_EVENTS];
static WAIT_ITEM waited[WAIT_EVENTS];
static STATUS waited_status;
static THREAD waiter;

/* ------------------------------------------------------------------------
   Helpers. */

_Noreturn static void fail(const char *reason)
{
    selftest_halt(SELFTEST_THREAD, reason);
}

static void create(THREAD *thread, THREAD_ENTRY entry, void *argument)
{
    if (thread_create(thread, entry, argument) != OK)
        fail("thread_create found no memory");
}

static void start(THREAD *thread)
{
    if (thread_start(thread) != OK)
        fail("thread_start refused a new thread");
}

static void join(THREAD *thread)
{
    UINT32 observed;

    if (wait_one(&thread->waitable, THREAD_TERMINATED,
                 clock_monotonic() + JOIN_LIMIT_NS, &observed) != OK)
        fail("a thread did not terminate");
}

static void sleep_ms(UINT64 ms)
{
    sleep_until(clock_monotonic() + ms * NS_PER_MS);
}

/* ------------------------------------------------------------------------
   Preemption, and exit. */

static void busy_main(void *argument)
{
    BUSY *b = argument;

    while (!busy_stop)
        b->loops++;
}

/* A freed guard page must be mapped again. A write through the direct
   map to one that is not faults here, and the page-fault handler halts
   with the guard's address in CR2. The page is free, so nothing else
   reads it. */
static void touch_guard(UINT64 run)
{
    *(volatile UINT64 *)direct_map(run) = GUARD_PROBE;
}

static void test_preemption(THREAD *spinner)
{
    UINT64 i, free_with_stacks, spinner_switches;

    busy_stop = 0;
    for (i = 0; i < BUSY_THREADS; i++) {
        busy[i].loops = 0;
        create(&busy[i].thread, busy_main, &busy[i]);
    }
    free_with_stacks = page_free_count();
    spinner_switches = spinner->switches;
    for (i = 0; i < BUSY_THREADS; i++)
        start(&busy[i].thread);
    sleep_ms(BUSY_MS);
    busy_stop = 1;
    for (i = 0; i < BUSY_THREADS; i++)
        join(&busy[i].thread);
    spinner_switches = spinner->switches - spinner_switches;

    for (i = 0; i < BUSY_THREADS; i++)
        if (busy[i].thread.switches < MIN_SWITCHES || busy[i].loops == 0)
            fail("a busy thread was not preempted and resumed");
    if (spinner_switches < MIN_SWITCHES)
        fail("the spinner stopped while busy threads ran");

    if (page_free_count() != free_with_stacks + BUSY_THREADS * THREAD_STACK_RUN)
        fail("exited threads did not free their stacks");
    for (i = 0; i < BUSY_THREADS; i++)
        touch_guard(busy[i].thread.stack);
}

/* ------------------------------------------------------------------------
   Deadlines. */

static void sleeper_main(void *argument)
{
    SLEEPER *s = argument;
    UINT64 round;

    for (round = 1; round <= SLEEP_ROUNDS; round++) {
        UINT64 deadline = s->start + round * s->period;
        UINT64 woke, late;

        sleep_until(deadline);
        woke = clock_monotonic();
        if (woke < deadline) {
            s->early++;
            continue;
        }
        late = woke - deadline;
        if (late > s->max_late)
            s->max_late = late;
    }
}

/* The sleepers are all created before the clock is read for their start,
   so that creating them takes none of their first sleep. */
static void test_deadlines(void)
{
    UINT64 i, now, longest_tick, limit, max_late = 0;

    for (i = 0; i < SLEEPERS; i++) {
        SLEEPER *s = &sleepers[i];

        s->period = sleep_periods_ms[i] * NS_PER_MS;
        s->early = 0;
        s->max_late = 0;
        create(&s->thread, sleeper_main, s);
    }
    sched_take_longest_tick();
    now = clock_monotonic();
    for (i = 0; i < SLEEPERS; i++) {
        sleepers[i].start = now;
        start(&sleepers[i].thread);
    }
    for (i = 0; i < SLEEPERS; i++)
        join(&sleepers[i].thread);
    longest_tick = sched_take_longest_tick();
    limit = TIME_SLICE_MS * NS_PER_MS + 2 * longest_tick + TICK_NS;

    for (i = 0; i < SLEEPERS; i++) {
        if (sleepers[i].early)
            fail("a sleeper woke before its deadline");
        if (sleepers[i].max_late > max_late)
            max_late = sleepers[i].max_late;
    }
    if (max_late > limit)
        fail("a sleeper woke later than the limit");
}

/* ------------------------------------------------------------------------
   Waits. */

static void waiter_main(void *argument)
{
    UINT32 i;

    (void)argument;
    for (i = 0; i < WAIT_EVENTS; i++) {
        waited[i].object = &events[i].waitable;
        waited[i].signals = USER_SIGNAL(i);
    }
    waited_status =
        wait_many(waited, WAIT_EVENTS, clock_monotonic() + WAIT_LIMIT_NS);
}

/* O9's rules on one event: a wait ends at once on a signal already
   active and leaves it active; deadline 0 polls; a deadline ends a wait
   no earlier than itself; and the counts and signals a call takes are
   checked. A wait that should end at once is given a deadline far off,
   so that one that blocks instead fails rather than hangs. */
static void test_wait_rules(void)
{
    WAITABLE *object = &events[0].waitable;
    UINT32 observed;
    UINT64 deadline;

    event_signal(&events[0], 0, USER_SIGNAL(0));
    if (wait_one(object, USER_SIGNAL(0), clock_monotonic() + JOIN_LIMIT_NS,
                 &observed) != OK ||
        observed != USER_SIGNAL(0))
        fail("a wait on an active signal did not end at once");
    if (object->signals != USER_SIGNAL(0))
        fail("a wait changed the event it observed");
    if (wait_one(object, USER_SIGNAL(1), 0, &observed) != ERR_TIMED_OUT ||
        observed != USER_SIGNAL(0))
        fail("a poll of an inactive signal did not time out");
    deadline = clock_monotonic() + TIMEOUT_MS * NS_PER_MS;
    if (wait_one(object, USER_SIGNAL(1), deadline, &observed) !=
            ERR_TIMED_OUT ||
        clock_monotonic() < deadline)
        fail("a wait did not time out at its deadline");
    if (event_signal(&events[0], 0, ~USER_SIGNALS) != ERR_INVALID_ARGS)
        fail("an event took a signal that is not a user signal");
    if (wait_many(waited, WAIT_MANY_MAX + 1, 0) != ERR_OUT_OF_RANGE)
        fail("wait_many took more than WAIT_MANY_MAX objects");
    event_signal(&events[0], USER_SIGNAL(0), 0);
}

static void early_main(void *argument)
{
    EARLY *early_waiter = argument;
    UINT32 observed;

    early_waiter->status =
        wait_one(&early_waiter->event.waitable, USER_SIGNAL(0),
                 early_waiter->deadline, &observed);
    early_waiter->ended = clock_monotonic();
}

/* Four threads wait on events of their own with deadlines in order, the
   list of deadlines the scheduler keeps. A signal ends the wait of the
   second, then of the last, then of the first, which takes each from the
   middle, the end and the head of the list; each ends with OK, and the
   third, left alone and the one entry the list then holds, times out at
   its own deadline. */
static void test_early_wakeups(void)
{
    static const UINT64 ended_early[] = {1, 3, 0};
    UINT64 i, limit, base = clock_monotonic();

    for (i = 0; i < EARLY_WAITERS; i++) {
        event_init(&early[i].event);
        early[i].deadline = base + (i + 1) * EARLY_STEP_MS * NS_PER_MS;
        create(&early[i].thread, early_main, &early[i]);
        start(&early[i].thread);
    }
    limit = clock_monotonic() + JOIN_LIMIT_NS;
    for (i = 0; i < EARLY_WAITERS; i++)
        while (early[i].thread.state != THREAD_BLOCKED) {
            if (clock_monotonic() > limit)
                fail("a timed waiter did not block");
            sleep_ms(POLL_MS);
        }
    for (i = 0; i < sizeof ended_early / sizeof ended_early[0]; i++)
        event_signal(&early[ended_early[i]].event, 0, USER_SIGNAL(0));
    for (i = 0; i < EARLY_WAITERS; i++)
        join(&early[i].thread);
    for (i = 0; i < sizeof ended_early / sizeof ended_early[0]; i++)
        if (early[ended_early[i]].status != OK ||
            early[ended_early[i]].ended >= early[ended_early[i]].deadline)
            fail("a timed wait a signal ended did not end with OK before "
                 "its deadline");
    if (early[2].status != ERR_TIMED_OUT || early[2].ended < early[2].deadline)
        fail("a timed wait left after others ended early did not time out "
             "at its deadline");
}

/* A thread waits on three events for signals 0, 1 and 2 of each in turn.
   A signal it did not ask for, on the first event, must leave it
   blocked; signal 1 on the second must end its wait, with the first
   event's other signal and the second's signal observed. A signal wakes
   a waiter inside event_signal, so a wrong wakeup shows in its state as
   soon as event_signal returns. */
static void test_wait_many(void)
{
    UINT64 i, limit;

    for (i = 0; i < WAIT_EVENTS; i++)
        event_init(&events[i]);
    test_wait_rules();
    create(&waiter, waiter_main, 0);
    start(&waiter);
    limit = clock_monotonic() + JOIN_LIMIT_NS;
    while (waiter.state != THREAD_BLOCKED) {
        if (clock_monotonic() > limit)
            fail("the wait-many did not block");
        sleep_ms(POLL_MS);
    }
    event_signal(&events[0], 0, USER_SIGNAL(WAIT_EVENTS));
    if (waiter.state != THREAD_BLOCKED)
        fail("a signal the wait-many did not ask for ended it");
    event_signal(&events[1], 0, USER_SIGNAL(1));
    join(&waiter);
    if (waited_status != OK)
        fail("the wait-many did not end on its signal");
    if (waited[0].observed != USER_SIGNAL(WAIT_EVENTS) ||
        waited[1].observed != USER_SIGNAL(1) || waited[2].observed != 0)
        fail("the wait-many observed the wrong signals");
}

void sched_self_test(void *spinner)
{
    test_preemption(spinner);
    test_deadlines();
    test_early_wakeups();
    test_wait_many();
    selftest_passed(SELFTEST_THREAD);
}
