/* The platform driver. It walks PCI configuration space through the ECAM
   ranges the kernel found in the ACPI MCFG table, from each range's
   first bus down through the bridges, mapping each bus it visits once,
   and logs each function. Two functions get a driver, each a process of
   its own in a job of its own, whose bootstrap message (pci.h) carries
   resources covering only that function: its configuration space, its
   memory BARs, its message-signalled interrupts and an initiator for
   its requester ID, with PIN alone. init keeps the initiator's RELEASE.

   The first virtio block function goes to \nova\virtblk.exe, which reads
   the boot disk and ends. The walk notes the second as the data disk,
   whose driver and file system service data.c starts, with this file's
   BAR sizing and driver handles. The first virtio input function goes to
   \nova\virtinp.exe, which runs on: once its device is live it reports
   with an endpoint, which init hands to its client, the console,
   \nova\console.exe, whose bootstrap message console.c writes
   (docs/spec/input.md, docs/spec/console.md).

   init supervises the three for the life of the system (ipc.md I10). It
   waits for them through one port, on which a watch of each one's
   process queues a packet when it ends, and a watch of the input
   driver's bootstrap channel one when it reports. One that ends, or does
   not start, is started again as the restart policy below allows, a
   driver once init has stopped its function: cleared Bus Master Enable,
   reset the device and released the initiator's quarantine. The block
   driver's end with VIRTIO_BLK_EXIT_DONE is the end of its work, and it
   is not started again. The client keeps its bootstrap channel to init,
   which hands it each endpoint the input driver reports: the first, and
   one after each restart of the driver. */

#include "platform.h"
#include "console.h"
#include "init.h"
#include "input_protocol.h"
#include "mmio.h"
#include "pci.h"
#include "virtio_blk.h"
#include "virtio_input.h"
#include "virtio_pci.h"

#define BUS_SIZE ((UINT64)1 << PCI_BUS_SHIFT)
#define BUSES 256
#define CHANNEL_CAPACITY 1
#define ALL_ONES 0xffffffffu
#define BLOCK_FILE "virtblk.exe"
#define INPUT_FILE "virtinp.exe"
#define CLIENT_FILE "console.exe"

/* The rights the driver's handles carry: its own process with MAP, to
   map its sections, and the initiator with PIN, so that releasing the
   quarantine stays with the supervisor. */
#define DRIVER_SELF_RIGHTS (PROCESS_RIGHT_MAP | RIGHT_TRANSFER)
#define DRIVER_INITIATOR_RIGHTS (INITIATOR_RIGHT_PIN | RIGHT_TRANSFER)

/* The rights a client's endpoint travels with: it reads key events and
   waits for them, and passes the endpoint nowhere. */
#define CLIENT_ENDPOINT_RIGHTS (CHANNEL_RIGHT_READ | RIGHT_WAIT)

/* The interrupt vectors a driver's job may hold: each driver takes one,
   for its queues, and none for configuration changes. */
#define DRIVER_INTERRUPT_VECTORS 1

static const JOB_LIMITS driver_limits = {DRIVER_INTERRUPT_VECTORS};

/* The restart policy (ipc.md I10). A program that ends, or does not
   start, is started again unless it was restarted within the last
   RESTART_PERIOD_SECONDS; then it is given up, and a driver's function
   stays stopped. A failure that comes back that soon after a restart is
   one a restart does not cure, and starting the program again would
   only repeat it as fast as the program fails, so a program that fails
   on every start runs twice. A minute is far longer than a driver takes
   to start and fail, under a second even emulated, and short enough
   that a fault that comes back only now and then gets its restart each
   time. */
#define RESTART_PERIOD_SECONDS 60
#define NS_PER_SECOND 1000000000ULL
#define RESTART_PERIOD_NS (RESTART_PERIOD_SECONDS * NS_PER_SECOND)

/* The port init waits on queues only watches' packets, none of the user
   packets its capacity counts. */
#define SUPERVISOR_PORT_CAPACITY 1

/* The client's bootstrap channel queues its bootstrap message and the
   endpoints it has yet to take: the one from the driver's start and the
   one from the restart the policy allows within RESTART_PERIOD_SECONDS,
   should the client take neither meanwhile. */
#define CLIENT_CHANNEL_CAPACITY 3

/* A packet's key: the child's index in bits 7:0, what the watch waits
   for in bits 15:8, and the child's start number from bit 16 on, so a
   packet from a watch of an earlier start is told apart. */
#define KEY_EVENT_SHIFT 8
#define KEY_START_SHIFT 16
#define KEY_FIELD_MASK 0xff
#define EVENT_ENDED 0
#define EVENT_REPORTED 1

/* What a child does once started: a driver that does its work and ends,
   one that reports with an endpoint and runs on, or that driver's
   client. */
