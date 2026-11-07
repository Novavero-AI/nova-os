/* The boot-time self-test of kernel objects, run by the startup thread
   before the first process, so that every object it makes is gone
   before any process could see it. It checks that interrupt vectors are
   charged to a job's quota (docs/spec/objects.md O6), and that closing a
   handle ends the waits made through it (O9); it logs each result to the
   serial port and halts with the reason at the first failure. */

#include "channel.h"
#include "interrupt.h"
#include "process.h"
#include "cpu.h"
#include "../x86_64/clock.h"
#include "../x86_64/serial.h"
#include "../x86_64/user.h"

/* The quota of the test's job, and the requester ID its interrupts are
   made for. No device is given their vectors, which are freed again
   before the first process starts. */
#define TEST_VECTORS 2
#define TEST_REQUESTER 0
#define TEST_SOURCE (INTERRUPT_SOURCE_MSI + TEST_REQUESTER)

/* A waiter of the cancel test is a kernel thread, whose tables map no
   user memory, waiting in port_wait for a packet that never comes: its
   packet address is checked and never written. A waiter ends well
   within JOIN_LIMIT_NS of its handle's close, and the test checks every
   POLL_MS that both have blocked. */
#define TEST_PACKET USER_BASE
#define JOIN_LIMIT_NS (5 * NS_PER_SECOND)
#define POLL_MS 1

/* A thread waiting through a handle in the cancel test's table, and how
   its wait ended. */
typedef struct {
    THREAD thread;
    HANDLE_TABLE *table;
    WAIT_ENTRY entry;
    STATUS status;
} WAITER;

static WAITER port_waiter, channel_waiter;

static void check(int passed, const char *reason)
{
    if (passed)
        return;
    serial_write("nova: object self-test failed: ");
    serial_write(reason);
    serial_write(", halted\r\n");
    halt_forever();
}

static STATUS make_interrupt(const RESOURCE *irq, JOB *job,
                             INTERRUPT **interrupt)
{
    UINT64 address;
    UINT32 data;

    return interrupt_create(irq, TEST_SOURCE, 0, job, interrupt, &address,
                            &data);
}

/* A job granted TEST_VECTORS vectors gets that many interrupts and
   ERR_NO_RESOURCES for one more; a vector freed returns to it; a child
   job is refused more than its parent has left; and a job's quota
   returns to its parent once the job is destroyed. The test's own root
   job stands in for the first process's. */
static void test_vector_quota(void)
{
    RESOURCE *irq = resource_create_root(RESOURCE_IRQ, 0,
                                         INTERRUPT_SOURCE_LIMIT);
    JOB *root = job_create_root();
    INTERRUPT *interrupts[TEST_VECTORS], *extra;
    JOB_LIMITS limits;
    JOB *job, *greedy;
    UINT32 i;

    check(irq && root, "no memory for the vector test");
    limits.interrupt_vectors = TEST_VECTORS;
    check(job_create(root, &limits, &job) == OK,
          "job_create refused a quota its parent had");
    for (i = 0; i < TEST_VECTORS; i++)
        check(make_interrupt(irq, job, &interrupts[i]) == OK,
              "an interrupt within its job's quota was refused");
    check(make_interrupt(irq, job, &extra) == ERR_NO_RESOURCES,
          "an interrupt past its job's quota did not fail with "
          "ERR_NO_RESOURCES");
    object_close(&interrupts[0]->object);
    check(make_interrupt(irq, job, &interrupts[0]) == OK,
          "a freed vector did not return to its job's quota");
    limits.interrupt_vectors = root->vector_limit - root->vectors_held + 1;
    check(job_create(root, &limits, &greedy) == ERR_NO_RESOURCES,
          "a child job was granted more vectors than its parent had left");
    for (i = 0; i < TEST_VECTORS; i++)
        object_close(&interrupts[i]->object);
    object_close(&job->object);
    check(root->vectors_held == 0,
          "a destroyed job's quota did not return to its parent");
    object_close(&root->object);
    object_close(&irq->object);
    serial_write("nova: objects: a job granted ");
    serial_number(TEST_VECTORS);
    serial_write(" vectors got ERR_NO_RESOURCES for one more, a freed "
                 "vector back, and its parent the quota once it was "
                 "destroyed\r\n");
}

