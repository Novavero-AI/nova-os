/* The platform driver. It walks PCI configuration space through the ECAM
   ranges the kernel found in the ACPI MCFG table, from each range's
   first bus down through the bridges, mapping each bus it visits once,
   and logs each function. Two functions get a driver, each a process of
   its own in a job of its own, whose bootstrap message (pci.h) carries
   resources covering only that function: its configuration space, its
   memory BARs, its message-signalled interrupts and an initiator for
   its requester ID, with PIN alone. init keeps the initiator's RELEASE.

   The first virtio block function goes to \nova\virtblk.exe, which reads
   the boot disk and ends. The first virtio input function goes to
   \nova\virtinp.exe, which runs on: once its device is live it reports
   with an endpoint, and init starts \nova\inputlog.exe, the client that
   logs the key events the driver sends there (docs/spec/input.md).

   A driver that does not start, or ends before it has done its work,
   the block driver without VIRTIO_BLK_EXIT_DONE and the input driver
   before its report, gets its function stopped: init clears Bus Master
   Enable, resets the device, releases the initiator's quarantine and
   starts the driver once more (ipc.md I10). init supervises the input driver only until the
   report: it ends once its checks are done, and the driver and its
   client run on with no supervisor. */

#include "platform.h"
#include "init.h"
#include "input_protocol.h"
#include "loader.h"
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
#define CLIENT_FILE "inputlog.exe"

/* The rights the driver's handles carry: its own process with MAP, to
   map its sections, and the initiator with PIN, so that releasing the
   quarantine stays with the supervisor. */
#define DRIVER_SELF_RIGHTS (PROCESS_RIGHT_MAP | RIGHT_TRANSFER)
#define DRIVER_INITIATOR_RIGHTS (INITIATOR_RIGHT_PIN | RIGHT_TRANSFER)

/* The interrupt vectors a driver's job may hold: each driver takes one,
   for its queues, and none for configuration changes. */
#define DRIVER_INTERRUPT_VECTORS 1

static const JOB_LIMITS driver_limits = {DRIVER_INTERRUPT_VECTORS};

/* How many times init starts a driver: once, and once more if it fails,
   since a failure that repeats at once is not one a restart cures. */
#define DRIVER_STARTS 2

/* What init waits for once it has started the input driver: its process
   to end or its report to come. */
#define WAIT_DRIVER 0
#define WAIT_REPORT 1
#define INPUT_WAITS 2

/* A driver init starts: what its log lines call it, and its file. */
typedef struct {
    const char *name;
    const char *file;
} DRIVER;

static const DRIVER block_driver = {"virtio block driver", BLOCK_FILE};
static const DRIVER input_driver = {"virtio input driver", INPUT_FILE};

typedef struct {
    UINT32 requester;
    UINT64 config;          /* physical, or 0 if no function was found */
    volatile UINT8 *mapped; /* in init, inside its bus's mapping */
    volatile UINT8 *common; /* its virtio common configuration, mapped
                               in init once it is first reset, or 0 */
} FUNCTION;

/* The first function of each kind a driver serves. */
typedef struct {
    FUNCTION block, input;
} FOUND;

/* The buses of one ECAM range, each mapped into init when first
   visited, or 0. */
typedef struct {
    const ECAM_RANGE *range;
    volatile UINT8 *buses[BUSES];
} SEGMENT;

static SEGMENT segment;

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

