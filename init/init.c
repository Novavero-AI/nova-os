/* init: the first process, which the kernel starts from \nova\init.exe
   with one bootstrap handle (docs/spec/ipc.md I6). It reads the bootstrap
   message, writes to the kernel's debug log with the root system
   resource the message carries, checks the answers of the calls it has
   to the cases their contracts name, and exits with EXIT_CHECKS_PASSED,
   which the kernel reads with process_info and logs. A failed check is
   logged, if the log is reachable, and ends the process with
   EXIT_CHECK_FAILED. */

#include "native.h"

/* A nonzero code, so that the kernel's log shows the value process_exit
   passed and not a default. */
#define EXIT_CHECKS_PASSED 42
#define EXIT_CHECK_FAILED 1

/* The handles the bootstrap message carries: the root system resource
   and the process itself. */
#define BOOTSTRAP_HANDLES 2

/* An address in the kernel's half, and one in user memory that nothing
   maps (the first page of user memory, below the image). */
#define KERNEL_ADDRESS 0xffff800000000000ULL
#define UNMAPPED_USER_ADDRESS 0x10000ULL

static UINT32 debug_log = HANDLE_INVALID;

static UINT64 length(const char *s)
{
    UINT64 n = 0;

    while (s[n])
        n++;
    return n;
}

static void say(const char *line)
{
    debug_write(debug_log, line, length(line));
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

/* Take the debug log's resource and the process's own handle from the
   bootstrap message, checking that a buffer too small for its handles
   leaves it queued and reports how many it holds; that read whole it
   holds the resource and the process; and that once it is read the queue
   is empty and the kernel's end closed. */
static void read_bootstrap(UINT32 bootstrap, UINT32 *self)
{
    RECEIVED_HANDLE handles[BOOTSTRAP_HANDLES];
    UINT8 bytes[1];
    UINT32 byte_count, handle_count;

    if (channel_read(bootstrap, bytes, sizeof bytes, handles, 0, &byte_count,
                     &handle_count) != ERR_BUFFER_TOO_SMALL ||
        handle_count != BOOTSTRAP_HANDLES)
        process_exit(EXIT_CHECK_FAILED);
    if (channel_read(bootstrap, bytes, sizeof bytes, handles,
                     BOOTSTRAP_HANDLES, &byte_count, &handle_count) != OK ||
        byte_count != 0 || handle_count != BOOTSTRAP_HANDLES ||
        handles[0].type != TYPE_RESOURCE ||
        !(handles[0].rights & RESOURCE_RIGHT_USE) ||
        handles[1].type != TYPE_PROCESS)
        process_exit(EXIT_CHECK_FAILED);
    debug_log = handles[0].handle;
    *self = handles[1].handle;
    check(channel_read(bootstrap, bytes, sizeof bytes, handles,
                       BOOTSTRAP_HANDLES, &byte_count,
                       &handle_count) == ERR_PEER_CLOSED,
          "a drained bootstrap channel is not peer-closed");
}

/* Each call checks its handle's value, type and rights in that order
   (docs/spec/objects.md O3), then its buffer, which must lie in user
   memory and be mapped (docs/spec/syscalls.md S6). */
static void check_errors(UINT32 bootstrap, UINT32 self)
{
    static const char line[] = "-";
    UINT32 state, exit_code;

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
    check(handle_close(bootstrap) == OK, "the bootstrap handle did not close");
    check(handle_close(bootstrap) == ERR_BAD_HANDLE,
          "a closed handle closed again");
    check(debug_write(HANDLE_INVALID, line, 1) == ERR_BAD_HANDLE,
          "HANDLE_INVALID named a handle");
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
    UINT32 self;

    (void)argument;
    read_bootstrap(bootstrap, &self);
    say("nova-init: running in user mode, bootstrap message read\r\n");
    check_errors(bootstrap, self);
    check_clock();
    say("nova-init: checks passed\r\n");
    process_exit(EXIT_CHECKS_PASSED);
}