static void port_waiter_main(void *argument)
{
    WAITER *waiter = argument;

    waiter->status = port_wait(waiter->table, waiter->entry.handle,
                               DEADLINE_INFINITE, TEST_PACKET);
}

static void channel_waiter_main(void *argument)
{
    WAITER *waiter = argument;

    waiter->status =
        handle_wait(waiter->table, &waiter->entry, 1, DEADLINE_INFINITE);
}

static void start_waiter(WAITER *waiter, THREAD_ENTRY entry,
                         HANDLE_TABLE *table, OBJECT *object, UINT32 rights,
                         UINT32 signals)
{
    UINT32 index;

    check(handle_reserve(table, &index) == OK, "a test table was full");
    waiter->table = table;
    waiter->entry.handle = handle_place(table, index, object, rights);
    waiter->entry.signals = signals;
    waiter->entry.observed = 0;
    waiter->status = OK;
    check(thread_create(&waiter->thread, entry, waiter) == OK,
          "no memory for a waiter");
    thread_start(&waiter->thread);
}

static void await_blocked(const WAITER *waiter)
{
    UINT64 limit = clock_monotonic() + JOIN_LIMIT_NS;

    while (waiter->thread.state != THREAD_BLOCKED) {
        check(clock_monotonic() < limit, "a waiter did not block");
        sleep_until(clock_monotonic() + POLL_MS * NS_PER_MS);
    }
}

static void join(WAITER *waiter)
{
    UINT32 observed;

    check(wait_one(&waiter->thread.waitable, THREAD_TERMINATED,
                   clock_monotonic() + JOIN_LIMIT_NS, &observed) == OK,
          "a wait through a closed handle went on");
}

/* One thread blocks in port_wait on a port, another in object_wait_one
   on a channel endpoint, each through a handle in a table of the test's
   own and with no deadline; closing each handle must end its wait with
   ERR_CANCELED, the channel's with HANDLE_CLOSED observed. */
static void test_canceled_waits(void)
{
    HANDLE_TABLE *table = handle_table_create();
    ENDPOINT *endpoint, *peer;
    PORT *port;

    check(table != 0, "no memory for the cancel test's table");
    check(port_create(1, &port) == OK, "no memory for the cancel test's port");
    check(channel_create(1, &endpoint, &peer) == OK,
          "no memory for the cancel test's channel");
    start_waiter(&port_waiter, port_waiter_main, table, &port->object,
                 PORT_RIGHTS, 0);
    start_waiter(&channel_waiter, channel_waiter_main, table,
                 &endpoint->object, CHANNEL_RIGHTS, CHANNEL_READABLE);
    await_blocked(&port_waiter);
    await_blocked(&channel_waiter);
    handle_close(table, port_waiter.entry.handle);
    handle_close(table, channel_waiter.entry.handle);
    join(&port_waiter);
    join(&channel_waiter);
    check(port_waiter.status == ERR_CANCELED,
          "port_wait did not end with ERR_CANCELED when its port handle "
          "closed");
    check(channel_waiter.status == ERR_CANCELED &&
              (channel_waiter.entry.observed & HANDLE_CLOSED),
          "object_wait_one did not end with ERR_CANCELED and "
          "HANDLE_CLOSED when its handle closed");
    object_close(&peer->object);
    handle_table_destroy(table);
    serial_write("nova: objects: threads blocked in port_wait and "
                 "object_wait_one with no deadline ended with ERR_CANCELED "
                 "when their handles closed\r\n");
}

void object_self_test(void)
{
    test_vector_quota();
    test_canceled_waits();
    serial_write("nova: object self-test passed\r\n");
}
