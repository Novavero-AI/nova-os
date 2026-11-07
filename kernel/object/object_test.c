/* The boot-time self-test of kernel objects, run by the startup thread
   before the first process, so that every object it makes is gone
   before any process could see it. It checks that interrupt vectors are
   charged to a job's quota (docs/spec/objects.md O6), that closing a
   handle ends the waits made through it (O9), that a device page takes
   one memory type (docs/spec/ipc.md I6), and that process_unmap removes
   whole mappings only (docs/spec/syscalls.md S8); it logs each result
   to the serial port and halts with the reason at the first failure. */

#include "boot_info.h"
#include "channel.h"
#include "interrupt.h"
#include "process.h"
#include "cpu.h"
#include "../acpi/acpi.h"
#include "../mm/page.h"
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

/* The unmap test maps a section of TEST_PAGES pages into a process of its
   own, which never runs: its first TEST_FIRST_PAGES at TEST_ADDRESS and
   the rest just above them, two mappings side by side, in a range no
   other mapping's tables reach. */
#define TEST_ADDRESS 0x40000000ULL
#define TEST_PAGES 3
#define TEST_FIRST_PAGES 2

/* A section's references in the unmap test: its creator's handle, and
   one for each mapping. */
#define TEST_UNMAPPED_REFERENCES 1
#define TEST_MAPPED_REFERENCES (TEST_UNMAPPED_REFERENCES + 2)

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

/* The framebuffer's page, which the direct map maps write-combining for
   the kernel's drawing, takes only a write-combining section; a page of
   PCI configuration space, which the direct map leaves out, takes the
   type of the first section over it and refuses the other while that
   section lives, and takes either once none does. No section made here
   is mapped. A framebuffer in memory the map describes, which no section
   may cover, or a machine without configuration space skips its part. */
static void test_memory_types(const BOOT_INFO *info)
{
    RESOURCE *mmio = resource_create_root(RESOURCE_MMIO, 0,
                                          1ULL << paging_physical_bits());
    UINT64 screen = (UINT64)info->screen.pixels - DIRECT_MAP_BASE;
    SECTION *uncached, *combining;
    STATUS status;

    check(mmio != 0, "no memory for the memory-type test");
    screen -= screen % PAGE_SIZE;
    status = section_create_device(mmio, screen, PAGE_SIZE,
                                   CACHE_WRITE_COMBINING, &combining);
    if (status == ERR_ACCESS_DENIED) {
        serial_write("nova: objects: the framebuffer lies in memory the map "
                     "describes; its memory-type check is skipped\r\n");
    } else {
        check(status == OK,
              "a write-combining section over the framebuffer was refused");
        object_close(&combining->object);
        check(section_create_device(mmio, screen, PAGE_SIZE, CACHE_UNCACHED,
                                    &uncached) == ERR_BAD_STATE,
              "an uncached section over the framebuffer, which the kernel "
              "draws write-combining, was not refused");
    }
    if (acpi_ecam_count() == 0) {
        serial_write("nova: objects: no pci configuration space; its "
                     "memory-type check is skipped\r\n");
    } else {
        UINT64 config = acpi_ecam(0)->base;

        check(section_create_device(mmio, config, PAGE_SIZE, CACHE_UNCACHED,
                                    &uncached) == OK,
              "an uncached section over configuration space was refused");
        check(section_create_device(mmio, config, PAGE_SIZE,
                                    CACHE_WRITE_COMBINING,
                                    &combining) == ERR_BAD_STATE,
              "a write-combining section over a page an uncached section "
              "covers was not refused");
        object_close(&uncached->object);
        check(section_create_device(mmio, config, PAGE_SIZE,
                                    CACHE_WRITE_COMBINING, &combining) == OK,
              "a page kept its memory type after its last section went");
        object_close(&combining->object);
    }
    object_close(&mmio->object);
    serial_write("nova: objects: device pages took one memory type: the "
                 "framebuffer only write-combining, configuration space "
                 "only its live section's type\r\n");
}

