/* The boot-time self-test of kernel objects, run by the startup thread
   before the first process, so that every object it makes is gone
   before any process could see it. It checks that interrupt vectors are
   charged to a job's quota (docs/spec/objects.md O6), that closing a
   handle ends the waits made through it (O9), that a watch queues its
   one packet and ends as ipc.md I4 has it, that a device page takes one
   memory type (docs/spec/ipc.md I6), that process_unmap removes whole
   mappings only (docs/spec/syscalls.md S8), and that killing a task ends
   what objects.md O7 and O10 say and nothing else: a job and the jobs
   under it, in any shape of tree, with processes that never ran and
   processes whose threads run in user mode, and one thread alone,
   started or not. It runs on a test disk, reports through selftest.h,
   and halts with the reason at the first failure. */

#include "boot_info.h"
#include "channel.h"
#include "interrupt.h"
#include "process.h"
#include "cpu.h"
#include "../acpi/acpi.h"
#include "../mm/page.h"
#include "../selftest.h"
#include "../x86_64/clock.h"
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

/* A code a process of the kill test chose before it was killed. */
#define TEST_EXIT_CODE 7

/* A running thread of the kill tests executes JMP to itself, the two
   bytes below mapped read-execute at TEST_ADDRESS, so it stays in user
   mode until it is stopped; its stack pointer, 8 more than a multiple
   of 16 as a thread starts with (S8), is never used. A thread ends well
   within JOIN_LIMIT_NS of a kill. */
#define TEST_STACK (TEST_ADDRESS + 8)
#define SPINNING_THREADS 3

/* The keys of the watch test's watches. */
#define KEY_LATER 1
#define KEY_AT_ONCE 2
#define KEY_HANDLE_CLOSED 3
#define KEY_PORT_CLOSED 4

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
    if (!passed)
        selftest_halt(SELFTEST_OBJECT, reason);
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
}

/* Place object's handle, which the caller holds, in table with rights,
   and return its value. */
static UINT32 place(HANDLE_TABLE *table, OBJECT *object, UINT32 rights)
{
    UINT32 index;

    check(handle_reserve(table, &index) == OK, "a test table was full");
    return handle_place(table, index, object, rights);
}

/* Read and discard the packet at the head of port, as port_wait does,
   which must be key's, from a watch that asked for PEER_CLOSED and saw
   it active. */
static void take_signal(PORT *port, UINT64 key)
{
    PACKET_NODE *node = port->first;

    check(node && node->packet.type == PACKET_SIGNAL &&
              node->packet.key == key &&
              (UINT32)node->packet.payload[0] == CHANNEL_PEER_CLOSED &&
              (node->packet.payload[0] >> PACKET_SIGNAL_ACTIVE_SHIFT &
               CHANNEL_PEER_CLOSED),
          "a watch's packet did not carry its key and signals");
    port_discard(port, node);
}

/* A watch through a handle in a table of the test's own queues one
   packet once a signal it asks for is active, or at once if one is
   already; it ends with no packet when its handle closes, and when its
   port's last handle closes; a port takes no more than PORT_WATCHES_MAX;
   and no watch leaves a page behind, its packet read or never queued. */
