/* KTAP results (ktap.h). Each line is written with log.h, which writes
   it whole once it ends. */

#include "ktap.h"
#include "test_report.h"
#include "log.h"

static void indent(UINT32 depth)
{
    UINT32 i;

    for (i = 0; i < depth; i++)
        say(KTAP_INDENT);
}

void ktap_plan(KTAP_PLAN *plan, UINT32 depth, UINT32 count)
{
    plan->depth = depth;
    plan->written = 0;
    indent(depth);
    say(KTAP_VERSION_LINE "\r\n");
    indent(depth);
    say("1..");
    say_number(count);
    say("\r\n");
}

void ktap_diagnostic_start(const KTAP_PLAN *plan)
{
    indent(plan->depth);
    say(KTAP_DIAGNOSTIC);
}

void ktap_diagnostic(const KTAP_PLAN *plan, const char *text)
{
    ktap_diagnostic_start(plan);
    say(text);
    say("\r\n");
}

void ktap_skip_rest(KTAP_PLAN *plan, const char *const *names, UINT32 count,
                    const char *reason)
{
    while (plan->written < count)
        ktap_result(plan, KTAP_SKIPPED, names[plan->written], reason);
}

void ktap_result(KTAP_PLAN *plan, KTAP_RESULT result, const char *name,
                 const char *data)
{
    indent(plan->depth);
    say(result == KTAP_FAILED ? "not ok " : "ok ");
    say_number(++plan->written);
    say(" ");
    say(name);
    if (data) {
        say(KTAP_DATA);
        if (result == KTAP_SKIPPED)
            say(KTAP_SKIP);
        say(data);
    }
    say("\r\n");
}
