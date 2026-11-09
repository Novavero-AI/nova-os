/* The platform driver. It walks PCI configuration space through the ECAM
   ranges the kernel found in the ACPI MCFG table, from each range's
   first bus down through the bridges, mapping each bus it visits once,
   and logs each function. Each function named below gets a driver, a
   process of its own in a job of its own, whose bootstrap message
   (pci.h) carries resources covering only that function: its
   configuration space, its memory BARs, its message-signalled
   interrupts and an initiator for its requester ID, with PIN alone.
   init keeps the initiator's RELEASE.

   On a test disk the first virtio block function goes to
   \nova\virtblk.exe, which reads the boot disk and ends, and init
   reports its end as the boot-disk test (tests.h); on any other disk it
   gets no driver. The walk notes the second as the data disk, whose
   driver and file system service data.c starts, with this file's BAR
   sizing and driver handles. Each virtio input function, up to
   INPUT_DEVICES_MAX, goes to a \nova\virtinp.exe of its own, which runs
   on: once its device is live it reports with an endpoint, which init
   hands to their client, the console, \nova\console.exe, whose bootstrap
   message console.c writes (docs/spec/input.md, docs/spec/console.md); or,
   where the first process starts a compositor, \nova\compose.exe,
   whose bootstrap message compositor.c writes, which is their client
   instead until it is given up, and then the console is
   (docs/spec/display.md D2, D3).

   init supervises the drivers and the console for the life of the
   system (ipc.md I10), and the programs it keeps running that are not
   drivers, the accounts service and login among them (platform_keep).
   The compositor is supervised as the console is.
   It waits for them through one port, on which a watch of each one's
   process queues a packet when it ends, and a watch of each input
   driver's bootstrap channel one when it reports. One that ends, or does
   not start, is started again as the restart policy below allows, a
   driver once init has stopped its function: cleared Bus Master Enable,
   reset the device and released the initiator's quarantine. The block
   driver's end with VIRTIO_BLK_EXIT_DONE is the end of its work, and it
   is not started again. The client keeps its bootstrap channel to init,
   which hands it each endpoint an input driver reports: the first, and
   one after each restart of the driver. A driver whose client ends, or
   is replaced by the console when the compositor is given up, sees its
   endpoint close, ends, and is started again for the client it now has,
   without counting a restart. */

#include "platform.h"
#include "console.h"
#include "compositor.h"
#include "init.h"
#include "tests.h"
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
#define COMPOSITOR_FILE "compose.exe"
#define INPUT_NAME "virtio-input"

/* The hexadecimal digits of a PCI vendor or device ID. */
#define PCI_ID_DIGITS 4

/* The rights the driver's handles carry: its own process with MAP, to
   map its sections, and the initiator with PIN, so that releasing the
   quarantine stays with the supervisor. */
#define DRIVER_SELF_RIGHTS (PROCESS_RIGHT_MAP | RIGHT_TRANSFER)
#define DRIVER_INITIATOR_RIGHTS (INITIATOR_RIGHT_PIN | RIGHT_TRANSFER)

/* The rights a client's endpoint travels with: it reads key events or
   pointer reports and waits for them, and passes the endpoint nowhere. */
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
   endpoints it has yet to take: for each input driver, the one from its
   start and the one from the restart the policy allows within
   RESTART_PERIOD_SECONDS, should the client take neither meanwhile. */
#define CLIENT_CHANNEL_CAPACITY (1 + 2 * INPUT_DEVICES_MAX)

/* A packet's key: the child's index in bits 7:0, what the watch waits
   for in bits 15:8, and the child's start number from bit 16 on, so a
   packet from a watch of an earlier start is told apart. */
#define KEY_EVENT_SHIFT 8
#define KEY_START_SHIFT 16
#define KEY_FIELD_MASK 0xff
#define EVENT_ENDED 0
#define EVENT_REPORTED 1

/* What a child does once started: a driver that does its work and ends,
   one that reports with an endpoint and runs on, the client of those,
   or a program kept running that drives nothing. */
#define KIND_FINISHES 0
#define KIND_REPORTS 1
#define KIND_CLIENT 2
#define KIND_PROGRAM 3

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

/* The first virtio block function, the data disk, the second, and the
   virtio input functions, inputs of them, in the order of the walk. */
