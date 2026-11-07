/* The platform driver. It walks PCI configuration space through the ECAM
   ranges the kernel found in the ACPI MCFG table, from each range's
   first bus down through the bridges, mapping each bus it visits once,
   and logs each function. The first virtio block function goes to the
   virtio block driver, \nova\virtblk.exe, a process of its own whose
   bootstrap message (pci.h) carries resources covering only that
   function: its configuration space, its memory BARs, its
   message-signalled interrupts and an initiator for its requester ID,
   with PIN alone. init keeps the initiator's RELEASE: once the driver
   has ended, it clears the function's Bus Master Enable, which stops the
   function's DMA, and releases the initiator's quarantine (ipc.md
   I10). */

#include "platform.h"
#include "init.h"
#include "loader.h"
#include "mmio.h"
#include "pci.h"
#include "virtio_blk.h"

#define BUS_SIZE ((UINT64)1 << PCI_BUS_SHIFT)
#define BUSES 256
#define CHANNEL_CAPACITY 1
#define ALL_ONES 0xffffffffu
#define DRIVER_FILE "virtblk.exe"

/* The rights the driver's handles carry: its own process with MAP, to
   map its sections, and the initiator with PIN, so that releasing the
   quarantine stays with the supervisor. */
#define DRIVER_SELF_RIGHTS (PROCESS_RIGHT_MAP | RIGHT_TRANSFER)
#define DRIVER_INITIATOR_RIGHTS (INITIATOR_RIGHT_PIN | RIGHT_TRANSFER)

typedef struct {
    UINT32 requester;
    UINT64 config;          /* physical */
    volatile UINT8 *mapped; /* in init, inside its bus's mapping */
} FUNCTION;

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

static int virtio_block(volatile UINT8 *config, UINT32 vendor, UINT32 id)
{
    return vendor == VIRTIO_PCI_VENDOR &&
           (id == VIRTIO_BLK_PCI_DEVICE ||
            id == VIRTIO_BLK_PCI_TRANSITIONAL) &&
           (mmio_read8(config + PCI_HEADER_TYPE) & PCI_HEADER_LAYOUT) ==
               PCI_HEADER_ENDPOINT;
}

/* Visit bus and, through the bridges on it, the buses below, each once;
   log every function, and note the first virtio block function in found
   if it has none yet. A device's functions above 0 are read only if
   function 0 is multi-function. */
static void scan_bus(const PLATFORM *platform, UINT32 bus, FUNCTION *found)
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
            if (!found->config && virtio_block(config, vendor, id)) {
                found->requester = requester;
                found->config = physical + offset;
                found->mapped = config;
            }
            if ((header & PCI_HEADER_LAYOUT) == PCI_HEADER_BRIDGE) {
                below = mmio_read8(config + PCI_SECONDARY_BUS);
                if (below > bus)
                    scan_bus(platform, below, found);
            }
        }
    }
}

/* Each range starts with no bus visited; the windows of an earlier one
   stay mapped. */
