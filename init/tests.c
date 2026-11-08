/* The first process's tests (tests.h). */

#include "tests.h"
#include "init.h"
#include "test_report.h"

#define NOT_RUN "not run"

/* The boot disk: the system's, or a test disk, on which the tests run
   and are reported. */
typedef enum { DISK_SYSTEM, DISK_TEST } DISK;

static const char *const names[INIT_TEST_COUNT + 1] = {
    [INIT_TEST_CALLS] = "calls",
    [INIT_TEST_FSTEST] = "fstest",
    [INIT_TEST_HELLO] = "hello",
    [INIT_TEST_CHAIN] = "chain",
    [INIT_TEST_BOOT_DISK] = "boot-disk",
};

static DISK disk = DISK_SYSTEM;
static KTAP_PLAN plan;
static int any_failed;

void tests_begin(void)
{
    if (!boot_file(TEST_DISK_MARKER))
        return;
    disk = DISK_TEST;
    ktap_plan(&plan, KTAP_TEST_DEPTH, INIT_TEST_COUNT);
}

int tests_enabled(void)
{
    return disk == DISK_TEST;
}

int tests_flag(const char *name)
{
    return disk == DISK_TEST && boot_file(name) != 0;
}

static void report_unrun(UINT32 before)
{
    while (plan.written + 1 < before) {
        any_failed = 1;
        ktap_diagnostic(&plan, NOT_RUN);
        ktap_result(&plan, KTAP_FAILED, names[plan.written + 1], 0);
    }
}

void tests_report(INIT_TEST test, KTAP_RESULT result, const char *reason)
{
    if (disk != DISK_TEST)
        return;
    report_unrun(test);
    if (result == KTAP_FAILED) {
        any_failed = 1;
        if (reason)
            ktap_diagnostic(&plan, reason);
    }
    ktap_result(&plan, result, names[test],
                result == KTAP_SKIPPED ? reason : 0);
}

void tests_diagnostic_start(INIT_TEST test)
{
    report_unrun(test);
    ktap_diagnostic_start(&plan);
}

void tests_report_exit(INIT_TEST test, UINT32 exit_code, UINT32 passed)
{
    if (disk != DISK_TEST)
        return;
    if (exit_code == passed) {
        tests_report(test, KTAP_PASSED, 0);
        return;
    }
    tests_diagnostic_start(test);
    say("exit code ");
    say_number(exit_code);
    say("\r\n");
    tests_report(test, KTAP_FAILED, 0);
}

void tests_end(void)
{
    KTAP_PLAN top;

    if (disk != DISK_TEST)
        return;
    report_unrun(INIT_TEST_COUNT + 1);
    top.depth = KTAP_TOP_DEPTH;
    top.written = KTAP_INIT_TEST - 1;
    ktap_result(&top, any_failed ? KTAP_FAILED : KTAP_PASSED, KTAP_INIT_NAME,
                0);
}
