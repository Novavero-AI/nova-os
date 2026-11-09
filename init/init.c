/* init: the first process, which the kernel starts from \nova\init.exe
   with one bootstrap handle (docs/spec/ipc.md I6). It reads the bootstrap
   message, writes to the kernel's debug log with the root system
   resource the message carries, starts the store service and the
   namespace server (store.c), and starts the virtio drivers as the
   platform driver (platform.c). Before the store service it starts the
   data disk's file system service, whose /data and /users the namespace
   then publishes beside /store (data.c). On a test disk it also runs
   its tests and reports them (tests.h): its checks of the answers of
   the calls it has against the cases their contracts name, task_kill's
   against processes it starts for the purpose (kill.c), the data
   disk's check (data.c), a test program from /store (store.c),
   stage0-pe32's chain from its seed as 32-bit processes when the boot
   volume holds the chain's inputs (chain.c), and the virtio block
   driver's read of the boot disk (platform.c). Then it takes the desktop
   background the kernel drew from its bootstrap channel, gives it to
   the console to prepare its panel from (console.c), reports to the
   kernel there that its startup is done (docs/spec/boot-protocol.md,
   Startup done), hands the display to the console once the kernel
   releases it, or to the type specimen from /store on a boot disk that
   asks for it (specimen_start.c), starts the accounts service and login
   (login_start.c), and runs on as the supervisor of the drivers, the
   console and those two (ipc.md I10). The store service and the
   namespace server run on after startup, and init keeps its channels
   to them (STORE_SERVICES).
   A failed step of its startup is logged, if the log is reachable, and
   ends the process with EXIT_STARTUP_FAILED, with no report, which the
   kernel reads with process_info and logs; so does
   EXIT_SUPERVISION_FAILED, if waiting for the drivers fails. */

#include "init.h"
#include "console.h"
#include "data.h"
#include "login_start.h"
#include "platform.h"
#include "specimen_start.h"
#include "tests.h"

#define EXIT_STARTUP_FAILED 1
#define EXIT_SUPERVISION_FAILED 2

/* The key of the watches check_watches registers. */
#define CHECK_KEY 1

/* The handles the bootstrap message carries (native_abi.h): the root
   system resource, the process itself, the root job, the root MMIO, IRQ
   and DMA resources, the store image, then the boot files. */
#define BOOTSTRAP_HANDLES (BOOTSTRAP_FIRST_FILE + BOOT_FILES_MAX)
#define BOOTSTRAP_RESOURCE 0
#define BOOTSTRAP_SELF 1
#define BOOTSTRAP_JOB 2

/* An address in the kernel's half, and one in user memory that nothing
   maps (the first page of user memory, below the image). */
#define KERNEL_ADDRESS 0xffff800000000000ULL
#define UNMAPPED_USER_ADDRESS 0x10000ULL

/* The bootstrap message's bytes: the platform record, then the store
   image's name and size and the files' (native_abi.h). */
typedef struct {
    BOOTSTRAP_PLATFORM platform;
    BOOTSTRAP_FILE store_image;
    BOOTSTRAP_FILE names[BOOT_FILES_MAX];
} BOOTSTRAP_BYTES;

/* The rights the kernel gives every section it passes: read, and pass
   on, never write. */
#define BOOT_SECTION_RIGHTS                                               \
    (SECTION_RIGHT_READ | RIGHT_DUPLICATE | RIGHT_TRANSFER)

/* channel_write needs a valid array even for no handles. */
static const HANDLE_TRANSFER no_handles[1];

static UINT32 debug_log = HANDLE_INVALID;
static UINT32 self = HANDLE_INVALID;
static LAYOUT_WINDOW layout;
static BOOTSTRAP_BYTES bytes;
static PLATFORM platform;
static UINT64 scratch = SCRATCH_BASE;
static BOOT_FILE_VIEW files[BOOT_FILES_MAX];
static UINT32 file_count;
static BOOT_FILE_VIEW store_image;
static STORE_SERVICES services;

static int lower(int c)
{
    return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c;
}

int same_name(const char *a, const char *b)
{
    while (*a && lower(*a) == lower(*b)) {
        a++;
        b++;
    }
    return lower(*a) == lower(*b);
}