#define KIND_FINISHES 0
#define KIND_REPORTS 1
#define KIND_CLIENT 2

/* How a child stands: not started, or not to be; started, with what it
   was started for still awaited, its end for one that finishes and its
   report for one that reports; running; done with its work; or given
   up. */
#define STATE_STOPPED 0
#define STATE_STARTING 1
#define STATE_RUNNING 2
#define STATE_DONE 3
#define STATE_GIVEN_UP 4

typedef struct {
    UINT32 requester;
    UINT64 config;          /* physical, or 0 if no function was found */
    volatile UINT8 *mapped; /* in init, inside its bus's mapping */
    volatile UINT8 *common; /* its virtio common configuration, mapped
                               in init once it is first reset, or 0 */
} FUNCTION;

/* The first function of each kind a driver serves, and the data disk,
   the second virtio block function. */
typedef struct {
    FUNCTION block, input, data;
} FOUND;

/* The buses of one ECAM range, each mapped into init when first
   visited, or 0. */
typedef struct {
    const ECAM_RANGE *range;
    volatile UINT8 *buses[BUSES];
} SEGMENT;

static SEGMENT segment;

/* A program init supervises: what its log lines call it, its file, what
   it does once started, its function if it is a driver, and how it
   stands. While it runs, init holds its process, its job if it is a
   driver, and its end of the program's bootstrap channel, which init
   keeps for a client, and for a driver that reports until it has.
   starts numbers its starts, for its packets' keys; restarted is the
   time of its last restart, if it has had one. client_start, for a
   driver that reports, is the start of the client its endpoint went to
   since its own last start, or 0. */
typedef struct {
    const char *name;
    const char *file;
    UINT32 kind;
    FUNCTION *function;
    PCI_DEVICE_BOOTSTRAP device;
    UINT32 initiator;
    UINT32 state;
    UINT32 job, process, channel;
    UINT32 starts;
    UINT32 has_restarted;
    UINT64 restarted;
    UINT32 client_start;
} CHILD;

#define CHILD_BLOCK 0
#define CHILD_INPUT 1
#define CHILD_CLIENT 2
#define CHILDREN 3

static FOUND found_functions;
static CHILD children[CHILDREN] = {
    [CHILD_BLOCK] = {.name = "virtio block driver",
                     .file = BLOCK_FILE,
                     .kind = KIND_FINISHES,
                     .function = &found_functions.block},
    [CHILD_INPUT] = {.name = "virtio input driver",
                     .file = INPUT_FILE,
                     .kind = KIND_REPORTS,
                     .function = &found_functions.input},
    [CHILD_CLIENT] = {.name = "console",
                      .file = CLIENT_FILE,
                      .kind = KIND_CLIENT},
};
static UINT32 supervisor_port;

/* Map size bytes of device memory at physical, uncached, into init. */
static volatile UINT8 *map_device(const PLATFORM *platform, UINT64 physical,
                                  UINT64 size)
{
    UINT32 section;
    UINT8 *address;

    if (section_create_physical(platform->mmio, physical, size,
                                CACHE_UNCACHED, &section) != OK)
        return 0;
    address = map_into_init(section, size, MAP_READ | MAP_WRITE);
    handle_close(section);
    return address;
}

static void say_function(UINT32 requester, UINT32 vendor, UINT32 id)
{
    say("nova-init: pci ");
    pci_say_function(requester);
    say(" ");
    say_hex_width(vendor, 4);
    say(":");
    say_hex_width(id, 4);
    say("\r\n");
}

static void say_child(const char *before, const CHILD *child,
                      const char *after)
{
    say("nova-init: ");
    say(before);
    say(child->name);
    say(after);
}

static void say_status(const char *before, const CHILD *child,
                       const char *after, UINT32 status)
{
    say_child(before, child, after);
    say(", status ");
    say_number(status);
    say("\r\n");
}

/* Note the function in kind unless one is noted there already. */
static void note(FUNCTION *kind, UINT32 requester, UINT64 config,
                 volatile UINT8 *mapped)
{
    if (kind->config)
        return;
    kind->requester = requester;
    kind->config = config;
    kind->mapped = mapped;
}

/* Note a virtio endpoint function, by its PCI device ID, if a driver
   serves it. The boot disk's function seen again, as two ECAM ranges
   over one bus show it, is not the data disk. */
static void note_virtio(FOUND *found, UINT32 id, UINT32 requester,
                        UINT64 config, volatile UINT8 *mapped)
{
    int block =
        id == VIRTIO_BLK_PCI_DEVICE || id == VIRTIO_BLK_PCI_TRANSITIONAL;

    if (block && !found->block.config)
        note(&found->block, requester, config, mapped);
    else if (block && requester != found->block.requester)
        note(&found->data, requester, config, mapped);
    else if (id == VIRTIO_INPUT_PCI_DEVICE)
        note(&found->input, requester, config, mapped);
}