static void scan(const PLATFORM *platform, FUNCTION *found)
{
    const BOOTSTRAP_PLATFORM *boot = platform->boot;
    UINT32 i, bus;

    found->config = 0;
    for (i = 0; i < boot->ecam_count && i < BOOTSTRAP_ECAM_RANGES_MAX &&
                !found->config;
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
static UINT32 driver_handles(const PLATFORM *platform, const FUNCTION *found,
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
                        INTERRUPT_SOURCE_MSI + found->requester, 1,
                        &handles[PCI_HANDLE_IRQ].handle) != OK ||
        resource_create(platform->mmio, found->config, PCI_CONFIG_SIZE,
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

/* Log what the driver's resources cover. */
static void say_grant(const FUNCTION *found,
                      const PCI_DEVICE_BOOTSTRAP *device)
{
    UINT32 i;

    say("nova-init: virtio block driver started for ");
    pci_say_function(found->requester);
    say(", with its configuration space at ");
    say_hex(found->config);
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

/* Start the driver from file and wait for it to end. Returns its exit
   code, or 0 if it did not start. */
static UINT32 run_driver(const PLATFORM *platform, const BOOT_FILE_VIEW *file,
                         const FUNCTION *found, PCI_DEVICE_BOOTSTRAP *device,
                         UINT32 initiator)
{
    HANDLE_TRANSFER handles[PCI_HANDLES_MAX];
    PROGRAM program;
    UINT32 mine, theirs, count, observed, state, exit_code;
    UINT32 status = loader_load(platform->job, file, &program);

    if (status != OK) {
        say("nova-init: cannot load the driver, status ");
        say_number(status);
        say("\r\n");
        return 0;
    }
    count = driver_handles(platform, found, device, program.process,
                           initiator, handles);
    if (!count || channel_create(CHANNEL_CAPACITY, &mine, &theirs) != OK) {
        close_handles(handles, count);
        status = ERR_NO_MEMORY;
    } else {
        status = channel_write(mine, device, sizeof *device, handles, count);
        if (status != OK)
            close_handles(handles, count);
        handle_close(mine);
        if (status == OK)
            status = loader_start(&program, theirs);
        if (status != OK)
            handle_close(theirs);
    }
    if (status != OK) {
        say("nova-init: cannot start the driver, status ");
        say_number(status);
        say("\r\n");
        if (program.thread != HANDLE_INVALID)
            handle_close(program.thread);
        handle_close(program.process);
        return 0;
    }
    say_grant(found, device);
    object_wait_one(program.process, PROCESS_TERMINATED, DEADLINE_INFINITE,
                    &observed);
    process_info(program.process, &state, &exit_code);
    handle_close(program.process);
    say("nova-init: virtio block driver ended, exit code ");
    say_number(exit_code);
    say("\r\n");
    return exit_code;
}

/* Clear the function's Bus Master Enable, so it starts no more DMA, and
   once a read shows the bit clear, release what its initiator holds in
   quarantine (ipc.md I8, I10). init does not reset the device itself: a
   driver started for it again resets it before it enables bus mastering
   (VIRTIO 1.4 3.1.1), so the device never reaches the released pages
   through the rings the dead driver left it. */
static void stop_function(const FUNCTION *found, UINT32 initiator)
{
    mmio_write16(found->mapped + PCI_COMMAND,
                 (UINT16)(mmio_read16(found->mapped + PCI_COMMAND) &
                          ~PCI_COMMAND_BUS_MASTER));
    if (mmio_read16(found->mapped + PCI_COMMAND) & PCI_COMMAND_BUS_MASTER) {
        say("nova-init: bus mastering stays on; the initiator's quarantine "
            "stays\r\n");
        return;
    }
    if (initiator_release_quarantine(initiator) == OK)
        say("nova-init: bus mastering off, initiator's quarantine "
            "released\r\n");
}

int platform_run(const PLATFORM *platform)
{
    const BOOT_FILE_VIEW *file = boot_file(DRIVER_FILE);
    PCI_DEVICE_BOOTSTRAP device;
    FUNCTION found;
    UINT32 initiator, exit_code;

    scan(platform, &found);
    if (!found.config) {
        say("nova-init: no virtio block function\r\n");
        return 1;
    }
    if (!file) {
        say("nova-init: no \\nova\\" DRIVER_FILE ", the virtio block "
            "driver\r\n");
        return 1;
    }
    size_bars(found.mapped, &device);
    device.requester = found.requester;
    device.reserved = 0;
    device.config = found.config;
    if (initiator_create(platform->dma, found.requester, &initiator) != OK) {
        say("nova-init: cannot create the function's initiator\r\n");
        return 0;
    }
    exit_code = run_driver(platform, file, &found, &device, initiator);
    stop_function(&found, initiator);
    handle_close(initiator);
    return exit_code == VIRTIO_BLK_EXIT_DONE;
}