const BOOT_FILE_VIEW *boot_file(const char *name)
{
    UINT32 i;

    for (i = 0; i < file_count; i++)
        if (same_name(files[i].name, name))
            return &files[i];
    return 0;
}

const LAYOUT_WINDOW *init_layout(void)
{
    return &layout;
}

UINT32 load_boot_file(UINT32 job, const BOOT_FILE_VIEW *file,
                      PROGRAM *program)
{
    program->process = HANDLE_INVALID;
    program->thread = HANDLE_INVALID;
    if (!file)
        return ERR_NOT_FOUND;
    return program_load(&layout, job, file->data, file->size, program);
}

UINT8 *map_into_init_at(UINT32 section, UINT64 address, UINT64 size,
                        UINT32 protection)
{
    if (process_map(self, section, 0, address, size, protection) != OK)
        return 0;
    return (UINT8 *)address;
}

UINT8 *map_into_init(UINT32 section, UINT64 size, UINT32 protection)
{
    UINT8 *address;

    size = (size + PAGE_SIZE - 1) / PAGE_SIZE * PAGE_SIZE;
    address = map_into_init_at(section, scratch, size, protection);
    if (address)
        scratch += size;
    return address;
}

UINT32 unmap_from_init(const UINT8 *address, UINT64 size)
{
    return process_unmap(self, (UINT64)address,
                         (size + PAGE_SIZE - 1) / PAGE_SIZE * PAGE_SIZE);
}

void say_status(const char *event, UINT32 status)
{
    say("nova-init: ");
    say(event);
    say(" status=");
    say_number(status);
    say("\r\n");
}

/* A step of startup that must succeed. */
static void check(int passed, const char *what)
{
    if (passed)
        return;
    say("nova-init: startup failed reason=");
    say_value(what);
    say("\r\n");
    process_exit(EXIT_STARTUP_FAILED);
}

/* A section the kernel passed, named and sized at name, as a read-only
   view into init. */
static void take_section(const RECEIVED_HANDLE *handle,
                         const BOOTSTRAP_FILE *name, BOOT_FILE_VIEW *view)
{
    UINT32 j;

    check(handle->type == TYPE_SECTION && handle->rights == BOOT_SECTION_RIGHTS,
          "a boot file is not a read-only section");
    for (j = 0; j < BOOT_FILE_NAME_SIZE; j++)
        view->name[j] = name->name[j];
    view->name[BOOT_FILE_NAME_SIZE - 1] = 0;
    view->section = handle->handle;
    view->size = name->size;
    view->data = name->size ? map_into_init(view->section, name->size, MAP_READ)
                            : (const UINT8 *)0;
    check(!name->size || view->data, "a boot file did not map");
}

/* Take the debug log's resource, the process's own handle, the root job,
   the root device resources, the store image and the boot files from the
   bootstrap message, checking that a buffer too small for its handles
   leaves it queued and reports how much it holds, and that read whole it
   holds them in that order, with the platform record and a name and size
   for the store image and each file. The kernel's background follows it
   on the channel, whenever the kernel has drawn it (take_background). */
