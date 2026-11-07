/* init: the first process, which the kernel starts from \nova\init.exe
   with one bootstrap handle (docs/spec/ipc.md I6). It reads the bootstrap
   message, writes to the kernel's debug log with the root system
   resource the message carries, checks the answers of the calls it has
   to the cases their contracts name, starts the store service and the
   namespace server and runs a test program from /store (store.c), runs
   stage0-pe32's chain from its seed as 32-bit processes when the boot
   volume holds the chain's inputs (chain.c), and starts the virtio
   drivers as the platform driver (platform.c). Then it reports to the
   kernel on its bootstrap channel that its startup is done
   (docs/spec/boot-protocol.md, Startup done), hands the display to the
   console once the kernel releases it (console.c), and runs on as the
   drivers' supervisor (ipc.md I10).
   A failed check is logged, if the log is reachable, and ends the
   process with EXIT_CHECK_FAILED, with no report, which the kernel reads
   with process_info and logs; so does EXIT_SUPERVISION_FAILED, if
   waiting for the drivers fails. */

#include "init.h"
#include "console.h"
#include "platform.h"

#define EXIT_CHECK_FAILED 1
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
static BOOTSTRAP_BYTES bytes;
static PLATFORM platform;
static UINT64 scratch = SCRATCH_BASE;
static BOOT_FILE_VIEW files[BOOT_FILES_MAX];
static UINT32 file_count;
static BOOT_FILE_VIEW store_image;

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

