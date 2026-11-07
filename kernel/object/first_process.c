/* The first process: the program nova-boot read from \nova\init.exe
   (docs/spec/boot-protocol.md), started with one bootstrap handle, a
   channel endpoint whose one message carries what the kernel gives it
   (docs/spec/ipc.md I6, objects.md O6):

   1. the root system resource, whose USE right lets it write to the
      kernel's debug log;
   2. a handle to the process itself with MAP and CREATE_THREAD, which a
      creator normally includes (O6).

   The other root resources and the root job that I6 lists join the
   message as the kernel implements their objects. The kernel's own end
   of the channel is closed once the message is written; a message
   already queued stays readable after its writer closes (I1).

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

#define BOOTSTRAP_HANDLES 2

#define ROOT_RESOURCE_RIGHTS                                          \
    (RESOURCE_RIGHT_USE | RIGHT_DUPLICATE | RIGHT_TRANSFER)
#define SELF_RIGHTS                                                   \
    (PROCESS_RIGHT_MAP | PROCESS_RIGHT_CREATE_THREAD | RIGHT_DUPLICATE | \
     RIGHT_TRANSFER | RIGHT_WAIT)

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

static void send_bootstrap(PROCESS *process, ENDPOINT *kernel_end)
{
    RESOURCE *root = resource_create_root_system();
    CARRIED_HANDLE handles[BOOTSTRAP_HANDLES];

    if (!root)
        fail("no memory for the root resource", ERR_NO_MEMORY);
    object_add_handle(&process->object);
    handles[0].object = &root->object;
    handles[0].rights = ROOT_RESOURCE_RIGHTS;
    handles[1].object = &process->object;
    handles[1].rights = SELF_RIGHTS;
    check(channel_write_handles(kernel_end, handles, BOOTSTRAP_HANDLES),
          "the bootstrap message was refused");
    object_close(&kernel_end->object);
}

void first_process_run(void *boot_info)
{
    const BOOT_INFO *info = boot_info;
    PROCESS *process;
    ENDPOINT *kernel_end, *process_end;
    IMAGE image;
    UINT64 stack;
    UINT32 observed, state, exit_code;

    check(process_create(&process), "no memory for the process");
    check(image_load(process->space, info->init_image, info->init_image_size,
                     user_space_top() - USER_STACK_PAGES * PAGE_SIZE, &image),
          "\\nova\\init.exe is not an image the kernel loads");
    stack = map_stack(process);
    check(channel_create(&kernel_end, &process_end),
          "no memory for the bootstrap channel");
    send_bootstrap(process, kernel_end);
    check(process_start(process, image.entry, stack, &process_end->object,
                        CHANNEL_RIGHTS, 0),
          "the process did not start");
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