/* Visit bus and, through the bridges on it, the buses below, each once;
   log every function, and note the first of each kind in found. A
   device's functions above 0 are read only if function 0 is
   multi-function. */
static void scan_bus(const PLATFORM *platform, UINT32 bus, FOUND *found)
{
    const ECAM_RANGE *range = segment.range;
    UINT64 physical =
        range->base + ((UINT64)(bus - range->start_bus) << PCI_BUS_SHIFT);
    UINT32 device, function, functions, vendor, id, below;
    volatile UINT8 *window;

    if (bus < range->start_bus || bus > range->end_bus ||
        segment.buses[bus - range->start_bus])
        return;
    window = map_device(platform, physical, BUS_SIZE);
    if (!window) {
        say("nova-init: cannot map a bus's configuration space\r\n");
        return;
    }
    segment.buses[bus - range->start_bus] = window;
    for (device = 0; device < PCI_DEVICES; device++) {
        functions = 1;
        for (function = 0; function < functions; function++) {
            UINT64 offset = (UINT64)device << PCI_DEVICE_SHIFT |
                            (UINT64)function << PCI_FUNCTION_SHIFT;
            volatile UINT8 *config = window + offset;
            UINT32 requester =
                PCI_REQUESTER(range->segment, bus, device, function);
            UINT32 header;

            vendor = mmio_read16(config + PCI_VENDOR_ID);
            if (vendor == PCI_VENDOR_ABSENT)
                continue;
            header = mmio_read8(config + PCI_HEADER_TYPE);
            if (function == 0 && (header & PCI_HEADER_MULTIFUNCTION))
                functions = PCI_FUNCTIONS;
            id = mmio_read16(config + PCI_DEVICE_ID);
            say_function(requester, vendor, id);
            if (vendor == VIRTIO_PCI_VENDOR &&
                (header & PCI_HEADER_LAYOUT) == PCI_HEADER_ENDPOINT)
                note_virtio(found, id, requester, physical + offset, config);
            if ((header & PCI_HEADER_LAYOUT) == PCI_HEADER_BRIDGE) {
                below = mmio_read8(config + PCI_SECONDARY_BUS);
                if (below > bus)
                    scan_bus(platform, below, found);
            }
        }
    }
}

/* The walk, made once, by whichever of data.c and platform_start asks
   first. Each range starts with no bus visited; the windows of an
   earlier one stay mapped. The walk stops after the range in which the
   last kind is found. */
static void scan(const PLATFORM *platform, FOUND *found)
{
    const BOOTSTRAP_PLATFORM *boot = platform->boot;
    static int scanned;
    UINT32 i, bus;

    if (scanned)
        return;
    scanned = 1;
    for (i = 0; i < boot->ecam_count && i < BOOTSTRAP_ECAM_RANGES_MAX &&
                !(found->block.config && found->input.config &&
                  found->data.config);
         i++) {
        segment.range = &boot->ecam[i];
        for (bus = 0; bus < BUSES; bus++)
            segment.buses[bus] = 0;
        scan_bus(platform, segment.range->start_bus, found);
    }
}

/* Size each memory BAR by writing all ones and reading back which bits
   hold, with memory and I/O decoding off meanwhile, then restore it: the
   lowest address bit that holds is the size, a power of two. A 64-bit
   BAR takes two registers. Left out are a BAR of a width other than 32
   or 64 bits, one smaller than a page, since a resource covers whole
   pages and the rest of its page can hold another function's registers,
   one whose base is not a multiple of its size, and one firmware left
   unassigned at 0. */
static void size_bars(volatile UINT8 *config, PCI_DEVICE_BOOTSTRAP *device)
{
    UINT16 command = mmio_read16(config + PCI_COMMAND);
    UINT32 i, low, high;
    UINT64 base, mask, size;

    mmio_write16(config + PCI_COMMAND,
                 (UINT16)(command & ~(PCI_COMMAND_MEMORY | PCI_COMMAND_IO)));
    for (i = 0; i < PCI_BARS; i++) {
        volatile UINT8 *bar = config + PCI_BARS_OFFSET + i * PCI_BAR_SIZE;
        int wide;

        device->bar_base[i] = 0;
        device->bar_size[i] = 0;
        low = mmio_read32(bar);
        if ((low & PCI_BAR_IO) || ((low & PCI_BAR_WIDTH) != PCI_BAR_32 &&
                                   (low & PCI_BAR_WIDTH) != PCI_BAR_64))
            continue;
        wide = (low & PCI_BAR_WIDTH) == PCI_BAR_64 && i + 1 < PCI_BARS;
        mmio_write32(bar, ALL_ONES);
        mask = mmio_read32(bar) | (UINT64)ALL_ONES << 32;
        mmio_write32(bar, low);
        base = low;
        if (wide) {
            high = mmio_read32(bar + PCI_BAR_SIZE);
            mmio_write32(bar + PCI_BAR_SIZE, ALL_ONES);
            mask = (mask & ALL_ONES) |
                   (UINT64)mmio_read32(bar + PCI_BAR_SIZE) << 32;
            mmio_write32(bar + PCI_BAR_SIZE, high);
            base |= (UINT64)high << 32;
        }
        base &= PCI_BAR_ADDRESS;
        mask &= PCI_BAR_ADDRESS;
        size = mask & (~mask + 1);
        if (size >= PAGE_SIZE && base && base % size == 0) {
            device->bar_base[i] = base;
            device->bar_size[i] = size;
        }
        if (wide) {
            i++;
            device->bar_base[i] = 0;
            device->bar_size[i] = 0;
        }
    }
    mmio_write16(config + PCI_COMMAND, command);
}

