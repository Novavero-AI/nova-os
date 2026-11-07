/* The nova-os kernel's entry. nova-boot calls kernel_main after
   ExitBootServices, with interrupts disabled, on the kernel's own page
   tables and stack (docs/spec/boot-protocol.md), so the firmware is gone
   and the kernel owns the machine.

   It checks that BOOT_INFO is the record it was built for; installs its
   own descriptor tables and exception handlers and removes the lower-half
   mapping nova-boot left for the switch; builds the physical page
   allocator, checks it and reports on the first serial port; reads the
   ACPI tables; starts its clock, the scheduler and the timer; seeds the
   random number generator; starts a thread that keeps the boot screen's
   spinner turning and a startup thread that tests the kernel's objects
   and threads, starts the first user process and, once it reports that
   its startup is done, draws the desktop background offscreen, stops the
   spinner, fades the boot screen into the background and releases the
   display to the process, then logs the process's end if it ever comes;
   and becomes the idle thread. */

#include "background.h"
#include "boot_info.h"
#include "cpu.h"
#include "acpi/acpi.h"
#include "mm/memory_map.h"
#include "mm/page.h"
#include "object/process.h"
#include "random/random.h"
#include "sched/thread.h"
#include "sched/wait.h"
#include "x86_64/arch.h"
#include "x86_64/clock.h"
#include "x86_64/serial.h"

#define FRAME_MS 33
#define BYTES_PER_MIB (1024 * 1024)

/* Signalled by the startup thread once the first process has reported
   that its startup is done, or cannot report it any more; the spinner
   stops on it. */
#define SPINNER_STOP USER_SIGNAL(0)

static THREAD spinner_thread;
static THREAD startup_thread;
static EVENT spinner_stop;

/* The frames the spinner has started: written by the spinner's thread,
   read by the startup thread while it runs. A frame counts as it starts,
   so that its own drawing does not delay the count past the bound
   ci/boot.py holds the spinner to. */
static volatile UINT64 spinner_frames;

static void serial_mib(UINT64 pages)
{
    serial_number(pages * PAGE_SIZE / BYTES_PER_MIB);
    serial_write(" MiB");
}

static void report_memory(const BOOT_INFO *info)
{
    const char *failure;

    serial_write("nova: memory ");
    serial_mib(page_free_count());
    serial_write(" free of ");
    serial_mib(page_total_count());
    serial_write("\r\n");

    failure = page_self_test(info);
    if (failure) {
        serial_write("nova: page allocator self-test failed: ");
        serial_write(failure);
    } else {
        serial_write("nova: page allocator self-test passed");
    }
    serial_write("\r\n");
}

/* A record from another loader, or from a nova-boot with another layout,
   cannot be read past its first two fields. */
static void check_boot_info(const BOOT_INFO *info)
{
    if (info->magic == BOOT_INFO_MAGIC && info->version == BOOT_INFO_VERSION)
        return;
    serial_write("nova: boot info magic ");
    serial_hex(info->magic);
    serial_write(" version ");
    serial_number(info->version);
    serial_write(", expected version ");
    serial_number(BOOT_INFO_VERSION);
    serial_write(", halted\r\n");
    halt_forever();
}

/* A frame every FRAME_MS, on absolute deadlines, so that a late frame
   does not delay the ones after it, until it is told to stop. */
static void spin(void *argument)
{
    BOOT_INFO *info = argument;
    UINT64 next = clock_monotonic();
    UINT32 observed;

    for (;;) {
        spinner_frames++;
        gfx_spinner_step(&info->screen, &info->spinner);
        next += FRAME_MS * NS_PER_MS;
        if (wait_one(&spinner_stop.waitable, SPINNER_STOP, next,
                     &observed) == OK)
            return;
    }
}

/* Stop the spinner and wait for its thread to end, after which nothing
   but the caller draws. */