static void check(int passed, const char *what)
{
    if (passed)
        return;
    say("nova-init: check failed: ");
    say(what);
    say("\r\n");
    process_exit(EXIT_CHECK_FAILED);
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
   leaves it queued and reports how much it holds; that read whole it
   holds them in that order, with the platform record and a name and size
   for the store image and each file; and that once it is read the queue
   is empty and the kernel's end still open, for the startup report. */
static UINT32 read_bootstrap(UINT32 bootstrap)
{
    BOOTSTRAP_FILE *names = bytes.names;
    RECEIVED_HANDLE handles[BOOTSTRAP_HANDLES];
    UINT32 byte_count, handle_count, i;

    if (channel_read(bootstrap, &bytes, sizeof bytes, handles, 0,
                     &byte_count, &handle_count) != ERR_BUFFER_TOO_SMALL ||
        handle_count < BOOTSTRAP_FIRST_FILE)
        process_exit(EXIT_CHECK_FAILED);
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
        process_exit(EXIT_CHECK_FAILED);
    debug_log = handles[BOOTSTRAP_RESOURCE].handle;
    say_open(debug_log);
    self = handles[BOOTSTRAP_SELF].handle;
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
    check(channel_read(bootstrap, &bytes, sizeof bytes, handles,
                       BOOTSTRAP_HANDLES, &byte_count,
                       &handle_count) == ERR_SHOULD_WAIT,
          "a drained bootstrap channel is not empty and open");
    return handles[BOOTSTRAP_JOB].handle;
}

/* Each call checks its handle's value, type and rights in that order
   (docs/spec/objects.md O3), then its buffer, which must lie in user
   memory and be mapped (docs/spec/syscalls.md S6). The bootstrap
   endpoint cannot leave init, since it lacks TRANSFER, so no other
   process can make init's startup report. No mapping of init's is
   writable and executable, nor may be: the kernel did not create it
   with PROCESS_OPTION_WRITE_EXECUTE (O5). */
static void check_errors(UINT32 bootstrap)
{
    static const char line[] = "-";
    HANDLE_TRANSFER transfer;
    UINT32 state, exit_code, section, mine, theirs, transaction = 0;

    check(debug_write(bootstrap, line, 1) == ERR_WRONG_TYPE,
          "debug_write took a channel");
    check(process_info(self, &state, &exit_code) == ERR_ACCESS_DENIED &&
              state == 0 && exit_code == 0,
          "process_info ran without INSPECT");
    check(debug_write(debug_log, (const void *)KERNEL_ADDRESS, 1) ==
              ERR_BAD_ADDRESS,
          "debug_write read kernel memory");
    check(debug_write(debug_log, (const void *)UNMAPPED_USER_ADDRESS, 1) ==
              ERR_BAD_ADDRESS,
          "debug_write read unmapped memory");
    check(debug_write(debug_log, line, DEBUG_WRITE_MAX_BYTES + 1) ==
              ERR_OUT_OF_RANGE,
          "debug_write took more than DEBUG_WRITE_MAX_BYTES");
    check(channel_create(1, &mine, &theirs) == OK, "channel_create failed");
    transfer.handle = bootstrap;
    transfer.rights = RIGHTS_SAME;
    check(channel_write(mine, &transaction, sizeof transaction, &transfer,
                        1) == ERR_ACCESS_DENIED,
          "the bootstrap endpoint moved in a message");
    check(handle_close(mine) == OK && handle_close(theirs) == OK,
          "a channel did not close");
    check(handle_close(mine) == ERR_BAD_HANDLE,
          "a closed handle closed again");
    check(debug_write(HANDLE_INVALID, line, 1) == ERR_BAD_HANDLE,
          "HANDLE_INVALID named a handle");
    check(section_create(PAGE_SIZE, 0, &section) == OK,
          "section_create failed");
    check(process_map(self, section, 0, scratch, PAGE_SIZE,
                      MAP_READ | MAP_WRITE | MAP_EXECUTE) == ERR_ACCESS_DENIED,
          "init mapped a page writable and executable");
    check(handle_close(section) == OK, "a section did not close");
}

/* A watch queues one signal packet, carrying its key and the signals
   asked for and active, at once for a signal already active, and none
   once its handle has closed; a port is not waitable (O9, ipc.md I4). */
static void check_watches(void)
{
    PORT_PACKET packet;
    UINT32 port, first, second;

    check(port_create(1, &port) == OK, "no port for the watch checks");
    check(channel_create(1, &first, &second) == OK,
          "no channel for the watch checks");
    check(object_wait_async(port, port, CHECK_KEY, CHANNEL_READABLE) ==
              ERR_NOT_SUPPORTED,
          "object_wait_async watched a port");
    check(object_wait_async(first, port, CHECK_KEY, CHANNEL_READABLE) ==
                  OK &&
              handle_close(first) == OK &&
              port_wait(port, 0, &packet) == ERR_TIMED_OUT,
          "a watch outlived its handle");
    check(object_wait_async(second, port, CHECK_KEY, CHANNEL_PEER_CLOSED) ==
                  OK &&
              port_wait(port, 0, &packet) == OK &&
              packet.type == PACKET_SIGNAL && packet.key == CHECK_KEY &&
              (UINT32)packet.payload[0] == CHANNEL_PEER_CLOSED &&
              (packet.payload[0] >> PACKET_SIGNAL_ACTIVE_SHIFT &
               CHANNEL_PEER_CLOSED),
          "a watch of a signal already active queued no packet");
    check(port_wait(port, 0, &packet) == ERR_TIMED_OUT,
          "a watch queued a second packet");
    handle_close(second);
    handle_close(port);
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

static void check_clock(void)
{
    UINT64 first, second;

    check(clock_monotonic(&first) == OK && clock_monotonic(&second) == OK &&
              second >= first && first > 0,
          "the monotonic clock went back");
}

_Noreturn void init_main(UINT32 bootstrap, UINT64 argument)
{
    UINT32 job, status;

    (void)argument;
    job = read_bootstrap(bootstrap);
    say("nova-init: running in user mode, bootstrap message read\r\n");
    check_errors(bootstrap);
    check_watches();
    check_clock();
    check(store_run(job, debug_log, &store_image),
          "the test program did not pass from /store");
    check(chain_run(job), "the seed chain did not build nova-nix's links");
    check(platform_start(&platform), "the platform driver's startup failed");
    say("nova-init: checks passed\r\n");
    report_startup_done(bootstrap);
    console_start(&platform, bootstrap);
    status = platform_supervise(&platform);
    say("nova-init: supervision stopped, status ");
    say_number(status);
    say("\r\n");
    process_exit(EXIT_SUPERVISION_FAILED);
}