/* The bootstrap record for function, with its BARs sized. */
static void describe(const FUNCTION *function, PCI_DEVICE_BOOTSTRAP *device)
{
    size_bars(function->mapped, device);
    device->requester = function->requester;
    device->reserved = 0;
    device->config = function->config;
}

/* The bootstrap record for child's function, and an initiator for its
   requester ID. Returns 0, logged, with nothing left open, if the
   initiator cannot be made. */
static int prepare(const PLATFORM *platform, CHILD *child)
{
    describe(child->function, &child->device);
    if (initiator_create(platform->dma, child->function->requester,
                         &child->initiator) == OK)
        return 1;
    say_child("cannot create the initiator for the ", child, "\r\n");
    return 0;
}

static void close_handles(const HANDLE_TRANSFER *handles, UINT32 count)
{
    UINT32 i;

    for (i = 0; i < count; i++)
        if (handles[i].handle != HANDLE_INVALID)
            handle_close(handles[i].handle);
}

int platform_data_disk(const PLATFORM *platform, PCI_DEVICE_BOOTSTRAP *device)
{
    scan(platform, &found_functions);
    if (!found_functions.data.config)
        return 0;
    describe(&found_functions.data, device);
    return 1;
}

UINT32 platform_driver_handles(const PLATFORM *platform,
                               const PCI_DEVICE_BOOTSTRAP *device,
                               UINT32 process, UINT32 initiator,
                               HANDLE_TRANSFER *handles)
{
    UINT32 count = PCI_HANDLE_BARS, i;

    for (i = 0; i < PCI_HANDLES_MAX; i++) {
        handles[i].handle = HANDLE_INVALID;
        handles[i].rights = RIGHTS_SAME;
    }
    if (resource_create(platform->debug_log, SYSTEM_PRIVILEGE_DEBUG_LOG, 1,
                        &handles[PCI_HANDLE_DEBUG_LOG].handle) != OK ||
        handle_duplicate(process, DRIVER_SELF_RIGHTS,
                         &handles[PCI_HANDLE_SELF].handle) != OK ||
        handle_duplicate(initiator, DRIVER_INITIATOR_RIGHTS,
                         &handles[PCI_HANDLE_INITIATOR].handle) != OK ||
        resource_create(platform->irq,
                        INTERRUPT_SOURCE_MSI + device->requester, 1,
                        &handles[PCI_HANDLE_IRQ].handle) != OK ||
        resource_create(platform->mmio, device->config, PCI_CONFIG_SIZE,
                        &handles[PCI_HANDLE_CONFIG].handle) != OK) {
        close_handles(handles, PCI_HANDLE_BARS);
        return 0;
    }
    for (i = 0; i < PCI_BARS; i++)
        if (device->bar_size[i] &&
            resource_create(platform->mmio, device->bar_base[i],
                            device->bar_size[i],
                            &handles[count++].handle) != OK) {
            close_handles(handles, count);
            return 0;
        }
    return count;
}

/* Log what the driver's resources cover, before it starts, so that the
   line comes whole ahead of the driver's own. */
static void say_start(const CHILD *child)
{
    const PCI_DEVICE_BOOTSTRAP *device = &child->device;
    UINT32 i;

    say_child("starting the ", child, " for ");
    pci_say_function(child->function->requester);
    say(", with its configuration space at ");
    say_hex(child->function->config);
    for (i = 0; i < PCI_BARS; i++)
        if (device->bar_size[i]) {
            say(", BAR ");
            say_number(i);
            say(" at ");
            say_hex(device->bar_base[i]);
            say(" (");
            say_number(device->bar_size[i]);
            say(" bytes)");
        }
    say(", its MSIs and an initiator with PIN\r\n");
}

