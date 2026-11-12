/* The first process's tests (docs/spec/log.md T2). On a test disk they
   are the init test of the boot's report, the second of its two
   top-level tests: each runs in the order INIT_TEST gives and is
   reported once, with tests_report. On any other disk none runs and
   nothing is written. The flag files a test disk may carry besides
   TESTS are read with tests_flag. */

#ifndef NOVA_TESTS_H
#define NOVA_TESTS_H

#include "ktap.h"

/* The init test's tests, numbered in the order they run. */
typedef enum {
    INIT_TEST_CALLS = 1, /* init's checks of its calls' contracts */
    INIT_TEST_FSTEST,    /* \nova\fstest.exe, the data disk's check */
    INIT_TEST_HELLO,     /* the test program from /store */
    INIT_TEST_CHAIN,     /* the bootstrap check (chain.c) */
    INIT_TEST_BOOT_DISK  /* the virtio block driver reading the boot disk */
} INIT_TEST;

#define INIT_TEST_COUNT INIT_TEST_BOOT_DISK

/* Read from the boot files whether the boot disk is a test disk, and on
   one write the init test's version line and plan. */
void tests_begin(void);

/* Whether the boot disk is a test disk. */
int tests_enabled(void);

/* The flag file that sets ui.reduce-motion on a test disk (disk-layout.md,
   display.md D19). */
#define TEST_FLAG_NO_MOTION "NOMOTION"

/* The flag file that makes the compositor end just after the first frame
   that shows a window's present, each time it starts, on a test disk
   (disk-layout.md, display.md D19). */
#define TEST_FLAG_COMPOSITOR_FAILS "COMPFAIL"

/* The flag file that makes the compositor answer no COMPOSITOR_CONNECT,
   each time it starts, on a test disk, so that the first process's wait
   for the answer meets its deadline (disk-layout.md, display.md D19). */
#define TEST_FLAG_NO_ANSWER "NOANSWER"

/* The flag file that makes the first process start the window test
   client, which makes a window and shows two frames on a test disk, in the
   console's place as the compositor's client (test/wintest.h, display.md
   D19). */
#define TEST_FLAG_WINDOW "WINDOW"

/* Whether the boot disk is a test disk that holds the flag file name: a
   flag file acts on a test disk alone. */
int tests_flag(const char *name);

/* Start a diagnostic line of test, which the caller writes with log.h
   and ends, after reporting not run any test before it not yet
   reported, so that the line comes just before test's result. */
void tests_diagnostic_start(INIT_TEST test);

/* Write test's result: passed; failed, after reason as a diagnostic if
   it is not 0; or skipped for reason. Each test not yet reported before
   it is first reported not ok, as not run. */
void tests_report(INIT_TEST test, KTAP_RESULT result, const char *reason);

/* Write the result of test, a program that ended with exit_code: passed
   if it is passed, else failed, after a diagnostic giving the code. */
void tests_report_exit(INIT_TEST test, UINT32 exit_code, UINT32 passed);

/* Report every test not yet reported not ok, as not run, then write the
   init test's result: ok if no test failed. */
void tests_end(void);

#endif