static void test_watches(void)
{
    UINT64 free_pages = page_free_count();
    HANDLE_TABLE *table = handle_table_create();
    ENDPOINT *watched, *peer, *other, *other_peer;
    PORT *port;
    UINT32 handle, other_handle, i;

    check(table != 0, "no memory for the watch test's table");
    check(port_create(1, &port) == OK, "no memory for the watch test's port");
    check(channel_create(1, &watched, &peer) == OK,
          "no memory for the watch test's channel");
    check(channel_create(1, &other, &other_peer) == OK,
          "no memory for the watch test's second channel");
    handle = place(table, &watched->object, CHANNEL_RIGHTS);
    other_handle = place(table, &other->object, CHANNEL_RIGHTS);
    check(port_watch(port, &watched->object, &watched->waitable, table,
                     handle, KEY_LATER, CHANNEL_PEER_CLOSED) == OK &&
              !port->first,
          "a watch queued a packet before its signal was active");
    object_close(&peer->object);
    take_signal(port, KEY_LATER);
    check(!port->first, "a watch queued more than one packet");
    check(port_watch(port, &watched->object, &watched->waitable, table,
                     handle, KEY_AT_ONCE, CHANNEL_PEER_CLOSED) == OK,
          "a watch was refused");
    take_signal(port, KEY_AT_ONCE);
    check(port_watch(port, &watched->object, &watched->waitable, table,
                     handle, KEY_HANDLE_CLOSED, CHANNEL_READABLE) == OK,
          "a watch was refused");
    handle_close(table, handle);
    check(!port->first, "a watch whose handle closed queued a packet");
    for (i = 0; i < PORT_WATCHES_MAX; i++)
        check(port_watch(port, &other->object, &other->waitable, table,
                         other_handle, KEY_PORT_CLOSED,
                         CHANNEL_PEER_CLOSED) == OK,
              "a watch within a port's bound was refused");
    check(port_watch(port, &other->object, &other->waitable, table,
                     other_handle, KEY_PORT_CLOSED,
                     CHANNEL_PEER_CLOSED) == ERR_NO_MEMORY,
          "a port took a watch past PORT_WATCHES_MAX");
    object_close(&port->object);
    object_close(&other_peer->object);
    handle_table_destroy(table);
    check(page_free_count() == free_pages,
          "a watch, or what it held, was not freed");
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
        selftest_note("the framebuffer lies in memory the map describes; "
                      "its memory-type check is skipped");
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
        selftest_note("no pci configuration space; its memory-type check "
                      "is skipped");
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
}

/* A process of the kill test, which never runs. */
static PROCESS *make_process(JOB *job)
{
    PROCESS *process = 0;

    check(process_create(job, 0, &process) == OK,
          "no memory for the kill test's process");
    return process;
}

static int ended_with(PROCESS *process, UINT32 exit_code)
{
    UINT32 state, code;

    process_info(process, &state, &code);
    return state == PROCESS_STATE_TERMINATED && code == exit_code;
}

static int untouched(const PROCESS *process)
{
    return process->state == PROCESS_STATE_NEW && !process->terminating;
}

static int interrupts_enabled(void)
{
    UINT64 flags = save_and_disable_interrupts();

    restore_interrupts(flags);
    return (flags & RFLAGS_INTERRUPT_ENABLE) != 0;
}

/* The jobs and processes of the kill test, none of which runs:

       root --- doomed --- nested         doomed is killed
         |
         +----- spared                    root is killed last

   doomed holds first, which maps a section and holds a channel endpoint
   whose peer a thread waits on, and exited, which ended with a code of
   its own before the kill; nested holds deep, which holds an interrupt;
   spared holds bystander, which holds an endpoint. Killing doomed ends
   first, exited's code stays, deep and its interrupt go and give the
   vector back, and the waiter wakes with PEER_CLOSED; bystander is
   untouched. A killed job takes no process and no child job, a job
   outside it still does, a kill finds nothing to do in a job whose
   processes are gone, and killing the root ends what is left. Every
   page the test took is free again at the end. */
