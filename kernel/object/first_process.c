/* The first process: the program nova-boot read from \nova\init.exe
   (docs/spec/boot-protocol.md), started in the root job with one
   bootstrap handle, a channel endpoint whose one message carries what
   the kernel gives it (docs/spec/ipc.md I6, objects.md O6):

   1. the root system resource, whose USE right lets it write to the
      kernel's debug log;
   2. a handle to the process itself with MAP and CREATE_THREAD, which a
      creator normally includes (O6);
   3. the root job, with which it creates processes;
   4. the root MMIO, IRQ and DMA resources, the authority over every
      device it gives drivers a part of (I6);
   5. the store image, as a loaded section over the pages nova-boot read
      it into, which only reads (docs/spec/storage.md F16; section.h);
   6. from BOOTSTRAP_FIRST_FILE on, each other file nova-boot read from
      \nova\, as a section holding its bytes that the process may only
      read.

   The message's bytes are a BOOTSTRAP_PLATFORM naming the PCI Express
   configuration space the ACPI MCFG table describes, then the store image's
   and the files' names and sizes (native_abi.h). The root I/O-port resource
   and the framebuffer's section that I6 lists join the message as the
   kernel implements their calls. The first process itself is not
   created with PROCESS_OPTION_WRITE_EXECUTE: the root job's handle carries
   the right for it to pass on, and no mapping of its own may be writable
   and executable (O5).

   The kernel keeps its own end of the channel until the process reports
   on it that its startup is done, or closes its end without reporting
   (docs/spec/boot-protocol.md, Startup done). The process's endpoint
   lacks TRANSFER, so no other process can ever hold it and report in
   the first process's place. Later the kernel waits for the process to
   end, logs the exit code process_info reports and closes its handle to
   the process. */

#include <stddef.h>

#include "process.h"
#include "boot_info.h"
#include "channel.h"
#include "cpu.h"
#include "image.h"
#include "resource.h"
#include "../acpi/acpi.h"
#include "../mm/page.h"
#include "../x86_64/paging.h"
#include "../x86_64/serial.h"
#include "../x86_64/user.h"

/* The first process's stack: USER_STACK_PAGES pages that end at USER_TOP,
   with RSP starting below the 40 bytes S8 has mapped above it, a return
   address and a home area. Nothing is mapped below them, so running off
   the stack faults. */
#define USER_STACK_PAGES 16
#define STACK_START_DEPTH 40

#define BOOTSTRAP_HANDLES (BOOTSTRAP_FIRST_FILE + BOOT_FILES_MAX)

/* The image the kernel runs, among the boot files. FAT matches names
   without regard to case, and the firmware reports the name as stored. */
#define INIT_NAME "init.exe"

/* The root DMA resource's range: every 32-bit bus requester ID (ipc.md
   I8). The MMIO resource spans the physical-address width; the memory
   map and the kernel's own registers are kept out of it when a section
   is made (section.h). */
#define REQUESTER_IDS 0x100000000ULL

/* The message's bytes: the platform record, then a name per file, the
   store image's first. */
typedef struct {
    BOOTSTRAP_PLATFORM platform;
    BOOTSTRAP_FILE store_image;
    BOOTSTRAP_FILE files[BOOT_FILES_MAX];
} BOOTSTRAP_BYTES;

#define SELF_RIGHTS                                                   \
    (PROCESS_RIGHT_MAP | PROCESS_RIGHT_CREATE_THREAD | RIGHT_DUPLICATE | \
     RIGHT_TRANSFER | RIGHT_WAIT)
#define BOOTSTRAP_ENDPOINT_RIGHTS                                     \
    (CHANNEL_RIGHT_READ | CHANNEL_RIGHT_WRITE | RIGHT_WAIT)
#define FILE_RIGHTS (SECTION_RIGHT_READ | RIGHT_DUPLICATE | RIGHT_TRANSFER)

_Static_assert(BOOTSTRAP_HANDLES <= CHANNEL_MAX_HANDLES,
               "the bootstrap message holds every handle");
_Static_assert(sizeof(BOOTSTRAP_BYTES) <= CHANNEL_MAX_BYTES,
               "the bootstrap message holds every file's name");
_Static_assert(offsetof(BOOTSTRAP_BYTES, files) ==
                   sizeof(BOOTSTRAP_PLATFORM) + sizeof(BOOTSTRAP_FILE),
               "the names follow the platform record directly");