/* The function's virtio common configuration structure (VIRTIO 1.4
   4.1.4.3), mapped uncached into init from the BAR its capability names,
   or 0 if it has none or the capability places it outside the BAR. */
static volatile UINT8 *map_common(const PLATFORM *platform,
                                  const FUNCTION *function,
                                  const PCI_DEVICE_BOOTSTRAP *device)
{
    UINT32 structures[VIRTIO_CAP_TYPES], msix, cap, index;
    UINT64 offset, first, end;
    volatile UINT8 *mapped;

    virtio_pci_capabilities(function->mapped, structures, &msix);
    cap = structures[VIRTIO_CAP_COMMON];
    if (!cap)
        return 0;
    index = mmio_read8(function->mapped + cap + VIRTIO_CAP_BAR);
    offset = mmio_read32(function->mapped + cap + VIRTIO_CAP_OFFSET);
    if (index >= PCI_BARS || offset > device->bar_size[index] ||
        VIRTIO_COMMON_SIZE > device->bar_size[index] - offset)
        return 0;
    first = offset - offset % PAGE_SIZE;
    end = (offset + VIRTIO_COMMON_SIZE + PAGE_SIZE - 1) / PAGE_SIZE *
          PAGE_SIZE;
    mapped = map_device(platform, device->bar_base[index] + first,
                        end - first);
    return mapped ? mapped + (offset - first) : 0;
}

/* Stop the function once its driver's process has terminated, so that
   nothing of the driver can turn bus mastering on again (ipc.md I10):
   clear Bus Master Enable, so it starts no more DMA, with Memory Space
   Enable set so its registers answer; reset the device by writing 0 to
   its device_status, after which it neither notifies nor touches its
   queues (VIRTIO 1.4 4.1.4.3, 2.4.1), so none of the pages they named is
   in use any more (3.3.1); and then release what the initiator holds in
   quarantine (ipc.md I8). Returns 1 if the function was stopped and the
   quarantine released; 0 if not, and the quarantine then stays. */
static int stop_function(const PLATFORM *platform, FUNCTION *function,
                         const PCI_DEVICE_BOOTSTRAP *device,
                         UINT32 initiator)
{
    UINT16 command = mmio_read16(function->mapped + PCI_COMMAND);
    UINT8 status;

    mmio_write16(function->mapped + PCI_COMMAND,
                 (UINT16)((command | PCI_COMMAND_MEMORY) &
                          ~PCI_COMMAND_BUS_MASTER));
    if (mmio_read16(function->mapped + PCI_COMMAND) &
        PCI_COMMAND_BUS_MASTER) {
        say("nova-init: bus mastering stays on; the initiator's quarantine "
            "stays\r\n");
        return 0;
    }
    if (!function->common)
        function->common = map_common(platform, function, device);
    if (!function->common) {
        say("nova-init: no virtio common configuration to reset the device "
            "through; the initiator's quarantine stays\r\n");
        return 0;
    }
    status = mmio_read8(function->common + VIRTIO_COMMON_DEVICE_STATUS);
    if (!virtio_pci_reset(function->common)) {
        say("nova-init: the device did not finish its reset; the "
            "initiator's quarantine stays\r\n");
        return 0;
    }
    if (initiator_release_quarantine(initiator) != OK)
        return 0;
    say("nova-init: bus mastering off, device reset from status ");
    say_hex(status);
    say(", initiator's quarantine released\r\n");
    return 1;
}

/* Whether child's file is on the boot volume; logged if not. */
static int have_file(const CHILD *child)
{
    if (boot_file(child->file))
        return 1;
    say("nova-init: no \\nova\\");
    say(child->file);
    say(", the ");
    say(child->name);
    say("\r\n");
    return 0;
}

/* Close *handle if it holds one, and leave HANDLE_INVALID there. */
static void close_if_open(UINT32 *handle)
{
    if (*handle != HANDLE_INVALID)
        handle_close(*handle);
    *handle = HANDLE_INVALID;
}

/* ------------------------------------------------------------------------
   Starting a child. Each start is watched before the process runs, so
   neither watch can fire at registration: its end through a watch of
   its process, and a driver's report through a watch of its bootstrap
   channel. Packets queue in the order their watches fire, and a driver
   writes its report before it can end, so a report always comes ahead
   of the end that may follow it. A watch that cannot be registered
   fails the start, and the process, never started, goes with its
   handle. */

/* Watch handle for signals through init's port, keyed with child's
   current start and event. Returns the status, logged if not OK. */
static UINT32 watch(const CHILD *child, UINT32 handle, UINT32 signals,
                    UINT32 event)
{
    UINT64 key = (UINT64)child->starts << KEY_START_SHIFT |
                 (UINT64)event << KEY_EVENT_SHIFT |
                 (UINT64)(child - children);
    UINT32 status =
        object_wait_async(handle, supervisor_port, key, signals);

    if (status != OK)
        say_status("cannot watch the ", child, "", status);
    return status;
}