static void stop_spinner(void)
{
    UINT32 observed;

    event_signal(&spinner_stop, 0, SPINNER_STOP);
    wait_one(&spinner_thread.waitable, THREAD_TERMINATED, DEADLINE_INFINITE,
             &observed);
}

/* Prepare the background while the spinner turns, and log how long that
   took and how many frames the spinner started meanwhile, which
   ci/boot.py checks against the longest a turning spinner goes between
   frames. The count is read before the clock at the start and after it
   at the end, so that it covers all the time logged. */
static void prepare_while_spinning(const BOOT_INFO *info,
                                   BACKGROUND *background)
{
    UINT64 frames, start, took, flags;

    frames = spinner_frames;
    start = clock_monotonic();
    background_prepare(info, background);
    took = clock_monotonic() - start;
    frames = spinner_frames - frames;
    if (background->state != BACKGROUND_READY)
        return;
    flags = serial_lock();
    serial_write("nova: the spinner started ");
    serial_number(frames);
    serial_write(frames == 1 ? " frame" : " frames");
    serial_write(" while the background was prepared, in ");
    serial_ms(took);
    serial_write("\r\n");
    serial_unlock(flags);
}

/* Test the kernel's objects and threads and start the first process.
   Once it has reported that its startup is done, draw the desktop
   background offscreen while the spinner still turns, since the drawing
   takes seconds under emulation on a large screen; then stop the spinner
   and, once its thread has ended so that nothing else draws, fade into
   the background and release the display, after which the kernel never
   draws (docs/spec/console.md C1). If it closed its bootstrap channel
   without the report, stop the spinner and leave the boot screen. The
   first process then runs on as the supervisor (docs/spec/ipc.md I10),
   and the thread logs its end if it comes. The thread self-test counts
   free pages, so no other thread may allocate or free any while it runs;
   it runs before the first process, since the processes it starts run
   on. */
static void startup(void *argument)
{
    BOOT_INFO *info = argument;
    BACKGROUND background;

    object_self_test(info);
    sched_self_test(&spinner_thread);
    if (first_process_start(info)) {
        prepare_while_spinning(info, &background);
        stop_spinner();
        background_show(info, &background);
        first_process_release_display();
    } else {
        stop_spinner();
    }
    first_process_wait();
}

static void start_clock(UINT64 tsc_per_ms)
{
    if (!clock_init(tsc_per_ms)) {
        serial_write("nova: boot info gives no timestamp counter rate, "
                     "halted\r\n");
        halt_forever();
    }
    serial_write("nova: clock from the timestamp counter at ");
    serial_number(tsc_per_ms);
    serial_write(" ticks per ms\r\n");
}

/* A thread just created is new, so thread_start cannot refuse it. */
static void start_thread(THREAD *thread, THREAD_ENTRY entry, void *argument)
{
    if (thread_create(thread, entry, argument) != OK) {
        serial_write("nova: no memory for a thread, halted\r\n");
        halt_forever();
    }
    thread_start(thread);
}

VOID kernel_main(BOOT_INFO *info)
{
    serial_write("nova: kernel running, boot services exited\r\n");
    check_boot_info(info);
    serial_write("nova: screen ");
    serial_number(info->screen.width);
    serial_write("x");
    serial_number(info->screen.height);
    serial_write("\r\n");
    arch_init();
    if (!page_init(info)) {
        serial_write("nova: no memory for the page allocator\r\n");
        halt_forever();
    }
    report_memory(info);
    memory_map_init(info);
    acpi_init(info);
    start_clock(info->tsc_per_ms);
    sched_init();
    arch_start_timer(info->tsc_per_ms);
    serial_write("nova: scheduler running, time slice ");
    serial_number(TIME_SLICE_MS);
    serial_write(" ms\r\n");
    random_init(info);
    event_init(&spinner_stop);
    start_thread(&spinner_thread, spin, info);
    start_thread(&startup_thread, startup, info);
    sched_idle();
}
