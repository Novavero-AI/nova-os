/* init: the first process, which the kernel starts from \nova\init.exe
   with one bootstrap handle (docs/spec/ipc.md I6). It reads the bootstrap
   message, writes to the kernel's debug log with the root system
   resource the message carries, checks the answers of the calls it has
   to the cases their contracts name, runs stage0-pe32's seed as a 32-bit
   process when the boot volume holds it (seed.c), and exits with
   EXIT_CHECKS_PASSED, which the kernel reads with process_info and logs.
   A failed check is logged, if the log is reachable, and ends the
   process with EXIT_CHECK_FAILED. */

#include "init.h"

/* A nonzero code, so that the kernel's log shows the value process_exit
   passed and not a default. */
#define EXIT_CHECKS_PASSED 42
#define EXIT_CHECK_FAILED 1

/* The handles the bootstrap message carries (native_abi.h): the root
   system resource, the process itself, the root job, then the boot
   files. */
#define BOOTSTRAP_HANDLES (BOOTSTRAP_FIRST_FILE + BOOT_FILES_MAX)
#define BOOTSTRAP_RESOURCE 0
#define BOOTSTRAP_SELF 1
#define BOOTSTRAP_JOB 2

/* An address in the kernel's half, and one in user memory that nothing
   maps (the first page of user memory, below the image). */
#define KERNEL_ADDRESS 0xffff800000000000ULL
#define UNMAPPED_USER_ADDRESS 0x10000ULL

/* Where init maps sections into itself: above its image and below its
   stack, each mapping at the next free address. */
#define SCRATCH_BASE 0x0000000200000000ULL
#define PAGE_SIZE 4096
#define DECIMAL_DIGITS 20

static UINT32 debug_log = HANDLE_INVALID;
static UINT32 self = HANDLE_INVALID;
static UINT64 scratch = SCRATCH_BASE;
static BOOT_FILE_VIEW files[BOOT_FILES_MAX];
static UINT32 file_count;

static UINT64 length(const char *s)
{
    UINT64 n = 0;

    while (s[n])
        n++;
    return n;
}

void say(const char *line)
{
    debug_write(debug_log, line, length(line));
}

void say_number(UINT64 n)
{
    char digits[DECIMAL_DIGITS + 1];
    UINT32 at = DECIMAL_DIGITS;

    digits[at] = 0;
    do {
        digits[--at] = (char)('0' + n % 10);
        n /= 10;
    } while (n);
    say(digits + at);
}

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

UINT8 *map_into_init(UINT32 section, UINT64 size, UINT32 protection)
{
    UINT64 address = scratch;

    size = (size + PAGE_SIZE - 1) / PAGE_SIZE * PAGE_SIZE;
    if (process_map(self, section, 0, address, size, protection) != OK)
        return 0;
    scratch += size;
    return (UINT8 *)address;
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

/* Take the debug log's resource, the process's own handle, the root job
   and the boot files from the bootstrap message, checking that a buffer
   too small for its handles leaves it queued and reports how much it
   holds; that read whole it holds them in that order, with a name and
   size for each file; and that once it is read the queue is empty and
   the kernel's end closed. */
static UINT32 read_bootstrap(UINT32 bootstrap)
{
    static BOOTSTRAP_FILE names[BOOT_FILES_MAX];
    RECEIVED_HANDLE handles[BOOTSTRAP_HANDLES];
    UINT32 byte_count, handle_count, i;

    if (channel_read(bootstrap, names, sizeof names, handles, 0, &byte_count,
                     &handle_count) != ERR_BUFFER_TOO_SMALL ||
        handle_count < BOOTSTRAP_FIRST_FILE)
        process_exit(EXIT_CHECK_FAILED);
    if (channel_read(bootstrap, names, sizeof names, handles,
                     BOOTSTRAP_HANDLES, &byte_count, &handle_count) != OK ||
        handle_count < BOOTSTRAP_FIRST_FILE ||
        byte_count != (handle_count - BOOTSTRAP_FIRST_FILE) *
                          sizeof(BOOTSTRAP_FILE) ||
        handles[BOOTSTRAP_RESOURCE].type != TYPE_RESOURCE ||
        !(handles[BOOTSTRAP_RESOURCE].rights & RESOURCE_RIGHT_USE) ||
        handles[BOOTSTRAP_SELF].type != TYPE_PROCESS ||
        handles[BOOTSTRAP_JOB].type != TYPE_JOB)
        process_exit(EXIT_CHECK_FAILED);
    debug_log = handles[BOOTSTRAP_RESOURCE].handle;
    self = handles[BOOTSTRAP_SELF].handle;
    for (i = BOOTSTRAP_FIRST_FILE; i < handle_count; i++) {
        BOOT_FILE_VIEW *file = &files[file_count++];
        const BOOTSTRAP_FILE *name = &names[i - BOOTSTRAP_FIRST_FILE];
        UINT32 j;

        check(handles[i].type == TYPE_SECTION &&
                  handles[i].rights == (SECTION_RIGHT_READ | RIGHT_DUPLICATE |
                                        RIGHT_TRANSFER),
              "a boot file is not a read-only section");
        for (j = 0; j < BOOT_FILE_NAME_SIZE; j++)
            file->name[j] = name->name[j];
        file->name[BOOT_FILE_NAME_SIZE - 1] = 0;
        file->section = handles[i].handle;
        file->size = name->size;
        file->data = name->size ? map_into_init(file->section, name->size,
                                                MAP_READ)
                                : (const UINT8 *)0;
        check(!name->size || file->data, "a boot file did not map");
    }
    check(channel_read(bootstrap, names, sizeof names, handles,
                       BOOTSTRAP_HANDLES, &byte_count,
                       &handle_count) == ERR_PEER_CLOSED,
          "a drained bootstrap channel is not peer-closed");
    return handles[BOOTSTRAP_JOB].handle;
}

/* Each call checks its handle's value, type and rights in that order
   (docs/spec/objects.md O3), then its buffer, which must lie in user
   memory and be mapped (docs/spec/syscalls.md S6). No mapping of init's
   is writable and executable, nor may be: the kernel did not create it
   with PROCESS_OPTION_WRITE_EXECUTE (O5). */
static void check_errors(UINT32 bootstrap)
{
    static const char line[] = "-";
    UINT32 state, exit_code, section;

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
    check(section_create(PAGE_SIZE, 0, &section) == OK,
          "section_create failed");
    check(process_map(self, section, 0, scratch, PAGE_SIZE,
                      MAP_READ | MAP_WRITE | MAP_EXECUTE) == ERR_ACCESS_DENIED,
          "init mapped a page writable and executable");
    check(handle_close(section) == OK, "a section did not close");
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
    UINT32 job;

    (void)argument;
    job = read_bootstrap(bootstrap);
    say("nova-init: running in user mode, bootstrap message read\r\n");
    check_errors(bootstrap);
    check_clock();
    check(seed_run(job), "the seed did not reproduce itself");
    say("nova-init: checks passed\r\n");
    process_exit(EXIT_CHECKS_PASSED);
}