/* The first process, held from its start until it has ended. */
static PROCESS *first;

static void fail(const char *what, STATUS status)
{
    serial_write("nova: first process: ");
    serial_write(what);
    serial_write(", status ");
    serial_number(status);
    serial_write(", halted\r\n");
    halt_forever();
}

static void check(STATUS status, const char *what)
{
    if (status != OK)
        fail(what, status);
}

static int lower(int c)
{
    return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c;
}

static int same_name(const char *a, const char *b)
{
    while (*a && lower(*a) == lower(*b)) {
        a++;
        b++;
    }
    return lower(*a) == lower(*b);
}

static const BOOT_FILE *find_init(const BOOT_INFO *info)
{
    UINT64 i;

    for (i = 0; i < info->file_count; i++)
        if (same_name(info->files[i].name, INIT_NAME))
            return &info->files[i];
    fail("no \\nova\\init.exe among the boot files", ERR_INVALID_ARGS);
    return 0;
}

/* Map the stack and return RSP for its first instruction. */
static UINT64 map_stack(PROCESS *process)
{
    UINT64 top = user_space_top();
    UINT64 page;

    for (page = top - USER_STACK_PAGES * PAGE_SIZE; page < top;
         page += PAGE_SIZE) {
        UINT8 *memory = page_alloc_mapped();

        if (!memory || !paging_map_user(process->space, page,
                                        (UINT64)memory - DIRECT_MAP_BASE,
                                        USER_READ_WRITE)) {
            if (memory)
                page_free_mapped(memory);
            fail("no memory for the stack", ERR_NO_MEMORY);
        }
    }
    return top - STACK_START_DEPTH;
}

static void describe_file(const BOOT_FILE *file, BOOTSTRAP_FILE *entry)
{
    UINT32 i;

    entry->size = file->size;
    for (i = 0; i < BOOT_FILE_NAME_SIZE; i++)
        entry->name[i] = file->name[i];
}

/* A section holding file's bytes, described at entry. An empty file gets
   a section of one page, since a section is never empty; its size in the
   message is still 0. */
static SECTION *file_section(const BOOT_FILE *file, BOOTSTRAP_FILE *entry)
{
    SECTION *section;

    check(section_create(file->size ? file->size : PAGE_SIZE, &section),
          "no memory for a boot file's section");
    section_fill(section, file->data, file->size);
    describe_file(file, entry);
    return section;
}

/* The store image's loaded section, over the pages nova-boot read it
   into, described at entry. */
static SECTION *store_image_section(const BOOT_FILE *image,
                                    BOOTSTRAP_FILE *entry)
{
    SECTION *section;

    check(section_create_loaded((UINT64)image->data - DIRECT_MAP_BASE,
                                image->size, &section),
          "the store image is not in pages nova-boot loaded");
    describe_file(image, entry);
    serial_write("nova: store image of ");
    serial_number(image->size);
    serial_write(" bytes passed as a read-only section over its pages\r\n");
    return section;
}

static OBJECT *root_resource(RESOURCE_KIND kind, UINT64 base, UINT64 size)
{
    RESOURCE *resource = resource_create_root(kind, base, size);

    if (!resource)
        fail("no memory for a root resource", ERR_NO_MEMORY);
    return &resource->object;
}

static void describe_platform(BOOTSTRAP_PLATFORM *platform)
{
    UINT32 i;

    platform->ecam_count = acpi_ecam_count();
    for (i = 0; i < platform->ecam_count; i++) {
        const ECAM_RANGE *range = acpi_ecam(i);

        platform->ecam[i].base = range->base;
        platform->ecam[i].segment = range->segment;
        platform->ecam[i].start_bus = range->start_bus;
        platform->ecam[i].end_bus = range->end_bus;
    }
}

