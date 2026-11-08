/* The kernel's self-tests' lines (selftest.h). The object and thread
   self-tests run once threads do, so each line is written under the
   port's lock, as serial.h has every such line written. */

#include "selftest.h"
#include "test_report.h"
#include "object/process.h"
#include "x86_64/serial.h"

/* The disk the kernel booted from: the system's, or a test disk, which
   runs every self-test and reports in KTAP. */
typedef enum { DISK_SYSTEM, DISK_TEST } DISK;

static const char *const names[SELFTEST_COUNT + 1] = {
    [SELFTEST_PAGE] = "page",
    [SELFTEST_RANDOM] = "random",
    [SELFTEST_OBJECT] = "object",
    [SELFTEST_THREAD] = "thread",
};

static DISK disk = DISK_SYSTEM;
static int any_failed;

static void indent(UINT32 depth)
{
    UINT32 i;

    for (i = 0; i < depth; i++)
        serial_write(KTAP_INDENT);
}

static void ktap_line(UINT32 depth, const char *text)
{
    indent(depth);
    serial_write(text);
    serial_write("\r\n");
}

static void ktap_plan(UINT32 depth, UINT64 count)
{
    ktap_line(depth, KTAP_VERSION_LINE);
    indent(depth);
    serial_write("1..");
    serial_number(count);
    serial_write("\r\n");
}

static void ktap_result(UINT32 depth, int passed, UINT64 number,
                        const char *name)
{
    indent(depth);
    serial_write(passed ? "ok " : "not ok ");
    serial_number(number);
    serial_write(" ");
    serial_write(name);
    serial_write("\r\n");
}

void selftest_begin(const BOOT_INFO *info)
{
    if (!boot_file_find(info, TEST_DISK_MARKER))
        return;
    disk = DISK_TEST;
    ktap_plan(KTAP_TOP_DEPTH, KTAP_TOP_TESTS);
    ktap_plan(KTAP_TEST_DEPTH, SELFTEST_COUNT);
}

int selftest_runs(SELFTEST test)
{
    return disk == DISK_TEST || test == SELFTEST_RANDOM;
}

void selftest_passed(SELFTEST test)
{
    UINT64 flags = serial_lock();

    if (disk == DISK_TEST) {
        ktap_result(KTAP_TEST_DEPTH, 1, test, names[test]);
    } else {
        serial_write("nova: selftest passed name=");
        serial_write(names[test]);
        serial_write("\r\n");
    }
    serial_unlock(flags);
}

static void ktap_diagnostic(UINT32 depth, const char *text)
{
    indent(depth);
    serial_write(KTAP_DIAGNOSTIC);
    serial_write(text);
    serial_write("\r\n");
}

void selftest_note(const char *text)
{
    UINT64 flags;

    if (disk != DISK_TEST)
        return;
    flags = serial_lock();
    ktap_diagnostic(KTAP_TEST_DEPTH, text);
    serial_unlock(flags);
}

void selftest_failed(SELFTEST test, const char *reason)
{
    UINT64 flags = serial_lock();

    any_failed = 1;
    if (disk == DISK_TEST) {
        ktap_diagnostic(KTAP_TEST_DEPTH, reason);
        ktap_result(KTAP_TEST_DEPTH, 0, test, names[test]);
    } else {
        serial_write("nova: selftest failed name=");
        serial_write(names[test]);
        serial_write(" reason=");
        serial_value(reason);
        serial_write("\r\n");
    }
    serial_unlock(flags);
}

_Noreturn void selftest_halt(SELFTEST test, const char *reason)
{
    selftest_failed(test, reason);
    serial_halt();
}

void selftest_end(void)
{
    UINT64 flags;

    if (disk != DISK_TEST)
        return;
    flags = serial_lock();
    ktap_result(KTAP_TOP_DEPTH, !any_failed, KTAP_KERNEL_TEST,
                KTAP_KERNEL_NAME);
    serial_unlock(flags);
}
