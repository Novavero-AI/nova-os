/* Checks on the host what the compositor's frames are counted as and
   what its budget lines say (lib/budget.c, docs/spec/display.md D19's
   phase 3 arrangement, rule 8). Every expectation is written here from
   the rule's text and not taken from the library:

   - the change a frame is counted under, for every combination of what
     damaged it, against a table of the rule's two kinds: a frame a
     window's present damaged, whatever else did, and a frame the
     pointer's motion alone damaged;
   - the line, byte for byte, against one built here with snprintf from
     the frames given: its keys in the rule's order, frames the count, pixels
     the most one frame composed, total_ms the times together and max_ms
     the longest, each in milliseconds to the tenth, rounded down;
   - that a line is written at each BUDGET_FRAMES frames of a kind and at
     no other frame, that a line counts only the frames since the kind's
     last, that the two kinds are counted apart, and that a frame of
     neither kind is counted nowhere, over the cases below and over a
     sequence of frames of every kind with pixels and times drawn at
     random, against a count kept here;
   - the rule's own example line, from frames that give its figures;
   - the longest line there is, which is as long as budget.h says and
     within one write of the log, so that it is written whole.

   It does not show which frames the compositor gives the library, or
   their pixels and times: compositor/serve.c's frame is read and run by
   the boots, and tools/check-compose.c checks the pixels a frame counts.

   Prints each failure and a count, and exits nonzero if any check fails.

   cc -std=c11 -Wall -Wextra -pedantic -I include -I lib \
     -o check-budget tools/check-budget.c lib/budget.c lib/line.c \
     && ./check-budget */

#include <stdio.h>
#include <string.h>

#include "budget.h"

/* The frames a budget line counts, as display.md D19's rule 8 gives
   BUDGET_FRAMES, which the library's constant is checked against. */
#define RULE_FRAMES 16

/* A time is logged in milliseconds to the tenth, rounded down
   (docs/spec/log.md T1): the nanoseconds in a tenth of a millisecond. */
#define NS_PER_TENTH 100000ULL
#define TENTHS_PER_MS 10ULL

/* The bytes of the longest line there is, its "\r\n" among them, as
   budget.h gives them: the 65 bytes of its words and keys, the kind's 7,
   the 2 digits of its frames, the 20 digits of the greatest UINT64 for
   its pixels, for each of its two times the 14 digits of that many
   nanoseconds in whole milliseconds, a point and a tenth, and the 2
   bytes of its end. */
#define LONGEST_BYTES 128

/* The greatest UINT64, for the pixels and a time of the longest line. */
#define GREATEST 0xFFFFFFFFFFFFFFFFULL

/* Two values that are no BUDGET_CHANGE_ value: the one past the last, and
   the greatest a change can hold. */
#define CHANGE_PAST_LAST (BUDGET_CHANGE_PRESENT + 1)
#define CHANGE_GREATEST 0xFFFFFFFFu

/* The frames of the sequence drawn at random, and the bounds of a
   frame's pixels and nanoseconds in it: up to a screen of 2880 x 1800,
   and up to two frames of 16.7 ms. */
#define SEQUENCE_FRAMES 5000
#define SEQUENCE_PIXELS 5184000ULL
#define SEQUENCE_NS 33333334ULL

/* The lines a write function was given, in order. */
#define LINES_MAX 1024
#define LINE_BYTES_MAX 512
static char lines[LINES_MAX][LINE_BYTES_MAX];
static unsigned line_count;
static unsigned checks;
static unsigned failures;

/* Count a check, and print what if it did not pass. */
static void check(int passed, const char *what)
{
    checks++;
    if (passed)
        return;
    printf("failed: %s\n", what);
    failures++;
}

static void collect(const char *line)
{
    if (line_count < LINES_MAX && strlen(line) < LINE_BYTES_MAX)
        strcpy(lines[line_count++], line);
    else
        failures++;
}

/* ------------------------------------------------------------------------
   The change a frame is counted under. */

/* Rule 8's two kinds, for each combination of what damaged a frame: a
   window's present, the frame's one rectangle, which a present's damage
   and any other, as a closing window's, lie in, and the pointer's
   motion. A frame a present damaged is the present's, whatever else
   damaged it. A frame the pointer's motion damaged is the pointer's only
   where the motion alone did, which is where the rectangle holds no
   pixel. Every other frame is neither's. */