static int test_pages_mapped(const PROCESS *process, UINT32 pages)
{
    UINT32 i;

    for (i = 0; i < pages; i++)
        if (!paging_user_mapped(process->space, TEST_ADDRESS + i * PAGE_SIZE))
            return 0;
    return 1;
}

static int test_pages_unmapped(const PROCESS *process, UINT32 pages)
{
    UINT32 i;

    for (i = 0; i < pages; i++)
        if (paging_user_mapped(process->space, TEST_ADDRESS + i * PAGE_SIZE))
            return 0;
    return 1;
}

/* Part of a mapping, a range reaching past one and a range with a page
   no mapping holds are each refused, removing nothing; then one call
   removes both mappings, each releases its section, every page the
   tables of the range took is free again, and under isolation the user
   table no longer names them. Nothing else allocates pages while the
   test runs, before the first process. */
static void test_unmap(void)
{
    JOB *job = job_create_root();
    PROCESS *process = 0;
    SECTION *section = 0;
    UINT64 free_before;

    check(job && process_create(job, 0, &process) == OK &&
              section_create(TEST_PAGES * PAGE_SIZE, &section) == OK,
          "no memory for the unmap test");
    free_before = page_free_count();
    check(process_map(process, section, 0, TEST_ADDRESS,
                      TEST_FIRST_PAGES * PAGE_SIZE, MAP_READ) == OK &&
              process_map(process, section, TEST_FIRST_PAGES * PAGE_SIZE,
                          TEST_ADDRESS + TEST_FIRST_PAGES * PAGE_SIZE,
                          (TEST_PAGES - TEST_FIRST_PAGES) * PAGE_SIZE,
                          MAP_READ | MAP_WRITE) == OK &&
              paging_tables_agree(process->space),
          "the unmap test's mappings were refused");
    check(process_unmap(process, TEST_ADDRESS, PAGE_SIZE) == ERR_BAD_STATE,
          "process_unmap of part of a mapping was not refused");
    check(process_unmap(process, TEST_ADDRESS + PAGE_SIZE,
                        TEST_FIRST_PAGES * PAGE_SIZE) == ERR_BAD_STATE,
          "process_unmap of a range reaching past a mapping was not "
          "refused");
    check(process_unmap(process, TEST_ADDRESS,
                        (TEST_PAGES + 1) * PAGE_SIZE) == ERR_BAD_STATE,
          "process_unmap of a range with an unmapped page was not refused");
    check(process_unmap(process, TEST_ADDRESS + 1, PAGE_SIZE) ==
              ERR_INVALID_ARGS,
          "process_unmap of an unaligned range was not refused");
    check(test_pages_mapped(process, TEST_PAGES) &&
              section->object.references == TEST_MAPPED_REFERENCES,
          "a refused process_unmap removed a page");
    check(process_unmap(process, TEST_ADDRESS, TEST_PAGES * PAGE_SIZE) == OK,
          "process_unmap of two whole mappings was refused");
    check(test_pages_unmapped(process, TEST_PAGES) &&
              process->mapping_count == 0,
          "process_unmap left a page mapped");
    check(section->object.references == TEST_UNMAPPED_REFERENCES,
          "an unmapped mapping kept its section");
    check(page_free_count() == free_before,
          "process_unmap left a table of the range allocated");
    check(paging_tables_agree(process->space),
          "process_unmap left the user table naming a freed table");
    object_close(&section->object);
    object_close(&process->object);
    object_close(&job->object);
    serial_write("nova: objects: process_unmap refused part of a mapping, "
                 "a range past one and a range with a hole, then removed "
                 "two whole mappings, released their section and freed "
                 "their tables\r\n");
}

void object_self_test(void *boot_info)
{
    test_vector_quota();
    test_canceled_waits();
    test_memory_types(boot_info);
    test_unmap();
    serial_write("nova: object self-test passed\r\n");
}