static UINT32 read_bootstrap(UINT32 bootstrap)
{
    BOOTSTRAP_FILE *names = bytes.names;
    RECEIVED_HANDLE handles[BOOTSTRAP_HANDLES];
    UINT32 byte_count, handle_count, i;

    if (channel_read(bootstrap, &bytes, sizeof bytes, handles, 0,
                     &byte_count, &handle_count) != ERR_BUFFER_TOO_SMALL ||
        handle_count < BOOTSTRAP_FIRST_FILE)
        process_exit(EXIT_STARTUP_FAILED);
    if (channel_read(bootstrap, &bytes, sizeof bytes, handles,
                     BOOTSTRAP_HANDLES, &byte_count, &handle_count) != OK ||
        handle_count < BOOTSTRAP_FIRST_FILE ||
        byte_count != sizeof bytes.platform + sizeof bytes.store_image +
                          (handle_count - BOOTSTRAP_FIRST_FILE) *
                              sizeof(BOOTSTRAP_FILE) ||
        handles[BOOTSTRAP_RESOURCE].type != TYPE_RESOURCE ||
        !(handles[BOOTSTRAP_RESOURCE].rights & RESOURCE_RIGHT_USE) ||
        handles[BOOTSTRAP_SELF].type != TYPE_PROCESS ||
        handles[BOOTSTRAP_JOB].type != TYPE_JOB ||
        handles[BOOTSTRAP_ROOT_MMIO].type != TYPE_RESOURCE ||
        handles[BOOTSTRAP_ROOT_IRQ].type != TYPE_RESOURCE ||
        handles[BOOTSTRAP_ROOT_DMA].type != TYPE_RESOURCE)
        process_exit(EXIT_STARTUP_FAILED);
    debug_log = handles[BOOTSTRAP_RESOURCE].handle;
    say_open(debug_log);
    self = handles[BOOTSTRAP_SELF].handle;
    layout.self = self;
    layout.window = LAYOUT_BASE;
    layout.window_size = LAYOUT_SIZE;
    platform.debug_log = debug_log;
    platform.self = self;
    platform.job = handles[BOOTSTRAP_JOB].handle;
    platform.mmio = handles[BOOTSTRAP_ROOT_MMIO].handle;
    platform.irq = handles[BOOTSTRAP_ROOT_IRQ].handle;
    platform.dma = handles[BOOTSTRAP_ROOT_DMA].handle;
    platform.boot = &bytes.platform;
    take_section(&handles[BOOTSTRAP_STORE_IMAGE], &bytes.store_image,
                 &store_image);
    for (i = BOOTSTRAP_FIRST_FILE; i < handle_count; i++)
        take_section(&handles[i], &names[i - BOOTSTRAP_FIRST_FILE],
                     &files[file_count++]);
    return handles[BOOTSTRAP_JOB].handle;
}

void boot_background_close(BOOT_BACKGROUND *background)
{
    UINT32 i;

    for (i = 0; i < background->count; i++)
        handle_close(background->sections[i]);
    background->count = 0;
}

/* Whether label is one native_abi.h gives for an image of sections
   sections: Limb, with as many as its size takes, or none, with every
   field 0 and no section. */
static int background_label_valid(const BOOTSTRAP_BACKGROUND *label,
                                  UINT32 sections)
{
    if (label->background == BOOTSTRAP_BACKGROUND_NONE)
        return label->width == 0 && label->height == 0 &&
               label->format == 0 && sections == 0;
    return label->background == BOOTSTRAP_BACKGROUND_LIMB &&
           sections ==
               bootstrap_background_sections(label->width, label->height);
}

/* Wait for the kernel's background on the bootstrap channel and take it
   into *background, checking that it is the message native_abi.h gives,
   with each section read-only, and that once it is read the queue is
   empty and the kernel's end still open, for the startup report. */
static void take_background(UINT32 bootstrap, BOOT_BACKGROUND *background)
{
    BOOTSTRAP_BACKGROUND_MESSAGE message;
    RECEIVED_HANDLE handles[BOOTSTRAP_BACKGROUND_SECTIONS_MAX];
    UINT32 byte_count = 0, handle_count = 0, observed, i;

    check(object_wait_one(bootstrap, CHANNEL_READABLE | CHANNEL_PEER_CLOSED,
                          DEADLINE_INFINITE, &observed) == OK &&
              channel_read(bootstrap, &message, sizeof message, handles,
                           BOOTSTRAP_BACKGROUND_SECTIONS_MAX, &byte_count,
                           &handle_count) == OK,
          "the kernel gave no background");
    background->count = handle_count;
    for (i = 0; i < handle_count; i++)
        background->sections[i] = handles[i].handle;
    check(byte_count == sizeof message && message.transaction == 0 &&
              background_label_valid(&message.background, handle_count),
          "the kernel's background is not one native_abi.h gives");
    for (i = 0; i < handle_count; i++)
        check(handles[i].type == TYPE_SECTION &&
                  handles[i].rights == BOOT_SECTION_RIGHTS,
              "a background section is not a read-only section");
    background->label = message.background;
    check(channel_read(bootstrap, &message, sizeof message, handles,
                       BOOTSTRAP_BACKGROUND_SECTIONS_MAX, &byte_count,
                       &handle_count) == ERR_SHOULD_WAIT,
          "a drained bootstrap channel is not empty and open");
}