static const struct {
    UINT32 presented, rectangle, pointer, wanted;
} changes[] = {
    {0, 0, 0, BUDGET_CHANGE_NONE},    /* nothing composed: a frame owed */
    {0, 0, 1, BUDGET_CHANGE_POINTER}, /* the pointer's motion alone */
    {0, 1, 0, BUDGET_CHANGE_NONE},    /* a closing window alone */
    {0, 1, 1, BUDGET_CHANGE_NONE},    /* the pointer and a closing window */
    {1, 0, 0, BUDGET_CHANGE_PRESENT}, /* a present */
    {1, 0, 1, BUDGET_CHANGE_PRESENT}, /* a present and the pointer */
    {1, 1, 0, BUDGET_CHANGE_PRESENT}, /* a present */
    {1, 1, 1, BUDGET_CHANGE_PRESENT}, /* a present and the pointer */
};
#define CHANGES (sizeof changes / sizeof changes[0])

/* A nonzero value other than 1, which says what 1 does. */
#define TRUE_OTHER 0x80000000u

static void check_changes(void)
{
    unsigned i, wrong = 0, wrong_other = 0;

    check(BUDGET_CHANGE_NONE != BUDGET_CHANGE_POINTER &&
              BUDGET_CHANGE_NONE != BUDGET_CHANGE_PRESENT &&
              BUDGET_CHANGE_POINTER != BUDGET_CHANGE_PRESENT,
          "the three changes are three values");
    for (i = 0; i < CHANGES; i++) {
        wrong += budget_change(changes[i].presented, changes[i].rectangle,
                               changes[i].pointer) != changes[i].wanted;
        wrong_other += budget_change(changes[i].presented * TRUE_OTHER,
                                     changes[i].rectangle * TRUE_OTHER,
                                     changes[i].pointer * TRUE_OTHER) !=
                       changes[i].wanted;
    }
    check(wrong == 0, "a frame is counted under the change rule 8 gives for "
                      "what damaged it");
    check(wrong_other == 0, "any nonzero value says a thing damaged the "
                            "frame");
}

/* ------------------------------------------------------------------------
   The line. */

/* What rule 8 has a line say of the frames of one kind since its last,
   kept here apart from the library's tally. */
typedef struct {
    unsigned frames;
    UINT64 pixels, total_ns, max_ns;
} COUNT;

static const COUNT no_count = {0, 0, 0, 0};

static void count_frame(COUNT *count, UINT64 pixels, UINT64 nanoseconds)
{
    count->frames++;
    if (pixels > count->pixels)
        count->pixels = pixels;
    count->total_ns += nanoseconds;
    if (nanoseconds > count->max_ns)
        count->max_ns = nanoseconds;
}

/* Rule 8's line for the frames count holds, of the kind named name, as
   the log takes it: its keys in the rule's order and each time in
   milliseconds to the tenth, rounded down. */
static void wanted_line(char line[LINE_BYTES_MAX], const char *name,
                        const COUNT *count)
{
    UINT64 total = count->total_ns / NS_PER_TENTH;
    UINT64 longest = count->max_ns / NS_PER_TENTH;

    snprintf(line, LINE_BYTES_MAX,
             "nova-compositor: budget change=%s frames=%u pixels=%llu "
             "total_ms=%llu.%llu max_ms=%llu.%llu\r\n",
             name, count->frames, count->pixels, total / TENTHS_PER_MS,
             total % TENTHS_PER_MS, longest / TENTHS_PER_MS,
             longest % TENTHS_PER_MS);
}

/* Give the library frames frames of change, each of pixels pixels and
   nanoseconds nanoseconds. */
static void give(BUDGET *budget, UINT32 change, unsigned frames, UINT64 pixels,
                 UINT64 nanoseconds)
{
    unsigned i;

    for (i = 0; i < frames; i++)
        budget_frame(budget, change, pixels, nanoseconds, collect);
}

/* Whether exactly one line has been written since the lines were last
   cleared, and it is wanted; the lines are then cleared. */
static int wrote(const char *wanted)
{
    int right = line_count == 1 && strcmp(lines[0], wanted) == 0;

    if (line_count == 1 && !right)
        printf("wrote:  %swanted: %s", lines[0], wanted);
    line_count = 0;
    return right;
}

