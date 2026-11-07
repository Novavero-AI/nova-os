/* The first process: the program nova-boot read from \nova\init.exe
   (docs/spec/boot-protocol.md), started in the root job with one
   bootstrap handle, a channel endpoint whose one message carries what
   the kernel gives it (docs/spec/ipc.md I6, objects.md O6):

   1. the root system resource, whose USE right lets it write to the
      kernel's debug log;
   2. a handle to the process itself with MAP and CREATE_THREAD, which a
      creator normally includes (O6);
   3. the root job, with which it creates processes;
   4. from BOOTSTRAP_FIRST_FILE on, each file nova-boot read from \nova\,
      as a section holding its bytes that the process may only read, with
      the files' names and sizes in the message's bytes (native_abi.h).

   The other root resources that I6 lists join the message as the kernel
   implements their objects. The kernel's own end of the channel is
   closed once the message is written; a message already queued stays
   readable after its writer closes (I1). The first process itself is
   not created with PROCESS_OPTION_WRITE_EXECUTE: the root job's handle
   carries the right for it to pass on, and no mapping of its own may be
   writable and executable (O5).

   The kernel then waits for the process to end, logs the exit code
   process_info reports and closes its handle to the process. */

#include "process.h"
#include "boot_info.h"
#include "channel.h"
#include "cpu.h"
#include "image.h"
#include "resource.h"
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

#define ROOT_RESOURCE_RIGHTS                                          \
    (RESOURCE_RIGHT_USE | RIGHT_DUPLICATE | RIGHT_TRANSFER)
#define SELF_RIGHTS                                                   \
    (PROCESS_RIGHT_MAP | PROCESS_RIGHT_CREATE_THREAD | RIGHT_DUPLICATE | \
     RIGHT_TRANSFER | RIGHT_WAIT)
#define FILE_RIGHTS (SECTION_RIGHT_READ | RIGHT_DUPLICATE | RIGHT_TRANSFER)

_Static_assert(BOOTSTRAP_HANDLES <= CHANNEL_MAX_HANDLES,
               "the bootstrap message holds every handle");
_Static_assert(sizeof(BOOTSTRAP_FILE) * BOOT_FILES_MAX <= CHANNEL_MAX_BYTES,
               "the bootstrap message holds every file's name");

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

/* A section holding file's bytes, described at entry. An empty file gets
   a section of one page, since a section is never empty; its size in the
   message is still 0. */
static SECTION *file_section(const BOOT_FILE *file, BOOTSTRAP_FILE *entry)
{
    SECTION *section;
    UINT32 i;

    check(section_create(file->size ? file->size : PAGE_SIZE, &section),
          "no memory for a boot file's section");
    section_fill(section, file->data, file->size);
    entry->size = file->size;
    for (i = 0; i < BOOT_FILE_NAME_SIZE; i++)
        entry->name[i] = file->name[i];
    return section;
}

static void send_bootstrap(PROCESS *process, JOB *job, ENDPOINT *kernel_end,
                           const BOOT_INFO *info)
{
    static BOOTSTRAP_FILE files[BOOT_FILES_MAX];
    RESOURCE *root = resource_create_root_system();
    CARRIED_HANDLE handles[BOOTSTRAP_HANDLES];
    UINT32 count = BOOTSTRAP_FIRST_FILE;
    UINT64 i;

    if (!root)
        fail("no memory for the root resource", ERR_NO_MEMORY);
    object_add_handle(&process->object);
    handles[0].object = &root->object;
    handles[0].rights = ROOT_RESOURCE_RIGHTS;
    handles[1].object = &process->object;
    handles[1].rights = SELF_RIGHTS;
    handles[2].object = &job->object;
    handles[2].rights = ROOT_JOB_RIGHTS;
    for (i = 0; i < info->file_count; i++) {
        handles[count].object = &file_section(&info->files[i], &files[i])->object;
        handles[count].rights = FILE_RIGHTS;
        count++;
    }
    check(channel_write_kernel(kernel_end, (const UINT8 *)files,
                               (UINT32)(info->file_count *
                                        sizeof(BOOTSTRAP_FILE)),
                               handles, count),
          "the bootstrap message was refused");
    object_close(&kernel_end->object);
}

void first_process_run(void *boot_info)
{
    const BOOT_INFO *info = boot_info;
    const BOOT_FILE *init = find_init(info);
    JOB *job = job_create_root();
    PROCESS *process;
    USER_THREAD *thread;
    ENDPOINT *kernel_end, *process_end;
    IMAGE image;
    UINT64 stack;
    UINT32 observed, state, exit_code;

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
                        &process_end->object, CHANNEL_RIGHTS, 0),
          "the process did not start");
    object_close(&thread->object);
    serial_write("nova: first process started: image ");
    serial_hex(image.base);
    serial_write(" to ");
    serial_hex(image.end);
    serial_write(", entry ");
    serial_hex(image.entry);
    serial_write(", stack ");
    serial_hex(stack);
    serial_write("\r\n");

    wait_one(&process->waitable, PROCESS_TERMINATED, DEADLINE_INFINITE,
             &observed);
    process_info(process, &state, &exit_code);
    serial_write("nova: first process ended: process_info reports state ");
    serial_number(state);
    serial_write(", exit code ");
    serial_number(exit_code);
    serial_write("\r\n");
    object_close(&process->object);
}
