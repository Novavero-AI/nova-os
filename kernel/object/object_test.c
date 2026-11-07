/* The boot-time self-test of kernel objects, run by the startup thread
   before the first process, so that every object it makes is gone
   before any process could see it. It checks that interrupt vectors are
   charged to a job's quota (docs/spec/objects.md O6), logs the result to
   the serial port, and halts with the reason at the first failure. */

#include "interrupt.h"
#include "process.h"
#include "cpu.h"
#include "../x86_64/serial.h"

/* The quota of the test's job, and the requester ID its interrupts are
   made for. No device is given their vectors, which are freed again
   before the first process starts. */
#define TEST_VECTORS 2
#define TEST_REQUESTER 0
#define TEST_SOURCE (INTERRUPT_SOURCE_MSI + TEST_REQUESTER)

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

void object_self_test(void)
{
    test_vector_quota();
    serial_write("nova: object self-test passed\r\n");
}
