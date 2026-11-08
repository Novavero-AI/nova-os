/** @file
    A test report's results in KTAP version 1 (docs/spec/log.md T2),
    written to the debug log through log.h, each line whole. A list of
    tests writes its plan, then each test's result in order, a failure's
    diagnostics before it. */

#ifndef NOVA_LIB_KTAP_H
#define NOVA_LIB_KTAP_H

#include "native_abi.h"

/** A list of tests at one depth of the report, and how far it has got. */
typedef struct {
    UINT32 depth;   /**< levels of indentation its lines take */
    UINT32 written; /**< results written, the last one's number */
} KTAP_PLAN;

/** How a test ended. */
typedef enum {
    KTAP_PASSED, /**< ok */
    KTAP_FAILED, /**< not ok */
    KTAP_SKIPPED /**< ok, with the SKIP directive and its reason */
} KTAP_RESULT;

/** Write the version line and the plan, count tests, at depth, and
    start plan with no result written. */
void ktap_plan(KTAP_PLAN *plan, UINT32 depth, UINT32 count);

/** Start a diagnostic line at plan's depth, its indentation and its
    hash sign; the caller writes the rest with log.h and ends the line. */
void ktap_diagnostic_start(const KTAP_PLAN *plan);

/** Write a diagnostic line at plan's depth that holds text. */
void ktap_diagnostic(const KTAP_PLAN *plan, const char *text);

/** Report each test from the next to the count-th, of those names
    names, skipped for reason: the tests a failure leaves unreached. */
void ktap_skip_rest(KTAP_PLAN *plan, const char *const *names, UINT32 count,
                    const char *reason);

/** Write the next test's result, named name, with data after its hash
    sign unless data is 0: for KTAP_SKIPPED, the skip's reason, after the
    SKIP directive; for another result, what it found. */
void ktap_result(KTAP_PLAN *plan, KTAP_RESULT result, const char *name,
                 const char *data);

#endif