static void send_bootstrap(PROCESS *process, JOB *job, ENDPOINT *kernel_end,
                           const BOOT_INFO *info)
{
    static BOOTSTRAP_BYTES bytes;
    CARRIED_HANDLE handles[BOOTSTRAP_HANDLES];
    UINT32 count = BOOTSTRAP_FIRST_FILE;
    UINT64 i;

    describe_platform(&bytes.platform);
    object_add_handle(&process->object);
    handles[0].object = root_resource(RESOURCE_SYSTEM, 0, SYSTEM_PRIVILEGES);
    handles[0].rights = RESOURCE_RIGHTS;
    handles[1].object = &process->object;
    handles[1].rights = SELF_RIGHTS;
    handles[2].object = &job->object;
    handles[2].rights = JOB_RIGHTS;
    handles[BOOTSTRAP_ROOT_MMIO].object =
        root_resource(RESOURCE_MMIO, 0, 1ULL << paging_physical_bits());
    handles[BOOTSTRAP_ROOT_MMIO].rights = RESOURCE_RIGHTS;
    handles[BOOTSTRAP_ROOT_IRQ].object =
        root_resource(RESOURCE_IRQ, 0, INTERRUPT_SOURCE_LIMIT);
    handles[BOOTSTRAP_ROOT_IRQ].rights = RESOURCE_RIGHTS;
    handles[BOOTSTRAP_ROOT_DMA].object =
        root_resource(RESOURCE_DMA, 0, REQUESTER_IDS);
    handles[BOOTSTRAP_ROOT_DMA].rights = RESOURCE_RIGHTS;
    handles[BOOTSTRAP_STORE_IMAGE].object =
        &store_image_section(&info->store_image, &bytes.store_image)->object;
    handles[BOOTSTRAP_STORE_IMAGE].rights = LOADED_SECTION_RIGHTS;
    for (i = 0; i < info->file_count; i++) {
        handles[count].object =
            &file_section(&info->files[i], &bytes.files[i])->object;
        handles[count].rights = FILE_RIGHTS;
        count++;
    }
    check(channel_write_kernel(kernel_end, (const UINT8 *)&bytes,
                               (UINT32)(sizeof bytes.platform +
                                        (1 + info->file_count) *
                                            sizeof(BOOTSTRAP_FILE)),
                               handles, count),
          "the bootstrap message was refused");
}

/* The line comes whole, though the process already runs and writes to
   the log too. */
static void say_started(const IMAGE *image, UINT64 stack)
{
    UINT64 flags = serial_lock();

    serial_write("nova: first process started: image ");
    serial_hex(image->base);
    serial_write(" to ");
    serial_hex(image->end);
    serial_write(", entry ");
    serial_hex(image->entry);
    serial_write(", stack ");
    serial_hex(stack);
    serial_write("\r\n");
    serial_unlock(flags);
}

static void say_line(const char *line)
{
    UINT64 flags = serial_lock();

    serial_write(line);
    serial_unlock(flags);
}

int first_process_start(void *boot_info)
{
    const BOOT_INFO *info = boot_info;
    const BOOT_FILE *init = find_init(info);
    JOB *job = job_create_root();
    PROCESS *process;
    USER_THREAD *thread;
    ENDPOINT *kernel_end, *process_end;
    IMAGE image;
    UINT64 stack;
    UINT32 observed;

    if (!job)
        fail("no memory for the root job", ERR_NO_MEMORY);
    check(process_create(job, 0, &process), "no memory for the process");
    check(image_load(process->space, init->data, init->size,
                     user_space_top() - USER_STACK_PAGES * PAGE_SIZE, &image),
          "\\nova\\init.exe is not an image the kernel loads");
    stack = map_stack(process);
    check(process_thread_create(process, &thread),
          "no memory for the first thread");
    check(channel_create(1, &kernel_end, &process_end),
          "no memory for the bootstrap channel");
    send_bootstrap(process, job, kernel_end, info);
    check(process_start(process, thread, image.entry, stack,
                        &process_end->object, BOOTSTRAP_ENDPOINT_RIGHTS, 0),
          "the process did not start");
    object_close(&thread->object);
    first = process;
    say_started(&image, stack);
    wait_one(&kernel_end->waitable, CHANNEL_READABLE | CHANNEL_PEER_CLOSED,
             DEADLINE_INFINITE, &observed);
    object_close(&kernel_end->object);
    if (observed & CHANNEL_READABLE) {
        say_line("nova: first process reports startup done\r\n");
        return 1;
    }
    say_line("nova: first process closed its bootstrap channel without "
             "reporting startup done\r\n");
    return 0;
}

void first_process_wait(void)
{
    UINT32 observed, state, exit_code;
    UINT64 flags;

    wait_one(&first->waitable, PROCESS_TERMINATED, DEADLINE_INFINITE,
             &observed);
    process_info(first, &state, &exit_code);
    flags = serial_lock();
    serial_write("nova: first process ended: process_info reports state ");
    serial_number(state);
    serial_write(", exit code ");
    serial_number(exit_code);
    serial_write("\r\n");
    serial_unlock(flags);
    object_close(&first->object);
}