typedef struct {
    FUNCTION block, data;
    FUNCTION input[INPUT_DEVICES_MAX];
    UINT32 inputs;
} FOUND;

/* The buses of one ECAM range, each mapped into init when first
   visited, or 0. */
typedef struct {
    const ECAM_RANGE *range;
    volatile UINT8 *buses[BUSES];
} SEGMENT;

static SEGMENT segment;

/* A program init supervises: the name its log lines give it, its file, what
   it does once started, its function if it is a driver, and how it
   stands. While it runs, init holds its process, its job if it is a
   driver, and its end of the program's bootstrap channel, which init
   keeps for a client, and for a driver that reports until it has.
   starts numbers its starts, for its packets' keys; restarted is the
   time of its last restart, if it has had one. serial, for a client,
   numbers its starts among every client's, so that a start is told from
   another's whichever client it is. client and client_start, for a
   driver that reports, are the client its endpoint went to since its own
   last start and that client's serial then, or 0; held is an endpoint
   it reported that no client has taken yet, since the compositor is not
   started. A program kept running is laid out by its load; a client is
   given its bootstrap message by its bootstrap and told it has started
   by its started. */
typedef UINT32 (*CLIENT_BOOTSTRAP)(const PLATFORM *platform, UINT32 process,
                                   UINT32 channel);
typedef void (*CLIENT_STARTED)(const PLATFORM *platform);

typedef struct child {
    const char *name;
    const char *file;
    UINT32 kind;
    PLATFORM_LOAD load;
    FUNCTION *function;
    PCI_DEVICE_BOOTSTRAP device;
    UINT32 initiator;
    UINT32 state;
    UINT32 job, process, channel;
    UINT32 starts;
    UINT32 has_restarted;
    UINT64 restarted;
    UINT32 serial;
    CLIENT_BOOTSTRAP bootstrap;
    CLIENT_STARTED started;
    const struct child *client;
    UINT32 client_start;
    UINT32 held;
} CHILD;

/* The block driver, the console and the compositor take the first
   places, then an input driver for each input function the walk found.
   The programs platform_keep keeps running take the places after
   theirs: the accounts service and login. */
#define CHILD_BLOCK 0
#define CHILD_CLIENT 1
#define CHILD_COMPOSITOR 2
#define CHILD_FIRST_INPUT 3
#define CHILD_FIRST_PROGRAM (CHILD_FIRST_INPUT + INPUT_DEVICES_MAX)
#define PROGRAMS_MAX 2
#define CHILDREN (CHILD_FIRST_PROGRAM + PROGRAMS_MAX)

_Static_assert(CHILDREN <= KEY_FIELD_MASK + 1,
               "a packet's key names every child");

static FOUND found_functions;
static CHILD children[CHILDREN] = {
    [CHILD_BLOCK] = {.name = "virtio-blk",
                     .file = BLOCK_FILE,
                     .kind = KIND_FINISHES,
                     .function = &found_functions.block},
    [CHILD_CLIENT] = {.name = "console",
                      .file = CLIENT_FILE,
                      .kind = KIND_CLIENT,
                      .bootstrap = console_bootstrap,
                      .started = console_started},
    [CHILD_COMPOSITOR] = {.name = "compositor",
                          .file = COMPOSITOR_FILE,
                          .kind = KIND_CLIENT,
                          .bootstrap = compositor_bootstrap,
                          .started = compositor_started},
};
static UINT32 supervisor_port;

/* The starts of every client counted: a client's serial is its start's
   place in the count. */
static UINT32 client_starts;

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
    say("nova-init: function pci=");
    pci_say_function(requester);
    say(" id=");
    say_hex_width(vendor, PCI_ID_DIGITS);
    say(":");
    say_hex_width(id, PCI_ID_DIGITS);
    say("\r\n");
}

/* The start of a line about child: event, its name, and a driver's
   function, if the walk found one, so that the input drivers are told
   apart. The caller writes any other keys and ends the line. */
static void say_child(const char *event, const CHILD *child)
{
    say("nova-init: ");
    say(event);
    say(" name=");
    say(child->name);
    if (child->function && child->function->config) {
        say(" pci=");
        pci_say_function(child->function->requester);
    }
}

static void say_child_line(const char *event, const CHILD *child)
{
    say_child(event, child);
    say("\r\n");
}