static void test_kill(void)
{
    RESOURCE *irq = resource_create_root(RESOURCE_IRQ, 0,
                                         INTERRUPT_SOURCE_LIMIT);
    JOB *root = job_create_root();
    HANDLE_TABLE *table;
    JOB_LIMITS doomed_limits, nested_limits, none;
    JOB *doomed = 0, *nested = 0, *spared = 0, *refused = 0;
    PROCESS *first, *exited, *deep, *bystander, *late = 0;
    ENDPOINT *first_end = 0, *first_peer = 0, *bystander_end = 0;
    ENDPOINT *bystander_peer = 0;
    SECTION *section = 0;
    INTERRUPT *interrupt = 0;
    UINT64 free_before;

    check(irq && root, "no memory for the kill test");
    free_before = page_free_count();
    table = handle_table_create();
    check(table != 0, "no memory for the kill test's table");
    doomed_limits.interrupt_vectors = TEST_VECTORS;
    nested_limits.interrupt_vectors = 1;
    none.interrupt_vectors = 0;
    check(job_create(root, &doomed_limits, &doomed) == OK &&
              job_create(doomed, &nested_limits, &nested) == OK &&
              job_create(root, &none, &spared) == OK,
          "no memory for the kill test's jobs");
    first = make_process(doomed);
    exited = make_process(doomed);
    deep = make_process(nested);
    bystander = make_process(spared);
    check(channel_create(1, &first_end, &first_peer) == OK &&
              channel_create(1, &bystander_end, &bystander_peer) == OK &&
              section_create(PAGE_SIZE, &section) == OK &&
              make_interrupt(irq, nested, &interrupt) == OK,
          "no memory for the kill test's objects");
    place(first->handles, &first_end->object, CHANNEL_RIGHTS);
    place(bystander->handles, &bystander_end->object, CHANNEL_RIGHTS);
    place(deep->handles, &interrupt->object, INTERRUPT_RIGHTS);
    check(process_map(first, section, 0, TEST_ADDRESS, PAGE_SIZE,
                      MAP_READ) == OK &&
              section->object.references == TEST_UNMAPPED_REFERENCES + 1,
          "the kill test's mapping was refused");
    process_terminate(exited, TEST_EXIT_CODE);
    check(nested->vectors_held == 1 && doomed->vectors_held == 1,
          "the kill test's interrupt was not charged to its job");
    start_waiter(&channel_waiter, channel_waiter_main, table,
                 &first_peer->object, CHANNEL_RIGHTS, CHANNEL_PEER_CLOSED);
    await_blocked(&channel_waiter);

    job_kill(doomed);
    check(interrupts_enabled(), "job_kill left interrupts disabled");
    join(&channel_waiter);
    check(channel_waiter.status == OK &&
              (channel_waiter.entry.observed & CHANNEL_PEER_CLOSED),
          "a wait on the peer of a killed process's endpoint did not end "
          "with PEER_CLOSED");
    check(ended_with(first, PROCESS_EXIT_KILLED) &&
              ended_with(deep, PROCESS_EXIT_KILLED),
          "killing a job left a process in it or under it running, or "
          "gave it a code other than PROCESS_EXIT_KILLED");
    check(ended_with(exited, TEST_EXIT_CODE),
          "a kill changed the exit code of a process that had ended");
    check(untouched(bystander) && !(bystander_peer->waitable.signals &
                                    CHANNEL_PEER_CLOSED),
          "killing a job ended a process outside it");
    check(doomed->killed && nested->killed && !spared->killed &&
              !root->killed,
          "a job's killed mark did not cover it and its descendants "
          "alone");
    check(first->mapping_count == 0 &&
              !paging_user_mapped(first->space, TEST_ADDRESS) &&
              section->object.references == TEST_UNMAPPED_REFERENCES,
          "a killed process kept its mapping");
    check(nested->vectors_held == 0 && doomed->vectors_held == 1,
          "a killed process kept its interrupt's vector");
    check(process_create(doomed, 0, &late) == ERR_BAD_STATE &&
              process_create(nested, 0, &late) == ERR_BAD_STATE,
          "a killed job took a process");
    check(job_create(doomed, &none, &refused) == ERR_BAD_STATE &&
              job_create(nested, &none, &refused) == ERR_BAD_STATE,
          "a killed job took a child job");
    check(process_create(spared, 0, &late) == OK && untouched(late),
          "a job outside the kill refused a process");
    object_close(&late->object);

    object_close(&first->object);
    object_close(&exited->object);
    object_close(&deep->object);
    check(doomed->processes == 0 && nested->processes == 0,
          "a destroyed process stayed on its job's list");
    job_kill(doomed);
    check(interrupts_enabled() && doomed->killed && nested->killed,
          "killing a job whose processes had gone failed");

    job_kill(root);
    check(interrupts_enabled(), "job_kill left interrupts disabled");
    check(ended_with(bystander, PROCESS_EXIT_KILLED) && root->killed &&
              spared->killed &&
              (bystander_peer->waitable.signals & CHANNEL_PEER_CLOSED),
          "killing the root job left a process running");

    object_close(&bystander->object);
    object_close(&bystander_peer->object);
    object_close(&section->object);
    object_close(&spared->object);
    object_close(&nested->object);
    object_close(&doomed->object);
    handle_table_destroy(table);
    check(page_free_count() == free_before,
          "the kill test left a page allocated");
    object_close(&root->object);
    object_close(&irq->object);
}