static void say_driver(const char *before, const DRIVER *driver,
                       const char *after)
{
    say("nova-init: ");
    say(before);
    say(driver->name);
    say(after);
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
   serves it. */
static void note_virtio(FOUND *found, UINT32 id, UINT32 requester,
                        UINT64 config, volatile UINT8 *mapped)
{
    if (id == VIRTIO_BLK_PCI_DEVICE || id == VIRTIO_BLK_PCI_TRANSITIONAL)
        note(&found->block, requester, config, mapped);
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

/* Each range starts with no bus visited; the windows of an earlier one
   stay mapped. The walk stops after the range in which the last kind
   is found. */
static void scan(const PLATFORM *platform, FOUND *found)
{
    const BOOTSTRAP_PLATFORM *boot = platform->boot;
    UINT32 i, bus;

    found->block.config = 0;
    found->block.common = 0;
    found->input.config = 0;
    found->input.common = 0;
    for (i = 0; i < boot->ecam_count && i < BOOTSTRAP_ECAM_RANGES_MAX &&
                !(found->block.config && found->input.config);
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

/* The bootstrap record for function, with its BARs sized, and an
   initiator for its requester ID. Returns 0, with nothing left open, if
   the initiator cannot be made. */
static int prepare(const PLATFORM *platform, const DRIVER *driver,
                   const FUNCTION *function, PCI_DEVICE_BOOTSTRAP *device,
                   UINT32 *initiator)
{
    size_bars(function->mapped, device);
    device->requester = function->requester;
    device->reserved = 0;
    device->config = function->config;
    if (initiator_create(platform->dma, function->requester, initiator) ==
        OK)
        return 1;
    say_driver("cannot create the initiator for the ", driver, "\r\n");
    return 0;
}

static void close_handles(const HANDLE_TRANSFER *handles, UINT32 count)
{
    UINT32 i;

    for (i = 0; i < count; i++)
        if (handles[i].handle != HANDLE_INVALID)
            handle_close(handles[i].handle);
}

/* The handles of the driver's bootstrap message, as pci.h orders them.
   Returns the handle count, or 0, with none left open, if a call
   fails. */
static UINT32 driver_handles(const PLATFORM *platform,
                             const FUNCTION *function,
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
                        INTERRUPT_SOURCE_MSI + function->requester, 1,
                        &handles[PCI_HANDLE_IRQ].handle) != OK ||
        resource_create(platform->mmio, function->config, PCI_CONFIG_SIZE,
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
static void say_start(const DRIVER *driver, const FUNCTION *function,
                      const PCI_DEVICE_BOOTSTRAP *device)
{
    UINT32 i;

    say_driver("starting the ", driver, " for ");
    pci_say_function(function->requester);
    say(", with its configuration space at ");
    say_hex(function->config);
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

/* A job of its own for a driver, a child of the root job that grants it
   driver_limits, as I10 has a supervisor do; HANDLE_INVALID, logged, if
   it cannot be made. */
static UINT32 driver_job(const PLATFORM *platform)
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

/* Start driver in job with function's bootstrap message. Returns 1 with
   its process in *process and init's end of its bootstrap channel in
   *channel; 0, logged, with nothing left open, if it did not start. */
static int start_driver(const PLATFORM *platform, const DRIVER *driver,
                        UINT32 job, const FUNCTION *function,
                        PCI_DEVICE_BOOTSTRAP *device, UINT32 initiator,
                        UINT32 *process, UINT32 *channel)
{
    HANDLE_TRANSFER handles[PCI_HANDLES_MAX];
    PROGRAM program;
    UINT32 theirs, count;
    UINT32 status = loader_load(job, boot_file(driver->file), &program);

    if (status != OK) {
        say_driver("cannot load the ", driver, ", status ");
        say_number(status);
        say("\r\n");
        return 0;
    }
    count = driver_handles(platform, function, device, program.process,
                           initiator, handles);
    if (!count || channel_create(CHANNEL_CAPACITY, channel, &theirs) != OK) {
        close_handles(handles, count);
        status = ERR_NO_MEMORY;
    } else {
        say_start(driver, function, device);
        status = channel_write(*channel, device, sizeof *device, handles,
                               count);
        if (status != OK)
            close_handles(handles, count);
        if (status == OK)
            status = loader_start(&program, theirs);
        if (status != OK) {
            handle_close(theirs);
            handle_close(*channel);
        }
    }
    if (status != OK) {
        say_driver("cannot start the ", driver, ", status ");
        say_number(status);
        say("\r\n");
        if (program.thread != HANDLE_INVALID)
            handle_close(program.thread);
        handle_close(program.process);
        return 0;
    }
    *process = program.process;
    return 1;
}

/* The exit code of driver's process, which a wait that gave status saw
   terminate; *ended is 0, and the code 0, if init cannot tell that it
   ended, when it may still run. */
static UINT32 exit_code_of(const DRIVER *driver, UINT32 process,
                           UINT32 status, int *ended)
{
    UINT32 state = 0, exit_code = 0;

    if (status == OK)
        status = process_info(process, &state, &exit_code);
    *ended = status == OK && state == PROCESS_STATE_TERMINATED;
    if (!*ended) {
        say_driver("the ", driver, "'s end was not seen, status ");
        say_number(status);
        say("\r\n");
        return 0;
    }
    say_driver("", driver, " ended, exit code ");
    say_number(exit_code);
    say("\r\n");
    return exit_code;
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

/* Whether driver's file is on the boot volume; logged if not. */
static int have_file(const DRIVER *driver)
{
    if (boot_file(driver->file))
        return 1;
    say("nova-init: no \\nova\\");
    say(driver->file);
    say(", the ");
    say(driver->name);
    say("\r\n");
    return 0;
}

/* ------------------------------------------------------------------------
   The block driver. */

/* Run the block driver to its end, and once more if it fails. A driver
   that does not start counts as failed, and its function is stopped
   before the second start as after a failed run. Returns 1 if it ended
   with VIRTIO_BLK_EXIT_DONE, or if there is no block function or no
   driver file; 0 if it failed. */
static int run_block(const PLATFORM *platform, FUNCTION *function)
{
    PCI_DEVICE_BOOTSTRAP device;
    UINT32 initiator, job, process, channel, observed, start;
    UINT32 exit_code = 0;
    int ended;

    if (!function->config) {
        say("nova-init: no virtio block function\r\n");
        return 1;
    }
    if (!have_file(&block_driver))
        return 1;
    if (!prepare(platform, &block_driver, function, &device, &initiator))
        return 0;
    for (start = 0; start < DRIVER_STARTS; start++) {
        if (start)
            say("nova-init: restarting the virtio block driver\r\n");
        ended = 1;
        exit_code = 0;
        job = driver_job(platform);
        if (job != HANDLE_INVALID) {
            if (start_driver(platform, &block_driver, job, function, &device,
                             initiator, &process, &channel)) {
                handle_close(channel);
                exit_code = exit_code_of(
                    &block_driver, process,
                    object_wait_one(process, PROCESS_TERMINATED,
                                    DEADLINE_INFINITE, &observed),
                    &ended);
                handle_close(process);
            }
            handle_close(job);
        }
        if (!ended || !stop_function(platform, function, &device, initiator) ||
            exit_code == VIRTIO_BLK_EXIT_DONE)
            break;
    }
    handle_close(initiator);
    return exit_code == VIRTIO_BLK_EXIT_DONE;
}

/* ------------------------------------------------------------------------
   The input driver and its client. */

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

/* Start the client in the root job with endpoint, which is the client's
   from then on, or closed if the client does not start. */
static void start_client(const PLATFORM *platform, UINT32 endpoint)
{
    HANDLE_TRANSFER handles[INPUT_CLIENT_HANDLES];
    PROGRAM program;
    UINT32 mine, theirs, transaction = 0;
    UINT32 status =
        loader_load(platform->job, boot_file(CLIENT_FILE), &program);

    handles[INPUT_CLIENT_HANDLE_DEBUG_LOG].handle = HANDLE_INVALID;
    handles[INPUT_CLIENT_HANDLE_DEBUG_LOG].rights = RIGHTS_SAME;
    handles[INPUT_CLIENT_HANDLE_EVENTS].handle = endpoint;
    handles[INPUT_CLIENT_HANDLE_EVENTS].rights = RIGHTS_SAME;
    if (status == OK)
        status = resource_create(
            platform->debug_log, SYSTEM_PRIVILEGE_DEBUG_LOG, 1,
            &handles[INPUT_CLIENT_HANDLE_DEBUG_LOG].handle);
    if (status == OK)
        status = channel_create(CHANNEL_CAPACITY, &mine, &theirs);
    if (status == OK) {
        say("nova-init: the virtio input driver reported; starting its "
            "client, \\nova\\" CLIENT_FILE "\r\n");
        status = channel_write(mine, &transaction, sizeof transaction,
                               handles, INPUT_CLIENT_HANDLES);
        handle_close(mine);
        if (status == OK)
            status = loader_start(&program, theirs);
        else
            close_handles(handles, INPUT_CLIENT_HANDLES);
        if (status != OK)
            handle_close(theirs);
    } else {
        close_handles(handles, INPUT_CLIENT_HANDLES);
    }
    if (program.thread != HANDLE_INVALID)
        handle_close(program.thread);
    if (program.process != HANDLE_INVALID)
        handle_close(program.process);
    if (status != OK) {
        say("nova-init: cannot start the input driver's client, status ");
        say_number(status);
        say("\r\n");
    }
}

/* Wait until the input driver, started as process with init's end of
   its bootstrap channel in channel, reports or ends, and close channel.
   Returns 1 once it has reported, with its client started if the report
   is well formed; 0 once it has ended, or if init cannot tell, with
   *ended as exit_code_of sets it. */
static int await_report(const PLATFORM *platform, UINT32 process,
                        UINT32 channel, int *ended)
{
    WAIT_ENTRY waits[INPUT_WAITS];
    UINT32 endpoint;
    UINT32 status;

    waits[WAIT_DRIVER].handle = process;
    waits[WAIT_DRIVER].signals = PROCESS_TERMINATED;
    waits[WAIT_REPORT].handle = channel;
    waits[WAIT_REPORT].signals = CHANNEL_READABLE;
    status = object_wait_many(waits, INPUT_WAITS, DEADLINE_INFINITE);
    if (status == OK && !(waits[WAIT_DRIVER].observed & PROCESS_TERMINATED)) {
        endpoint = take_report(channel);
        handle_close(channel);
        if (endpoint != HANDLE_INVALID)
            start_client(platform, endpoint);
        return 1;
    }
    handle_close(channel);
    exit_code_of(&input_driver, process, status, ended);
    return 0;
}

/* Start the input driver, and once more if it ends before it reports or
   does not start, its function stopped between the two as for the block
   driver; once it reports, start its client with the endpoint it gave.
   init's handles to the driver's process, job and initiator close
   then, and with them its supervision. */
static void run_input(const PLATFORM *platform, FUNCTION *function)
{
    PCI_DEVICE_BOOTSTRAP device;
    UINT32 initiator, job, process, channel, start;
    int ended, reported = 0;

    if (!function->config) {
        say("nova-init: no virtio input function\r\n");
        return;
    }
    if (!have_file(&input_driver))
        return;
    if (!boot_file(CLIENT_FILE)) {
        say("nova-init: no \\nova\\" CLIENT_FILE ", the input driver's "
            "client\r\n");
        return;
    }
    if (!prepare(platform, &input_driver, function, &device, &initiator))
        return;
    for (start = 0; start < DRIVER_STARTS && !reported; start++) {
        if (start)
            say("nova-init: restarting the virtio input driver\r\n");
        ended = 1;
        job = driver_job(platform);
        if (job != HANDLE_INVALID) {
            if (start_driver(platform, &input_driver, job, function, &device,
                             initiator, &process, &channel)) {
                reported = await_report(platform, process, channel, &ended);
                handle_close(process);
            }
            handle_close(job);
        }
        if (!reported &&
            (!ended || !stop_function(platform, function, &device, initiator)))
            break;
    }
    handle_close(initiator);
    if (!reported)
        say("nova-init: the virtio input driver never reported; the system "
            "runs on without it\r\n");
}

int platform_run(const PLATFORM *platform)
{
    FOUND found;
    int block;

    scan(platform, &found);
    block = run_block(platform, &found.block);
    run_input(platform, &found.input);
    return block;
}