/* Each call checks its handle's value, type and rights in that order
   (docs/spec/objects.md O3), then its buffer, which must lie in user
   memory and be mapped (docs/spec/syscalls.md S6). Each check below
   returns the first case that does not hold, or 0, and leaves nothing
   it opened open. */
static const char *check_arguments(UINT32 bootstrap)
{
    /* A newline, so that a call that writes when it should not leaves
       a blank line in the log, outside any other. */
    static const char line[] = "\n";
    UINT32 state, exit_code;

    if (debug_write(bootstrap, line, 1) != ERR_WRONG_TYPE)
        return "debug_write took a channel";
    if (process_info(self, &state, &exit_code) != ERR_ACCESS_DENIED ||
        state != 0 || exit_code != 0)
        return "process_info ran without INSPECT";
    if (debug_write(debug_log, (const void *)KERNEL_ADDRESS, 1) !=
        ERR_BAD_ADDRESS)
        return "debug_write read kernel memory";
    if (debug_write(debug_log, (const void *)UNMAPPED_USER_ADDRESS, 1) !=
        ERR_BAD_ADDRESS)
        return "debug_write read unmapped memory";
    if (debug_write(debug_log, line, DEBUG_WRITE_MAX_BYTES + 1) !=
        ERR_OUT_OF_RANGE)
        return "debug_write took more than DEBUG_WRITE_MAX_BYTES";
    if (debug_write(HANDLE_INVALID, line, 1) != ERR_BAD_HANDLE)
        return "HANDLE_INVALID named a handle";
    return 0;
}

/* The bootstrap endpoint cannot leave init, since it lacks TRANSFER, so
   no other process can make init's startup report; a closed handle
   names nothing. */
static const char *check_transfer(UINT32 bootstrap)
{
    HANDLE_TRANSFER transfer;
    UINT32 mine, theirs, transaction = 0;
    const char *failure = 0;
    int closed;

    if (channel_create(1, &mine, &theirs) != OK)
        return "channel_create failed";
    transfer.handle = bootstrap;
    transfer.rights = RIGHTS_SAME;
    if (channel_write(mine, &transaction, sizeof transaction, &transfer,
                      1) != ERR_ACCESS_DENIED)
        failure = "the bootstrap endpoint moved in a message";
    closed = handle_close(mine) == OK;
    closed &= handle_close(theirs) == OK;
    if (!closed)
        return failure ? failure : "a channel did not close";
    if (!failure && handle_close(mine) != ERR_BAD_HANDLE)
        failure = "a closed handle closed again";
    return failure;
}

/* No mapping of init's is writable and executable, nor may be: the
   kernel did not create it with PROCESS_OPTION_WRITE_EXECUTE (O5). */
static const char *check_mapping(void)
{
    UINT32 section, status;
    const char *failure = 0;

    if (section_create(PAGE_SIZE, 0, &section) != OK)
        return "section_create failed";
    status = process_map(self, section, 0, scratch, PAGE_SIZE,
                         MAP_READ | MAP_WRITE | MAP_EXECUTE);
    if (status == OK)
        process_unmap(self, scratch, PAGE_SIZE);
    if (status != ERR_ACCESS_DENIED)
        failure = "init mapped a page writable and executable";
    if (handle_close(section) != OK && !failure)
        failure = "a section did not close";
    return failure;
}

/* A watch queues one signal packet, carrying its key and the signals
   asked for and active, at once for a signal already active, and none
   once its handle has closed; a port is not waitable (O9, ipc.md I4).
   first is closed here, and HANDLE_INVALID once it is. */