UINT32 platform_driver_job(const PLATFORM *platform)
{
    UINT32 job;
    UINT32 status = job_create(platform->job, &driver_limits, &job);

    if (status == OK)
        return job;
    say("nova-init: cannot create a driver's job, status ");
    say_number(status);
    say("\r\n");
    return HANDLE_INVALID;
}

/* Start child, a driver, in a job of its own with its function's
   bootstrap message. Returns 1 with its job, its process and, for one
   that reports, init's end of its bootstrap channel in child; 0, logged,
   with nothing left open, if it did not start. */
static int start_driver(const PLATFORM *platform, CHILD *child)
{
    HANDLE_TRANSFER handles[PCI_HANDLES_MAX];
    PROGRAM program;
    UINT32 job = platform_driver_job(platform);
    UINT32 mine = HANDLE_INVALID, theirs = HANDLE_INVALID, count = 0;
    UINT32 status = ERR_NO_MEMORY;

    program.process = HANDLE_INVALID;
    program.thread = HANDLE_INVALID;
    if (job != HANDLE_INVALID)
        status = load_boot_file(job, boot_file(child->file), &program);
    if (status == OK) {
        count = platform_driver_handles(platform, &child->device,
                                        program.process, child->initiator,
                                        handles);
        status = count ? channel_create(CHANNEL_CAPACITY, &mine, &theirs)
                       : ERR_NO_MEMORY;
    }
    if (status == OK) {
        say_start(child);
        status = channel_write(mine, &child->device, sizeof child->device,
                               handles, count);
    }
    if (status != OK)
        close_handles(handles, count);
    if (status == OK && child->kind == KIND_REPORTS)
        status = watch(child, mine, CHANNEL_READABLE, EVENT_REPORTED);
    if (status == OK)
        status = watch(child, program.process, PROCESS_TERMINATED,
                       EVENT_ENDED);
    if (status == OK)
        status = program_start(&program, theirs);
    if (status != OK) {
        say_status("cannot start the ", child, "", status);
        close_if_open(&theirs);
        close_if_open(&mine);
        close_if_open(&program.thread);
        close_if_open(&program.process);
        close_if_open(&job);
        return 0;
    }
    child->job = job;
    child->process = program.process;
    if (child->kind == KIND_REPORTS)
        child->channel = mine;
    else
        handle_close(mine);
    return 1;
}

/* Start child, the input driver's client, the console, in the root job
   with the bootstrap message console.c writes, keeping init's end of the
   channel, on which its endpoints follow (input_protocol.h). Returns 1
   with its process and that end in child; 0, logged, with nothing left
   open, if it did not start. */
static int start_client(const PLATFORM *platform, CHILD *child)
{
    PROGRAM program;
    UINT32 mine = HANDLE_INVALID, theirs = HANDLE_INVALID;
    UINT32 status =
        load_boot_file(platform->job, boot_file(child->file), &program);

    if (status == OK)
        status = channel_create(CLIENT_CHANNEL_CAPACITY, &mine, &theirs);
    if (status == OK)
        status = console_bootstrap(platform, program.process, mine);
    if (status == OK)
        status = watch(child, program.process, PROCESS_TERMINATED,
                       EVENT_ENDED);
    if (status == OK)
        status = program_start(&program, theirs);
    if (status != OK) {
        say_status("cannot start the ", child, "", status);
        close_if_open(&theirs);
        close_if_open(&mine);
        close_if_open(&program.thread);
        close_if_open(&program.process);
        return 0;
    }
    child->process = program.process;
    child->channel = mine;
    console_started(platform);
    return 1;
}

/* ------------------------------------------------------------------------
   The restart policy. */

/* Whether the restart policy lets child be restarted now; if it does,
   the restart is counted. */
static int restart_allowed(CHILD *child)
{
    UINT64 now;

    if (clock_monotonic(&now) != OK ||
        (child->has_restarted && now - child->restarted < RESTART_PERIOD_NS))
        return 0;
    child->has_restarted = 1;
    child->restarted = now;
    return 1;
}

/* Leave child down for good, with its initiator closed: a driver's
   function is stopped already or keeps its quarantine (ipc.md I10). A
   driver that finishes is one of init's checks, which fails without
   it; the system runs on without any other. */
static void give_up(CHILD *child, const char *why)
{
    child->state = STATE_GIVEN_UP;
    if (child->initiator != HANDLE_INVALID) {
        handle_close(child->initiator);
        child->initiator = HANDLE_INVALID;
    }
    say_child("the ", child, " is given up: ");
    say(why);
    say(child->kind == KIND_FINISHES ? "\r\n"
                                     : "; the system runs on without it\r\n");
}