/* Rule 8's example: frames the pointer's motion alone damaged, the most
   pixels of one 4,992, their times 10.0 ms together and the longest
   0.6 ms. Fifteen frames of 624,000 ns and one of 690,000 ns are
   10,050,000 ns together. */
static void check_example(void)
{
    static BUDGET budget;

    line_count = 0;
    give(&budget, BUDGET_CHANGE_POINTER, RULE_FRAMES - 1, 4992, 624000);
    check(line_count == 0, "no line is written before BUDGET_FRAMES frames "
                           "of a kind");
    give(&budget, BUDGET_CHANGE_POINTER, 1, 2496, 690000);
    check(wrote("nova-compositor: budget change=pointer frames=16 "
                "pixels=4992 total_ms=10.0 max_ms=0.6\r\n"),
          "the frames of rule 8's example write its line");
}

/* Times round down to the tenth: 16 frames of 6,249 ns are 99,984 ns,
   under a tenth of a millisecond, and 16 of 6,250 ns are 100,000 ns, a
   tenth, whose longest is still under one; 1,999,999 ns is 1.9 ms. */
static void check_rounding(void)
{
    static BUDGET budget;

    line_count = 0;
    give(&budget, BUDGET_CHANGE_PRESENT, RULE_FRAMES, 1, 6249);
    check(wrote("nova-compositor: budget change=present frames=16 pixels=1 "
                "total_ms=0.0 max_ms=0.0\r\n"),
          "times under a tenth of a millisecond are 0.0");
    give(&budget, BUDGET_CHANGE_PRESENT, RULE_FRAMES, 1, 6250);
    check(wrote("nova-compositor: budget change=present frames=16 pixels=1 "
                "total_ms=0.1 max_ms=0.0\r\n"),
          "times that reach a tenth of a millisecond together are 0.1");
    give(&budget, BUDGET_CHANGE_PRESENT, RULE_FRAMES - 1, 1, 0);
    give(&budget, BUDGET_CHANGE_PRESENT, 1, 1, 1999999);
    check(wrote("nova-compositor: budget change=present frames=16 pixels=1 "
                "total_ms=1.9 max_ms=1.9\r\n"),
          "a time is rounded down to the tenth");
    give(&budget, BUDGET_CHANGE_PRESENT, RULE_FRAMES, 0, 0);
    check(wrote("nova-compositor: budget change=present frames=16 pixels=0 "
                "total_ms=0.0 max_ms=0.0\r\n"),
          "frames of no pixel and no time write zeros");
}

/* A line counts the frames since the kind's last and no earlier one: a
   second line after a larger frame and a longer one shows neither. */
static void check_starts_again(void)
{
    static BUDGET budget;

    line_count = 0;
    give(&budget, BUDGET_CHANGE_POINTER, 1, 9000, 5000000);
    give(&budget, BUDGET_CHANGE_POINTER, RULE_FRAMES - 1, 100, 200000);
    check(wrote("nova-compositor: budget change=pointer frames=16 "
                "pixels=9000 total_ms=8.0 max_ms=5.0\r\n"),
          "a line holds the most pixels and the longest time of its frames");
    give(&budget, BUDGET_CHANGE_POINTER, RULE_FRAMES, 100, 200000);
    check(wrote("nova-compositor: budget change=pointer frames=16 pixels=100 "
                "total_ms=3.2 max_ms=0.2\r\n"),
          "the next line counts only the frames since the last");
}

/* The two kinds are counted apart, and a frame of neither kind, or of no
   kind there is, is counted nowhere: 15 frames of each kind, with frames
   of neither among them, write nothing, and the sixteenth of each writes
   its own kind's figures. */
static void check_kinds_apart(void)
{
    static BUDGET budget;
    unsigned i;

    line_count = 0;
    for (i = 0; i < RULE_FRAMES - 1; i++) {
        give(&budget, BUDGET_CHANGE_POINTER, 1, 300, 100000);
        give(&budget, BUDGET_CHANGE_NONE, 1, 7777777, 900000000);
        give(&budget, BUDGET_CHANGE_PRESENT, 1, 50000, 2000000);
        give(&budget, CHANGE_PAST_LAST, 1, 8888888, 800000000);
        give(&budget, CHANGE_GREATEST, 1, 9999999, 700000000);
    }
    check(line_count == 0, "frames of neither kind bring no line nearer");
    give(&budget, BUDGET_CHANGE_PRESENT, 1, 50000, 2000000);
    check(wrote("nova-compositor: budget change=present frames=16 "
                "pixels=50000 total_ms=32.0 max_ms=2.0\r\n"),
          "a present's line counts the presents' frames alone");
    give(&budget, BUDGET_CHANGE_POINTER, 1, 300, 100000);
    check(wrote("nova-compositor: budget change=pointer frames=16 pixels=300 "
                "total_ms=1.6 max_ms=0.1\r\n"),
          "the pointer's line counts the pointer's frames alone");
}

