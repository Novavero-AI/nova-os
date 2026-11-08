/** @file
    The layout of a boot's test report, in KTAP version 1, that the kernel
    and the first process share (docs/spec/log.md T2): what makes a boot
    disk a test disk, the report's two top-level tests, and the depths
    their tests and cases are written at. */

#ifndef NOVA_TEST_REPORT_H
#define NOVA_TEST_REPORT_H

/** The boot file, in `\nova\`, whose presence makes the boot disk a test
    disk, whatever it holds (docs/spec/disk-layout.md). */
#define TEST_DISK_MARKER "TESTS"

/** The line that opens a report and each subtest's results. */
#define KTAP_VERSION_LINE "KTAP version 1"

/** One level of a subtest's indentation. */
#define KTAP_INDENT "  "

/** The prefix of a diagnostic line, after its indentation. */
#define KTAP_DIAGNOSTIC "# "

/** What separates a result from the directive or data that follows its
    name. */
#define KTAP_DATA " # "

/** The directive of a test that did not run, before its reason. */
#define KTAP_SKIP "SKIP "

/** @name The report's top-level tests
    The kernel writes the report's plan and its own test's result; the
    first process writes its test's result.
    @{ */
#define KTAP_TOP_TESTS 2        /**< the top-level plan's count */
#define KTAP_KERNEL_TEST 1      /**< the kernel's self-tests */
#define KTAP_INIT_TEST 2        /**< the first process's tests */
#define KTAP_KERNEL_NAME "kernel" /**< the kernel test's name */
#define KTAP_INIT_NAME "init"   /**< the first process's test's name */
/** @} */

/** @name Depths
    A top-level test's tests are written one level in, and a test's own
    cases, a test program's among them, two levels in.
    @{ */
#define KTAP_TOP_DEPTH 0  /**< the report's own plan and results */
#define KTAP_TEST_DEPTH 1 /**< a top-level test's tests */
#define KTAP_CASE_DEPTH 2 /**< a test's cases */
/** @} */

#endif