/* ------------------------------------------------------------------------
   Killing tasks that run. */

static const UINT8 spin_code[] = {0xEB, 0xFE};

static void await_signal(WAITABLE *object, UINT32 signal, const char *reason)
{
    UINT32 observed;

    check(wait_one(object, signal, clock_monotonic() + JOIN_LIMIT_NS,
                   &observed) == OK,
          reason);
}

/* A new process in job whose count threads run the code in user mode.
   The caller holds a handle to the process and to each thread. */
static PROCESS *start_spinning(JOB *job, SECTION *code,
                               USER_THREAD **threads, UINT32 count)
{
    PROCESS *process = make_process(job);
    ENDPOINT *bootstrap = 0, *peer = 0;
    UINT32 i;

    check(process_map(process, code, 0, TEST_ADDRESS, PAGE_SIZE,
                      MAP_READ | MAP_EXECUTE) == OK &&
              channel_create(1, &bootstrap, &peer) == OK,
          "no memory for a running test process");
    for (i = 0; i < count; i++)
        check(process_thread_create(process, &threads[i]) == OK,
              "no memory for a test thread");
    check(process_start(process, threads[0], TEST_ADDRESS, TEST_STACK,
                        &bootstrap->object, CHANNEL_RIGHTS, 0) == OK,
          "a test process did not start");
    object_close(&peer->object);
    for (i = 1; i < count; i++)
        check(process_thread_start(threads[i], TEST_ADDRESS, TEST_STACK, 0,
                                   0) == OK,
              "a test thread did not start");
    return process;
}

static void close_threads(USER_THREAD **threads, UINT32 count)
{
    UINT32 i;

    for (i = 0; i < count; i++)
        object_close(&threads[i]->object);
}

/* One thread of a process that runs two is killed: it ends alone, and the
   process runs on, until the last thread is killed, which ends the
   process with code 0. A job's kill ends a process whose three threads
   run, each of them. A thread killed before it starts asserts
   THREAD_TERMINATED, wakes a waiter that was blocked on it, and never
   starts. After each kill interrupts are enabled again. */
