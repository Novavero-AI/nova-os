/* The kernel's self-tests and what each writes to the log
   (docs/spec/log.md T2, T3). On a test disk every self-test runs, each a
   case of the report's first test, kernel; on the system disk only
   SELFTEST_RANDOM runs, and writes a selftest line. */

#ifndef NOVA_SELFTEST_H
#define NOVA_SELFTEST_H

#include "boot_info.h"

/* The self-tests, numbered in the order they run, which is their order
   in the report. */
typedef enum {
    SELFTEST_PAGE = 1,
    SELFTEST_RANDOM,
    SELFTEST_OBJECT,
    SELFTEST_THREAD
} SELFTEST;

#define SELFTEST_COUNT SELFTEST_THREAD

/* Read from the boot files whether the boot disk is a test disk, and on
   one write the report's version line and plan and the kernel test's. */
void selftest_begin(const BOOT_INFO *info);

/* Whether test runs on this boot. */
int selftest_runs(SELFTEST test);

/* Report test passed. */
void selftest_passed(SELFTEST test);

/* Write text as a diagnostic of the self-test running, before its
   result: a part of it that cannot run on this machine. */
void selftest_note(const char *text);

/* Report test failed for reason, and go on. */
void selftest_failed(SELFTEST test, const char *reason);

/* Report test failed for reason, and halt. */
_Noreturn void selftest_halt(SELFTEST test, const char *reason);

/* After the last self-test, on a test disk, write the kernel test's
   result: ok if every self-test passed. */
void selftest_end(void);

#endif