/* The longest line: the greatest pixels, and one frame of the greatest
   time among 15 of none, so that the times together and the longest are
   both 18,446,744,073,709,551,615 ns, 18,446,744,073,709.5 ms. */
static void check_longest(void)
{
    static BUDGET budget;
    static const char wanted[] =
        "nova-compositor: budget change=pointer frames=16 "
        "pixels=18446744073709551615 total_ms=18446744073709.5 "
        "max_ms=18446744073709.5\r\n";

    line_count = 0;
    give(&budget, BUDGET_CHANGE_POINTER, RULE_FRAMES - 1, GREATEST, 0);
    give(&budget, BUDGET_CHANGE_POINTER, 1, 0, GREATEST);
    check(strlen(wanted) == LONGEST_BYTES,
          "the longest line is the 128 bytes budget.h says");
    check(sizeof wanted <= DEBUG_WRITE_MAX_BYTES,
          "the longest line and its NUL are within one write of the log");
    check(wrote(wanted), "the longest line is written whole");
}

/* The generator of the sequence's frames: a 64-bit linear congruential
   generator with Knuth's MMIX multiplier and increment, whose high bits
   are used. */
static UINT64 random_state = 0x9E3779B97F4A7C15ULL;

static UINT64 random_below(UINT64 n)
{
    random_state =
        random_state * 6364136223846793005ULL + 1442695040888963407ULL;
    return (random_state >> 20) % n;
}

/* A sequence of frames of every change, each with pixels and a time
   drawn at random: a line is written exactly when the count kept here
   reaches RULE_FRAMES frames of a kind, and is the line that count
   gives. */
static void check_sequence(void)
{
    static const UINT32 kinds[] = {BUDGET_CHANGE_NONE, BUDGET_CHANGE_POINTER,
                                   BUDGET_CHANGE_PRESENT};
    static const char *const names[] = {0, "pointer", "present"};
    static BUDGET budget;
    COUNT counts[sizeof kinds / sizeof kinds[0]];
    unsigned frame, k, written = 0, wrong = 0, stray = 0;

    for (k = 0; k < sizeof kinds / sizeof kinds[0]; k++)
        counts[k] = no_count;
    line_count = 0;
    for (frame = 0; frame < SEQUENCE_FRAMES; frame++) {
        UINT64 pixels = random_below(SEQUENCE_PIXELS + 1);
        UINT64 nanoseconds = random_below(SEQUENCE_NS + 1);
        char wanted[LINE_BYTES_MAX];

        k = (unsigned)random_below(sizeof kinds / sizeof kinds[0]);
        budget_frame(&budget, kinds[k], pixels, nanoseconds, collect);
        if (kinds[k] != BUDGET_CHANGE_NONE)
            count_frame(&counts[k], pixels, nanoseconds);
        if (counts[k].frames == RULE_FRAMES) {
            wanted_line(wanted, names[k], &counts[k]);
            wrong += line_count != 1 || strcmp(lines[0], wanted) != 0;
            written++;
            counts[k] = no_count;
        } else {
            stray += line_count != 0;
        }
        line_count = 0;
    }
    check(written > SEQUENCE_FRAMES / (3 * RULE_FRAMES),
          "the sequence reaches a line of each kind many times");
    check(wrong == 0, "each BUDGET_FRAMES frames of a kind write the line "
                      "rule 8 gives for them");
    check(stray == 0, "no other frame writes a line");
}

int main(void)
{
    check(BUDGET_FRAMES == RULE_FRAMES, "BUDGET_FRAMES is rule 8's 16");
    check_changes();
    check_example();
    check_rounding();
    check_starts_again();
    check_kinds_apart();
    check_longest();
    check_sequence();
    printf("%u checks, %u failed\n", checks, failures);
    return failures != 0;
}