/* Stop child's function, if it is a driver, once its process has ended
   or did not start. Returns 1 if there is none or it stopped; 0, with
   the child given up, if not. */
static int stopped(const PLATFORM *platform, CHILD *child)
{
    if (!child->function ||
        stop_function(platform, child->function, &child->device,
                      child->initiator))
        return 1;
    give_up(child, "its function did not stop");
    return 0;
}

/* Whether child, ended or not started and its function stopped, may
   start again; logged either way, and given up if not. */
static int restarting(CHILD *child)
{
    if (!restart_allowed(child)) {
        say_child("the ", child, " is down again within ");
        say_number(RESTART_PERIOD_SECONDS);
        say(" s of its restart\r\n");
        give_up(child, "a restart does not keep it up");
        return 0;
    }
    say_child("restarting the ", child, "\r\n");
    return 1;
}

/* Start child, and again while it does not start and the policy allows.
   A driver that reports is awaited, one that finishes too; a client
   runs. */
static void launch(const PLATFORM *platform, CHILD *child)
{
    for (;;) {
        child->starts++;
        child->client_start = 0;
        if (child->kind == KIND_CLIENT ? start_client(platform, child)
                                       : start_driver(platform, child)) {
            child->state = child->kind == KIND_CLIENT ? STATE_RUNNING
                                                      : STATE_STARTING;
            return;
        }
        if (!stopped(platform, child) || !restarting(child))
            return;
    }
}

/* ------------------------------------------------------------------------
   What init does when a packet comes. */

/* The exit code of child's process, which has ended, logged. */
static UINT32 exit_code_of(const CHILD *child)
{
    UINT32 state = 0, exit_code = 0;
    UINT32 status = process_info(child->process, &state, &exit_code);

    if (status != OK) {
        say_status("cannot read the end of the ", child, "", status);
        return 0;
    }
    say_child("the ", child, " ended, exit code ");
    say_number(exit_code);
    say("\r\n");
    return exit_code;
}

/* child's process has ended. A driver's function is stopped first,
   whatever the end; the block driver's end with VIRTIO_BLK_EXIT_DONE is
   the end of its work, whose function keeps its quarantine if it does
   not stop, as any driver's does; the input driver is not started again
   for a client given up, which would leave each endpoint it reports
   with nobody to read it. The input driver's end with
   VIRTIO_INPUT_EXIT_DONE once the client it served has been restarted
   is the client's failure, not the driver's, and the driver starts
   again without counting a restart; the client's own policy bounds how
   often that comes. Any other child starts again as the policy
   allows. */
static void ended(const PLATFORM *platform, CHILD *child)
{
    UINT32 exit_code = exit_code_of(child);

    close_if_open(&child->process);
    close_if_open(&child->job);
    close_if_open(&child->channel);
    if (child->kind == KIND_FINISHES && exit_code == VIRTIO_BLK_EXIT_DONE) {
        stop_function(platform, child->function, &child->device,
                      child->initiator);
        child->state = STATE_DONE;
        close_if_open(&child->initiator);
        return;
    }
    if (!stopped(platform, child))
        return;
    if (child->kind == KIND_REPORTS &&
        children[CHILD_CLIENT].state == STATE_GIVEN_UP) {
        give_up(child, "its client is given up");
        return;
    }
    child->state = STATE_STOPPED;
    if (child->kind == KIND_REPORTS && exit_code == VIRTIO_INPUT_EXIT_DONE &&
        child->client_start &&
        child->client_start != children[CHILD_CLIENT].starts) {
        say_child("restarting the ", child, " for its restarted client\r\n");
        launch(platform, child);
    } else if (restarting(child)) {
        launch(platform, child);
    }
}

/* The client's endpoint from the driver's report, read from channel, if
   the report is the one virtio_input.h gives; else HANDLE_INVALID,
   logged, with any handle it carried closed. */
static UINT32 take_report(UINT32 channel)
{
    RECEIVED_HANDLE handle;
    UINT32 transaction, byte_count, handle_count;
    UINT32 status = channel_read(channel, &transaction, sizeof transaction,
                                 &handle, 1, &byte_count, &handle_count);

    if (status == OK && byte_count == VIRTIO_INPUT_REPORT_SIZE &&
        transaction == 0 && handle_count == 1 &&
        handle.type == TYPE_CHANNEL &&
        handle.rights == VIRTIO_INPUT_CLIENT_RIGHTS)
        return handle.handle;
    if (status == OK && handle_count)
        handle_close(handle.handle);
    say("nova-init: the virtio input driver's report is malformed, "
        "status ");
    say_number(status);
    say("\r\n");
    return HANDLE_INVALID;
}