static const char *watch_failure(UINT32 port, UINT32 *first, UINT32 second)
{
    PORT_PACKET packet;

    if (object_wait_async(port, port, CHECK_KEY, CHANNEL_READABLE) !=
        ERR_NOT_SUPPORTED)
        return "object_wait_async watched a port";
    if (object_wait_async(*first, port, CHECK_KEY, CHANNEL_READABLE) != OK ||
        handle_close(*first) != OK)
        return "a watch outlived its handle";
    *first = HANDLE_INVALID;
    if (port_wait(port, 0, &packet) != ERR_TIMED_OUT)
        return "a watch outlived its handle";
    if (object_wait_async(second, port, CHECK_KEY, CHANNEL_PEER_CLOSED) !=
            OK ||
        port_wait(port, 0, &packet) != OK || packet.type != PACKET_SIGNAL ||
        packet.key != CHECK_KEY ||
        (UINT32)packet.payload[0] != CHANNEL_PEER_CLOSED ||
        !(packet.payload[0] >> PACKET_SIGNAL_ACTIVE_SHIFT &
          CHANNEL_PEER_CLOSED))
        return "a watch of a signal already active queued no packet";
    if (port_wait(port, 0, &packet) != ERR_TIMED_OUT)
        return "a watch queued a second packet";
    return 0;
}

static const char *check_watches(void)
{
    UINT32 port, first = HANDLE_INVALID, second = HANDLE_INVALID;
    const char *failure;

    if (port_create(1, &port) != OK)
        return "no port for the watch checks";
    if (channel_create(1, &first, &second) == OK) {
        failure = watch_failure(port, &first, second);
    } else {
        first = HANDLE_INVALID;
        second = HANDLE_INVALID;
        failure = "no channel for the watch checks";
    }
    if (first != HANDLE_INVALID)
        handle_close(first);
    if (second != HANDLE_INVALID)
        handle_close(second);
    handle_close(port);
    return failure;
}

static const char *check_clock(void)
{
    UINT64 first, second;

    if (clock_monotonic(&first) != OK || clock_monotonic(&second) != OK ||
        second < first || first == 0)
        return "the monotonic clock went back";
    return 0;
}

/* The calls test: init's checks of the calls it makes against the cases
   their contracts name, stopping at the first that does not hold. */
static void test_calls(UINT32 bootstrap, UINT32 job)
{
    const char *failure = check_arguments(bootstrap);

    if (!failure)
        failure = check_transfer(bootstrap);
    if (!failure)
        failure = check_mapping();
    if (!failure)
        failure = check_watches();
    if (!failure)
        failure = check_clock();
    if (!failure)
        failure = kill_check(self, job);
    tests_report(INIT_TEST_CALLS, failure ? KTAP_FAILED : KTAP_PASSED,
                 failure);
}

/* Report to the kernel that startup is done. The channel stays open: the
   kernel's close of its end releases the display (console.c). */
static void report_startup_done(UINT32 bootstrap)
{
    UINT32 transaction = 0;

    check(channel_write(bootstrap, &transaction, BOOTSTRAP_DONE_SIZE,
                        no_handles, 0) == OK,
          "the kernel took no startup report");
}

/* The console's panel is prepared before the report, so that the
   display shows it as soon as the kernel releases it (console.md C1);
   the specimen, which draws in the console's place, takes no
   background. */
_Noreturn void init_main(UINT32 bootstrap, UINT64 argument)
{
    BOOT_BACKGROUND background;
    UINT32 job, status;
    int specimen;

    (void)argument;
    job = read_bootstrap(bootstrap);
    tests_begin();
    if (tests_enabled())
        test_calls(bootstrap, job);
    data_start(&platform, job);
    check(store_run(job, debug_log, &store_image, &services),
          "the store service and the namespace server did not start");
    chain_run(job);
    check(platform_start(&platform), "the platform driver's startup failed");
    tests_end();
    take_background(bootstrap, &background);
    specimen = boot_file(SPECIMEN_MARKER) != 0;
    if (specimen)
        boot_background_close(&background);
    else
        console_prepare(&platform, &background);
    report_startup_done(bootstrap);
    if (specimen)
        specimen_start(&platform, &services, bootstrap);
    else
        console_start(&platform, bootstrap);
    login_start(&platform, &services);
    status = platform_supervise(&platform);
    say_status("supervision failed", status);
    process_exit(EXIT_SUPERVISION_FAILED);
}