static void test_kill_running(void)
{
    UINT64 free_before = page_free_count();
    JOB *root = job_create_root();
    JOB_LIMITS none;
    JOB *single = 0, *whole = 0;
    SECTION *code = 0;
    USER_THREAD *pair[2], *three[SPINNING_THREADS], *unstarted = 0;
    PROCESS *alone, *all, *idle;
    HANDLE_TABLE *table;
    ENDPOINT *bootstrap = 0, *peer = 0;
    UINT32 i;

    check(root && section_create(PAGE_SIZE, &code) == OK,
          "no memory for the running kill test");
    section_fill(code, spin_code, sizeof spin_code);
    table = handle_table_create();
    check(table != 0, "no memory for the running kill test's table");
    none.interrupt_vectors = 0;
    check(job_create(root, &none, &single) == OK &&
              job_create(root, &none, &whole) == OK,
          "no memory for the running kill test's jobs");

    alone = start_spinning(single, code, pair, 2);
    check(alone->state == PROCESS_STATE_RUNNING && alone->threads == 2,
          "a process did not start two threads");
    process_thread_kill(pair[1]);
    check(interrupts_enabled(), "process_thread_kill left interrupts "
                                "disabled");
    await_signal(&pair[1]->thread.waitable, THREAD_TERMINATED,
                 "a killed thread did not end");
    check(alone->state == PROCESS_STATE_RUNNING && !alone->terminating &&
              alone->threads == 1,
          "killing one of a process's threads ended the process");
    process_thread_kill(pair[0]);
    await_signal(&alone->waitable, PROCESS_TERMINATED,
                 "killing a process's last thread did not end it");
    check(ended_with(alone, 0),
          "a process whose last thread was killed did not end with code 0");
    await_signal(&pair[0]->thread.waitable, THREAD_TERMINATED,
                 "a killed last thread did not end");
    process_thread_kill(pair[0]);
    check(interrupts_enabled() && ended_with(alone, 0),
          "killing a thread that had ended changed its process");
    close_threads(pair, 2);
    object_close(&alone->object);

    all = start_spinning(whole, code, three, SPINNING_THREADS);
    check(all->threads == SPINNING_THREADS,
          "a process did not start its threads");
    job_kill(whole);
    check(interrupts_enabled(), "job_kill left interrupts disabled");
    await_signal(&all->waitable, PROCESS_TERMINATED,
                 "killing a job did not end a process whose threads run");
    check(ended_with(all, PROCESS_EXIT_KILLED),
          "a killed process whose threads ran did not end with "
          "PROCESS_EXIT_KILLED");
    for (i = 0; i < SPINNING_THREADS; i++)
        await_signal(&three[i]->thread.waitable, THREAD_TERMINATED,
                     "killing a process left one of its threads running");
    close_threads(three, SPINNING_THREADS);
    object_close(&all->object);

    idle = make_process(single);
    check(process_thread_create(idle, &unstarted) == OK &&
              channel_create(1, &bootstrap, &peer) == OK,
          "no memory for the unstarted thread test");
    object_retain(&unstarted->object);
    start_waiter(&channel_waiter, channel_waiter_main, table,
                 &unstarted->object, THREAD_RIGHTS, THREAD_TERMINATED);
    await_blocked(&channel_waiter);
    process_thread_kill(unstarted);
    check(interrupts_enabled(), "process_thread_kill left interrupts "
                                "disabled");
    await_signal(&channel_waiter.thread.waitable, THREAD_TERMINATED,
                 "killing a thread before it started did not wake a wait "
                 "for THREAD_TERMINATED");
    check(channel_waiter.status == OK &&
              (channel_waiter.entry.observed & THREAD_TERMINATED),
          "killing a thread before it started did not wake a wait for "
          "THREAD_TERMINATED");
    process_thread_kill(unstarted);
    await_signal(&unstarted->thread.waitable, THREAD_TERMINATED,
                 "a thread killed before it started did not stay ended");
    check(process_start(idle, unstarted, TEST_ADDRESS, TEST_STACK,
                        &bootstrap->object, CHANNEL_RIGHTS, 0) ==
              ERR_BAD_STATE &&
              untouched(idle),
          "process_start took a thread that had been killed");
    object_close(&bootstrap->object);
    object_close(&peer->object);
    object_release(&unstarted->object);
    object_close(&idle->object);

    handle_table_destroy(table);
    object_close(&code->object);
    object_close(&whole->object);
    object_close(&single->object);
    object_close(&root->object);
    check(page_free_count() == free_before,
          "the running kill test left a page allocated");
}

/* A tree of jobs with several children at each of three levels:

       top --- c1
         |
         +---- c2 --- d1
         |       |
         |       +--- d2 --- e
         |
         +---- c3

   each holding a process that never ran, and a process in a job outside
   it. Killing top ends every one under it and none outside. A child job
   that is destroyed leaves its parent's list at once, whether it is the
   head, in the middle or the last. */