static void say_child_status(const char *event, const CHILD *child,
                             UINT32 status)
{
    say_child(event, child);
    say(" status=");
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

/* Note an input function after those noted, unless it is one of them
   seen again, as two ECAM ranges over one bus show it, or there are
   INPUT_DEVICES_MAX already, which is logged. */
static void note_input(FOUND *found, UINT32 requester, UINT64 config,
                       volatile UINT8 *mapped)
{
    UINT32 i;

    for (i = 0; i < found->inputs; i++)
        if (found->input[i].requester == requester)
            return;
    if (found->inputs == INPUT_DEVICES_MAX) {
        say("nova-init: child refused name=" INPUT_NAME " pci=");
        pci_say_function(requester);
        say(" max=");
        say_number(INPUT_DEVICES_MAX);
        say("\r\n");
        return;
    }
    note(&found->input[found->inputs++], requester, config, mapped);
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
        note_input(found, requester, config, mapped);
}

/* Visit bus and, through the bridges on it, the buses below, each once;
   log every function, and note in found those a driver serves. A
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
        say("nova-init: pci map failed bus=");
        say_number(bus);
        say("\r\n");
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
   last kind is found, every input function the drivers serve
   included. */
static void scan(const PLATFORM *platform, FOUND *found)
{
    const BOOTSTRAP_PLATFORM *boot = platform->boot;
    static int scanned;
    UINT32 i, bus;

    if (scanned)
        return;
    scanned = 1;
    for (i = 0; i < boot->ecam_count && i < BOOTSTRAP_ECAM_RANGES_MAX &&
                !(found->block.config && found->data.config &&
                  found->inputs == INPUT_DEVICES_MAX);
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
    say_child_line("initiator create failed", child);
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
static void say_stop_failed(const FUNCTION *function, const char *reason)
{
    say("nova-init: function stop failed pci=");
    pci_say_function(function->requester);
    say(" reason=");
    say_value(reason);
    say("\r\n");
}

static int stop_function(const PLATFORM *platform, FUNCTION *function,
                         const PCI_DEVICE_BOOTSTRAP *device,
                         UINT32 initiator)
{
    UINT16 command = mmio_read16(function->mapped + PCI_COMMAND);

    mmio_write16(function->mapped + PCI_COMMAND,
                 (UINT16)((command | PCI_COMMAND_MEMORY) &
                          ~PCI_COMMAND_BUS_MASTER));
    if (mmio_read16(function->mapped + PCI_COMMAND) &
        PCI_COMMAND_BUS_MASTER) {
        say_stop_failed(function, "bus mastering stays on");
        return 0;
    }
    if (!function->common)
        function->common = map_common(platform, function, device);
    if (!function->common) {
        say_stop_failed(function, "no virtio common configuration");
        return 0;
    }
    if (!virtio_pci_reset(function->common)) {
        say_stop_failed(function, "the device did not finish its reset");
        return 0;
    }
    if (initiator_release_quarantine(initiator) != OK) {
        say_stop_failed(function, "the quarantine was not released");
        return 0;
    }
    return 1;
}

/* Whether child's file is on the boot volume; logged if not. */
static int have_file(const CHILD *child)
{
    if (boot_file(child->file))
        return 1;
    say_child("child absent", child);
    say(" file=");
    say_value(child->file);
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
        say_child_status("child watch failed", child, status);
    return status;
}

UINT32 platform_driver_job(const PLATFORM *platform)
{
    UINT32 job;
    UINT32 status = job_create(platform->job, &driver_limits, &job);

    if (status == OK)
        return job;
    say_status("driver job create failed", status);
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
    if (status == OK)
        status = channel_write(mine, &child->device, sizeof child->device,
                               handles, count);
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
        say_child_status("child start failed", child, status);
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

/* Start child, a client of the input drivers, the console or the
   compositor, in the root job with the bootstrap message its bootstrap
   writes, keeping init's end of the channel, on which its endpoints
   follow (input_protocol.h). Returns 1 with its process and that end in
   child, once its started has run; 0, logged, with nothing left open, if
   it did not start. */
static int start_client(const PLATFORM *platform, CHILD *child)
{
    PROGRAM program;
    UINT32 mine = HANDLE_INVALID, theirs = HANDLE_INVALID;
    UINT32 status =
        load_boot_file(platform->job, boot_file(child->file), &program);

    if (status == OK)
        status = channel_create(CLIENT_CHANNEL_CAPACITY, &mine, &theirs);
    if (status == OK)
        status = child->bootstrap(platform, program.process, mine);
    if (status == OK)
        status = watch(child, program.process, PROCESS_TERMINATED,
                       EVENT_ENDED);
    if (status == OK)
        status = program_start(&program, theirs);
    if (status != OK) {
        say_child_status("child start failed", child, status);
        close_if_open(&theirs);
        close_if_open(&mine);
        close_if_open(&program.thread);
        close_if_open(&program.process);
        return 0;
    }
    child->process = program.process;
    child->channel = mine;
    child->started(platform);
    return 1;
}

/* Start child, a program kept running, as its load lays it out.
   Returns 1 with its process in child; 0, logged, with nothing left
   open, if it did not start. */
static int start_program(const PLATFORM *platform, CHILD *child)
{
    PROGRAM program;
    UINT32 bootstrap = HANDLE_INVALID;
    UINT32 status = child->load(platform, &program, &bootstrap);

    if (status == OK) {
        status = watch(child, program.process, PROCESS_TERMINATED,
                       EVENT_ENDED);
        if (status == OK)
            status = program_start(&program, bootstrap);
        if (status != OK) {
            close_if_open(&bootstrap);
            close_if_open(&program.thread);
            close_if_open(&program.process);
        }
    }
    if (status != OK) {
        say_child_status("child start failed", child, status);
        return 0;
    }
    child->process = program.process;
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
   function is stopped already or keeps its quarantine (ipc.md I10). The
   system runs on without it; a driver that finishes, the boot disk's
   reader, then fails the boot-disk test. */
static void give_up(CHILD *child, const char *why)
{
    child->state = STATE_GIVEN_UP;
    if (child->initiator != HANDLE_INVALID) {
        handle_close(child->initiator);
        child->initiator = HANDLE_INVALID;
    }
    say_child("child given up", child);
    say(" reason=");
    say_value(why);
    say("\r\n");
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

/* The child that takes the input drivers' endpoints: the compositor,
   from the start of a boot that is to run one until it is given up, and
   the console otherwise (display.md D2, D3). */
static CHILD *input_client(const PLATFORM *platform)
{
    CHILD *compositor = &children[CHILD_COMPOSITOR];

    if (platform->drawer == DRAWER_COMPOSITOR &&
        compositor->state != STATE_GIVEN_UP)
        return compositor;
    return &children[CHILD_CLIENT];
}

/* Hand the client endpoint, from driver, which then is the client's or,
   if the client cannot take it, closed, so that the driver sees its
   client gone. A client not started yet, the compositor before the
   release, takes none: driver keeps the endpoint until it is. */
static void hand_endpoint(const PLATFORM *platform, CHILD *driver,
                          UINT32 endpoint)
{
    CHILD *client = input_client(platform);
    HANDLE_TRANSFER transfer;
    UINT32 transaction = 0;
    UINT32 status = ERR_BAD_STATE;

    if (client->state == STATE_STOPPED) {
        driver->held = endpoint;
        return;
    }
    transfer.handle = endpoint;
    transfer.rights = CLIENT_ENDPOINT_RIGHTS;
    if (client->state == STATE_RUNNING)
        status = channel_write(client->channel, &transaction,
                               INPUT_CLIENT_MESSAGE_SIZE, &transfer, 1);
    if (status == OK) {
        driver->client = client;
        driver->client_start = client->serial;
        return;
    }
    handle_close(endpoint);
    say_child_status("endpoint send failed", client, status);
}

/* Hand each input driver's held endpoint to the client, in the order of
   the drivers, which is the order they reported in. */
static void hand_held(const PLATFORM *platform)
{
    UINT32 i;

    for (i = CHILD_FIRST_INPUT; i < CHILD_FIRST_PROGRAM; i++) {
        CHILD *driver = &children[i];
        UINT32 endpoint = driver->held;

        if (endpoint != HANDLE_INVALID) {
            driver->held = HANDLE_INVALID;
            hand_endpoint(platform, driver, endpoint);
        }
    }
}

/* child, a client, has been given up. The compositor's end leaves the
   console the input drivers' client and the display's holder: the
   console is sent the display after a compositor, and the endpoints no
   client has taken go to it. The drivers whose endpoints the compositor
   held end by themselves, and are started again for the console (ended). */
static void client_given_up(const PLATFORM *platform, const CHILD *child)
{
    if (child != &children[CHILD_COMPOSITOR])
        return;
    compositor_given_up();
    console_display_after(platform);
    hand_held(platform);
}

/* Whether child, ended or not started and its function stopped, may
   start again; logged either way, and given up if not, a client given
   up with what follows from it. */
static int restarting(const PLATFORM *platform, CHILD *child)
{
    if (!restart_allowed(child)) {
        give_up(child, "down again within the restart period");
        if (child->kind == KIND_CLIENT)
            client_given_up(platform, child);
        return 0;
    }
    say_child_line("child restarted", child);
    return 1;
}

/* Start child, and again while it does not start and the policy allows.
   A driver that reports is awaited, one that finishes too; a client and
   a program kept running run. */
static void launch(const PLATFORM *platform, CHILD *child)
{
    int started;

    for (;;) {
        child->starts++;
        child->client = 0;
        child->client_start = 0;
        if (child->kind == KIND_CLIENT)
            child->serial = ++client_starts;
        say_child_line("child start", child);
        if (child->kind == KIND_CLIENT)
            started = start_client(platform, child);
        else if (child->kind == KIND_PROGRAM)
            started = start_program(platform, child);
        else
            started = start_driver(platform, child);
        if (started) {
            child->state = child->kind == KIND_CLIENT ||
                                   child->kind == KIND_PROGRAM
                               ? STATE_RUNNING
                               : STATE_STARTING;
            return;
        }
        if (!stopped(platform, child) || !restarting(platform, child))
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
        say_child_status("child end read failed", child, status);
        return 0;
    }
    say_child("child ended", child);
    say(" exit=");
    say_number(exit_code);
    say("\r\n");
    return exit_code;
}

/* Why driver, an input driver that ended with VIRTIO_INPUT_EXIT_DONE,
   is started again without counting a restart: the client its endpoint
   went to has been started again, or has been replaced by another; or 0
   if neither, and the driver's end is its own. */
static const char *client_change(const PLATFORM *platform,
                                 const CHILD *driver)
{
    const CHILD *client = input_client(platform);

    if (!driver->client || driver->client_start == client->serial)
        return 0;
    return driver->client == client ? "its client restarted"
                                    : "its client changed";
}

/* child's process has ended. A driver's function is stopped first,
   whatever the end; the block driver's end with VIRTIO_BLK_EXIT_DONE is
   the end of its work, whose function keeps its quarantine if it does
   not stop, as any driver's does; an input driver is not started again
   for a client given up, which would leave each endpoint it reports
   with nobody to read it. An input driver's end with
   VIRTIO_INPUT_EXIT_DONE once the client it served has been restarted
   or replaced is the client's change, not the driver's failure, and the
   driver starts again without counting a restart; the client's own
   policy bounds how often that comes. Any other child starts again as
   the policy allows. */
static void ended(const PLATFORM *platform, CHILD *child)
{
    UINT32 exit_code = exit_code_of(child);
    const char *change = 0;

    close_if_open(&child->process);
    close_if_open(&child->job);
    close_if_open(&child->channel);
    close_if_open(&child->held);
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
        input_client(platform)->state == STATE_GIVEN_UP) {
        give_up(child, "its client is given up");
        return;
    }
    child->state = STATE_STOPPED;
    if (child->kind == KIND_REPORTS && exit_code == VIRTIO_INPUT_EXIT_DONE)
        change = client_change(platform, child);
    if (change) {
        say_child("child restarted", child);
        say(" reason=");
        say_value(change);
        say("\r\n");
        launch(platform, child);
    } else if (restarting(platform, child)) {
        launch(platform, child);
    }
}

/* The client's endpoint from the report of driver, read from its
   bootstrap channel, if the report is the one virtio_input.h gives; else
   HANDLE_INVALID, logged, with any handle it carried closed. */
static UINT32 take_report(const CHILD *driver)
{
    RECEIVED_HANDLE handle;
    UINT32 transaction, byte_count, handle_count;
    UINT32 status =
        channel_read(driver->channel, &transaction, sizeof transaction,
                     &handle, 1, &byte_count, &handle_count);

    if (status == OK && byte_count == VIRTIO_INPUT_REPORT_SIZE &&
        transaction == 0 && handle_count == 1 &&
        handle.type == TYPE_CHANNEL &&
        handle.rights == VIRTIO_INPUT_CLIENT_RIGHTS)
        return handle.handle;
    if (status == OK && handle_count)
        handle_close(handle.handle);
    say_child_status("report refused", driver, status);
    return HANDLE_INVALID;
}

/* child, an input driver, has reported: its endpoint goes to the
   client, which the first report of any input driver starts where the
   client is the console; the compositor starts at the release
   (platform_start_compositor), and the endpoints wait for it. */
static void reported(const PLATFORM *platform, CHILD *child)
{
    CHILD *client = input_client(platform);
    UINT32 endpoint = take_report(child);

    close_if_open(&child->channel);
    child->state = STATE_RUNNING;
    if (endpoint == HANDLE_INVALID)
        return;
    if (client->state == STATE_STOPPED && platform->drawer == DRAWER_CONSOLE)
        launch(platform, client);
    hand_endpoint(platform, child, endpoint);
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
        say("nova-init: packet refused\r\n");
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
        say_child("child absent", child);
        say(" reason=\"no function\"\r\n");
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

/* The input driver for the walk's input function index. */
static CHILD *input_child(UINT32 index)
{
    CHILD *child = &children[CHILD_FIRST_INPUT + index];

    child->name = INPUT_NAME;
    child->file = INPUT_FILE;
    child->kind = KIND_REPORTS;
    child->function = &found_functions.input[index];
    return child;
}

/* The boot-disk test: the block driver read the boot disk and ended
   with VIRTIO_BLK_EXIT_DONE, or it never ran. */
static void report_boot_disk(const CHILD *block)
{
    if (block->state == STATE_DONE)
        tests_report(INIT_TEST_BOOT_DISK, KTAP_PASSED, 0);
    else if (!block->function->config)
        tests_report(INIT_TEST_BOOT_DISK, KTAP_SKIPPED,
                     "no virtio block function");
    else
        tests_report(INIT_TEST_BOOT_DISK, KTAP_FAILED,
                     "the driver did not finish its read");
}

int platform_start(const PLATFORM *platform)
{
    CHILD *block = &children[CHILD_BLOCK];
    UINT32 status = port_create(SUPERVISOR_PORT_CAPACITY, &supervisor_port);
    UINT32 i;

    scan(platform, &found_functions);
    if (status == OK && tests_enabled()) {
        status = start_up(platform, block);
        if (status == OK)
            report_boot_disk(block);
    }
    if (status == OK && !found_functions.inputs)
        say("nova-init: child absent name=" INPUT_NAME
            " reason=\"no function\"\r\n");
    for (i = 0; status == OK && i < found_functions.inputs; i++)
        status = start_up(platform, input_child(i));
    if (status == OK)
        return 1;
    say_status("supervision failed", status);
    return 0;
}

void platform_start_compositor(const PLATFORM *platform)
{
    CHILD *console = &children[CHILD_CLIENT];

    launch(platform, &children[CHILD_COMPOSITOR]);
    hand_held(platform);
    if (console->state == STATE_STOPPED && have_file(console))
        launch(platform, console);
    hand_held(platform);
}

int platform_keep(const PLATFORM *platform, const char *name,
                  PLATFORM_LOAD load)
{
    CHILD *child = 0;
    UINT32 i;

    for (i = CHILD_FIRST_PROGRAM; i < CHILDREN && !child; i++)
        if (!children[i].name)
            child = &children[i];
    if (!child) {
        say("nova-init: child refused name=");
        say(name);
        say(" reason=\"no room\"\r\n");
        return 0;
    }
    child->name = name;
    child->kind = KIND_PROGRAM;
    child->load = load;
    launch(platform, child);
    return child->state == STATE_RUNNING;
}

UINT32 platform_supervise(const PLATFORM *platform)
{
    PORT_PACKET packet;
    UINT32 status;

    for (;;) {
        status = port_wait(supervisor_port, DEADLINE_INFINITE, &packet);
        if (status != OK)
            return status;
        handle_packet(platform, &packet);
    }
}