/* Hand the client endpoint, from driver, which then is the client's or,
   if the client cannot take it, closed, so that the driver sees its
   client gone. */
static void hand_endpoint(CHILD *driver, UINT32 endpoint)
{
    CHILD *client = &children[CHILD_CLIENT];
    HANDLE_TRANSFER transfer;
    UINT32 transaction = 0;
    UINT32 status = ERR_BAD_STATE;

    transfer.handle = endpoint;
    transfer.rights = CLIENT_ENDPOINT_RIGHTS;
    if (client->state == STATE_RUNNING)
        status = channel_write(client->channel, &transaction,
                               INPUT_CLIENT_MESSAGE_SIZE, &transfer, 1);
    if (status == OK) {
        driver->client_start = client->starts;
        return;
    }
    handle_close(endpoint);
    say_status("the endpoint reached no ", client, "", status);
}

/* child, the input driver, has reported: its endpoint goes to its
   client, which is started the first time. */
static void reported(const PLATFORM *platform, CHILD *child)
{
    CHILD *client = &children[CHILD_CLIENT];
    UINT32 endpoint = take_report(child->channel);

    close_if_open(&child->channel);
    child->state = STATE_RUNNING;
    if (endpoint == HANDLE_INVALID)
        return;
    if (client->state == STATE_STOPPED) {
        say_child("the ", child, " reported; starting its client, \\nova\\");
        say(client->file);
        say("\r\n");
        launch(platform, client);
    } else if (client->state == STATE_RUNNING) {
        say_child("the ", child, " reported; its client takes the new "
                  "endpoint\r\n");
    }
    hand_endpoint(child, endpoint);
}

/* A packet from a watch of a start before the child's current one finds
   that start's handles closed already, and is passed over. */
static void handle_packet(const PLATFORM *platform,
                          const PORT_PACKET *packet)
{
    UINT32 index = (UINT32)(packet->key & KEY_FIELD_MASK);
    UINT32 event = (UINT32)(packet->key >> KEY_EVENT_SHIFT & KEY_FIELD_MASK);
    CHILD *child;

    if (packet->type != PACKET_SIGNAL || index >= CHILDREN) {
        say("nova-init: an unexpected packet on the supervisor's port\r\n");
        return;
    }
    child = &children[index];
    if (packet->key >> KEY_START_SHIFT != child->starts)
        return;
    if (event == EVENT_REPORTED && child->channel != HANDLE_INVALID)
        reported(platform, child);
    else if (event == EVENT_ENDED && child->process != HANDLE_INVALID)
        ended(platform, child);
}

/* Take the packets on init's port as they come until child is no
   longer starting. Returns OK, or port_wait's status if it fails. */
static UINT32 supervise_start(const PLATFORM *platform, const CHILD *child)
{
    PORT_PACKET packet;
    UINT32 status = OK;

    while (status == OK && child->state == STATE_STARTING) {
        status = port_wait(supervisor_port, DEADLINE_INFINITE, &packet);
        if (status == OK)
            handle_packet(platform, &packet);
    }
    return status;
}

/* Start child, a driver, if its function and file are there, and wait
   until it has done what it was started for, or is given up. A missing
   function or file leaves it stopped, logged. */
static UINT32 start_up(const PLATFORM *platform, CHILD *child)
{
    if (!child->function->config) {
        say_child("no function for the ", child, "\r\n");
        return OK;
    }
    if (!have_file(child))
        return OK;
    if (child->kind == KIND_REPORTS && !have_file(&children[CHILD_CLIENT]))
        return OK;
    if (!prepare(platform, child)) {
        child->state = STATE_GIVEN_UP;
        return OK;
    }
    launch(platform, child);
    return supervise_start(platform, child);
}

int platform_start(const PLATFORM *platform)
{
    CHILD *block = &children[CHILD_BLOCK];
    UINT32 status = port_create(SUPERVISOR_PORT_CAPACITY, &supervisor_port);

    scan(platform, &found_functions);
    if (status == OK)
        status = start_up(platform, block);
    if (status == OK && block->state == STATE_GIVEN_UP)
        return 0;
    if (status == OK)
        status = start_up(platform, &children[CHILD_INPUT]);
    if (status == OK)
        return 1;
    say("nova-init: cannot supervise, status ");
    say_number(status);
    say("\r\n");
    return 0;
}

UINT32 platform_supervise(const PLATFORM *platform)
{
    PORT_PACKET packet;
    UINT32 status, i;

    for (i = 0; i < CHILDREN; i++)
        if (children[i].state == STATE_RUNNING)
            say_child("supervising the ", &children[i], "\r\n");
    for (;;) {
        status = port_wait(supervisor_port, DEADLINE_INFINITE, &packet);
        if (status != OK)
            return status;
        handle_packet(platform, &packet);
    }
}