static void test_kill_tree(void)
{
    enum { TOP, C1, C2, C3, D1, D2, E, JOBS };
    static const UINT32 parent[JOBS] = {TOP, TOP, TOP, TOP, C2, C2, D2};
    JOB *jobs[JOBS], *outside = job_create_root(), *spare = 0;
    PROCESS *in[JOBS], *beyond;
    JOB_LIMITS none;
    UINT32 i;

    check(outside != 0, "no memory for the tree kill test");
    none.interrupt_vectors = 0;
    jobs[TOP] = job_create_root();
    check(jobs[TOP] != 0, "no memory for the tree kill test");
    for (i = TOP + 1; i < JOBS; i++)
        check(job_create(jobs[parent[i]], &none, &jobs[i]) == OK,
              "no memory for the tree kill test's jobs");
    for (i = 0; i < JOBS; i++)
        in[i] = make_process(jobs[i]);
    beyond = make_process(outside);

    job_kill(jobs[TOP]);
    check(interrupts_enabled(), "job_kill left interrupts disabled");
    for (i = 0; i < JOBS; i++)
        check(jobs[i]->killed && ended_with(in[i], PROCESS_EXIT_KILLED),
              "killing a job left one of the jobs under it, or its "
              "process, alive");
    check(untouched(beyond) && !outside->killed,
          "killing a job reached one that is not under it");
    for (i = 0; i < JOBS; i++)
        check(in[i]->object.references == 1 && in[i]->object.handles == 1,
              "killing a job changed a process's references");

    for (i = 0; i < JOBS; i++)
        object_close(&in[i]->object);
    check(jobs[C2]->children == jobs[D2] &&
              jobs[D2]->next_sibling == jobs[D1],
          "a job's children were not on its list newest first");
    object_close(&jobs[D2]->object);
    check(jobs[C2]->children == jobs[D2] && jobs[E]->parent == jobs[D2],
          "a job with a child left its parent's list");
    object_close(&jobs[E]->object);
    check(jobs[C2]->children == jobs[D1] && !jobs[D1]->next_sibling &&
              jobs[D1]->sibling_link == &jobs[C2]->children,
          "a destroyed job stayed on its parent's list");
    check(job_create(jobs[C2], &none, &spare) == ERR_BAD_STATE,
          "a killed job took a child job");
    object_close(&jobs[D1]->object);
    check(!jobs[C2]->children, "a destroyed job was the last on a list");
    object_close(&jobs[C2]->object);
    check(jobs[TOP]->children == jobs[C3] &&
              jobs[C3]->next_sibling == jobs[C1] &&
              jobs[C1]->sibling_link == &jobs[C3]->next_sibling,
          "a destroyed job in the middle of a list broke it");
    object_close(&jobs[C3]->object);
    check(jobs[TOP]->children == jobs[C1] &&
              jobs[C1]->sibling_link == &jobs[TOP]->children,
          "a destroyed job at the head of a list broke it");
    job_kill(jobs[TOP]);
    check(interrupts_enabled(), "job_kill left interrupts disabled");

    object_close(&beyond->object);
    object_close(&outside->object);
    object_close(&jobs[C1]->object);
    object_close(&jobs[TOP]->object);
}

/* A kill ends processes whose handles close each other's: the holder of
   the last handle to a process that the kill has stopped but not yet
   finished, finished first, must not end it with a code of its own or
   lose it, and one visited first must not end the process it holds
   before the kill has stopped that. The test keeps a reference to each
   such process, which no handle closing can free. */
static void test_kill_lifetimes(void)
{
    JOB *finished_first = job_create_root(), *stopped_first = job_create_root();
    PROCESS *held_late, *holder_late, *holder_early, *held_early;

    check(finished_first && stopped_first, "no memory for the lifetime test");

    /* The older process is finished first, and holds the newer one. */
    holder_late = make_process(finished_first);
    held_late = make_process(finished_first);
    object_retain(&held_late->object);
    place(holder_late->handles, &held_late->object, PROCESS_RIGHTS);

    /* The newer process is stopped first, and holds the older one, which
       the kill has yet to reach. */
    held_early = make_process(stopped_first);
    holder_early = make_process(stopped_first);
    object_retain(&held_early->object);
    place(holder_early->handles, &held_early->object, PROCESS_RIGHTS);

    job_kill(finished_first);
    job_kill(stopped_first);
    check(interrupts_enabled(), "job_kill left interrupts disabled");
    check(ended_with(held_late, PROCESS_EXIT_KILLED) &&
              held_late->object.references == 1 &&
              held_late->object.handles == 0,
          "a process whose last handle closed while its job was killed "
          "was lost or ended with another code");
    check(ended_with(held_early, PROCESS_EXIT_KILLED) &&
              held_early->object.references == 1 &&
              held_early->object.handles == 0,
          "a process whose last handle closed while its job was killed "
          "ended before the kill stopped it");

    object_release(&held_late->object);
    object_release(&held_early->object);
    object_close(&holder_late->object);
    object_close(&holder_early->object);
    object_close(&finished_first->object);
    object_close(&stopped_first->object);
}

void object_self_test(void *boot_info)
{
    test_vector_quota();
    test_canceled_waits();
    test_watches();
    test_memory_types(boot_info);
    test_unmap();
    test_kill();
    test_kill_running();
    test_kill_tree();
    test_kill_lifetimes();
    selftest_passed(SELFTEST_OBJECT);
}
