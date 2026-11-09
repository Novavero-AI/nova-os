/* Host check of lib/backlog.c, what the compositor keeps for a window whose
   channel cannot take what it has to send: docs/spec/display.md D11, rule
   7 (the events that wait, their limit, the merging of motions, the drop,
   the state that is kept and not queued, and the stall), and the order the
   messages go in that lib/backlog.h gives.

   The channel is a function that takes a number of messages and then
   refuses, as full or as closed, so a case runs the library against a
   channel of any room. A table of cases, each with the messages the rules
   give written out, covers the edges: 63, 64 and 65 events, a motion
   merged at the limit, the ring wrapping, the state kept once however
   often it is set, the order of every permutation of the calls, a flush in
   pieces, a closed channel, and the stall at the instant it falls due.
   Random runs of pushes, state and flushes, with the channel's room and
   the time chosen at random, run beside a model of the rules written
   another way (a plain array shifted left where the library has a ring,
   flags where it has fields) and beside laws of the stream: no more than
   the limit wait, events are sent in the order they were pushed, an event
   that is never sent, a leave as much as any, is one a drop was told for,
   and a flush in any pieces sends what one flush sends. Random schedules
   of flushes check the stall in the rule's own words: a channel that took
   a message less than WINDOW_STALL_NS ago is never stalled, and one that
   takes none is stalled at the deadline and not a nanosecond before. Random
   cases come from a fixed seed, so a failure is the same one every run.

   cc -std=c11 -I include -I lib -I gfx tools/check-backlog.c lib/backlog.c */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "backlog.h"

#define FAILURES_SHOWN 12

#define SEED 0xd1b54a32d192ed03ull

#define RUNS 4000
#define RUN_STEPS 160
#define PIECES_CASES 400
#define PIECES_MOST 70

#define LOG_MAX 4096
#define UNLIMITED 0xFFFFFFFFu

#define STALL_SCHEDULES 3000
#define STALL_STEPS 40

#define STALL ((UINT64)WINDOW_STALL_NS)
#define TIME_START 1000000000ull
#define UINT64_LARGEST 0xFFFFFFFFFFFFFFFFull

static unsigned failures;
static unsigned long long checks;

static void check(int passed, const char *what, long long a, long long b)
{
    checks++;
    if (!passed && failures++ < FAILURES_SHOWN)
        printf("failed: %s (%lld, %lld)\n", what, a, b);
}

static unsigned long long random_state = SEED;

static unsigned long long next_random(void)
{
    random_state ^= random_state << 13;
    random_state ^= random_state >> 7;
    random_state ^= random_state << 17;
    return random_state;
}

static unsigned long long below(unsigned long long limit)
{
    return next_random() % limit;
}

/* ------------------------------------------------------------------------
   The channel, the events and the messages */

typedef struct {
    UINT32 room;                  /* messages it takes before it refuses */
    UINT32 refusal;               /* what it answers then: BACKLOG_SEND_FULL
                                     or BACKLOG_SEND_CLOSED */
    unsigned logged;              /* how many it took */
    BACKLOG_MESSAGE log[LOG_MAX]; /* what it took, in order */
} CHANNEL;

static UINT32 channel_send(void *context, const BACKLOG_MESSAGE *message)
{
    CHANNEL *channel = (CHANNEL *)context;

    /* A channel that has taken as many as it can log is full, so a flush
       that would send for ever ends and fails a check. */
    if (channel->logged >= LOG_MAX)
        return BACKLOG_SEND_FULL;
    if (channel->room == 0)
        return channel->refusal;
    if (channel->room != UNLIMITED)
        channel->room--;
    memcpy(&channel->log[channel->logged], message, sizeof *message);
    channel->logged++;
    return BACKLOG_SEND_TAKEN;
}

static WINDOW_INPUT key_event(UINT64 id)
{
    WINDOW_INPUT input;

    memset(&input, 0, sizeof input);
    input.kind = WINDOW_KEY;
    input.body.key.event.code = (UINT16)id;
    input.body.key.event.value = INPUT_KEY_PRESSED;
    input.body.key.event.text_length = 1;
    input.body.key.event.time = id;
    input.body.key.text[0] = 'k';
    return input;
}

static WINDOW_INPUT motion_event(UINT64 id)
{
    WINDOW_INPUT input;

    memset(&input, 0, sizeof input);
    input.kind = WINDOW_POINTER;
    input.body.pointer.x = (INT32)(id * 10);
    input.body.pointer.y = (INT32)(id * 20);
    input.body.pointer.time = id;
    return input;
}

static WINDOW_INPUT click_event(UINT64 id)
{
    WINDOW_INPUT input = motion_event(id);

    input.body.pointer.buttons = 1;
    input.body.pointer.button[0].code = 0x110;
    input.body.pointer.button[0].value = INPUT_KEY_PRESSED;
    return input;
}

static WINDOW_INPUT wheel_event(UINT64 id)
{
    WINDOW_INPUT input = motion_event(id);

    input.body.pointer.wheel = 120;
    return input;
}

static WINDOW_INPUT enter_event(UINT64 id)
{
    WINDOW_INPUT input;

    memset(&input, 0, sizeof input);
    input.kind = WINDOW_ENTER;
    input.body.enter.x = (INT32)(id * 10);
    input.body.enter.y = (INT32)(id * 20);
    input.body.enter.time = id;
    return input;
}

/* A leave has no body; the test gives it an id, in the first bytes a body
   has, which the backlog carries with the rest, so that one lost shows. */
static WINDOW_INPUT leave_event(UINT64 id)
{
    WINDOW_INPUT input;

    memset(&input, 0, sizeof input);
    input.kind = WINDOW_LEAVE;
    input.body.enter.time = id;
    return input;
}

static WINDOW_CONFIGURE_EVENT configure_of(UINT32 serial)
{
    WINDOW_CONFIGURE_EVENT configure;

    memset(&configure, 0, sizeof configure);
    configure.serial = serial;
    configure.width = 100 + serial;
    configure.height = 50 + serial;
    configure.scale = 4;
    configure.state = serial & WINDOW_STATES;
    return configure;
}

/* The id an input event carries in its time, or 0 for one that has none. */
static UINT64 id_of(const WINDOW_INPUT *input)
{
    switch (input->kind) {
    case WINDOW_KEY:
        return input->body.key.event.time;
    case WINDOW_POINTER:
        return input->body.pointer.time;
    case WINDOW_ENTER:
    case WINDOW_LEAVE:
        return input->body.enter.time;
    default:
        return 0;
    }
}

/* A message as a string: C serial, R mask, F time, D, K id, M id (a
   motion), P id (a pointer event that is not a motion), E id, L id. */
static void describe(const BACKLOG_MESSAGE *message, char *text, size_t size)
{
    const WINDOW_INPUT *input = &message->body.input;

    switch (message->kind) {
    case WINDOW_CONFIGURE:
        snprintf(text, size, "C%u", message->body.configure.serial);
        break;
    case WINDOW_RELEASED:
        snprintf(text, size, "R%u", message->body.released.buffers);
        break;
    case WINDOW_FRAME:
        snprintf(text, size, "F%llu",
                 (unsigned long long)message->body.frame.time);
        break;
    case WINDOW_DROPPED:
        snprintf(text, size, "D");
        break;
    case WINDOW_KEY:
        snprintf(text, size, "K%llu", (unsigned long long)id_of(input));
        break;
    case WINDOW_ENTER:
        snprintf(text, size, "E%llu", (unsigned long long)id_of(input));
        break;
    case WINDOW_LEAVE:
        snprintf(text, size, "L%llu", (unsigned long long)id_of(input));
        break;
    case WINDOW_POINTER:
        snprintf(text, size, "%c%llu",
                 input->body.pointer.buttons == 0 &&
                         input->body.pointer.wheel == 0
                     ? 'M'
                     : 'P',
                 (unsigned long long)id_of(input));
        break;
    default:
        snprintf(text, size, "?%u", message->kind);
        break;
    }
}

/* What a channel took, as a string. */
static void took(const CHANNEL *channel, char *text, size_t size)
{
    unsigned i;
    size_t at = 0;

    text[0] = 0;
    for (i = 0; i < channel->logged && i < LOG_MAX && at + 32 < size; i++) {
        char one[32];

        describe(&channel->log[i], one, sizeof one);
        at += (size_t)snprintf(text + at, size - at, "%s%s", i ? " " : "", one);
    }
}

static void expect_took(const CHANNEL *channel, const char *want,
                        const char *what)
{
    static char got[LOG_MAX * 8];

    took(channel, got, sizeof got);
    checks++;
    if (strcmp(got, want) != 0 && failures++ < FAILURES_SHOWN)
        printf("failed: %s: got \"%s\", want \"%s\"\n", what, got, want);
}

/* Flush with a channel of room that then answers refusal, at now. */
static UINT32 flush_ending(BACKLOG *backlog, CHANNEL *channel, UINT32 room,
                           UINT32 refusal, UINT64 now)
{
    channel->room = room;
    channel->refusal = refusal;
    channel->logged = 0;
    return backlog_flush(backlog, channel_send, channel, now);
}

/* Flush with a channel of room that is then full, at now. */
static UINT32 flush(BACKLOG *backlog, CHANNEL *channel, UINT32 room, UINT64 now)
{
    return flush_ending(backlog, channel, room, BACKLOG_SEND_FULL, now);
}

static void push_keys(BACKLOG *backlog, UINT64 first, UINT64 last)
{
    UINT64 id;

    for (id = first; id <= last; id++) {
        WINDOW_INPUT event = key_event(id);

        backlog_push(backlog, &event);
    }
}

/* Append the keys first to last to text, which holds at bytes, as "K2 K3",
   and return the bytes it holds. */
static size_t keys_text(char *text, size_t size, size_t at, UINT64 first,
                        UINT64 last)
{
    UINT64 id;

    for (id = first; id <= last; id++)
        at += (size_t)snprintf(text + at, size - at, "%sK%llu", at ? " " : "",
                               (unsigned long long)id);
    return at;
}

/* ------------------------------------------------------------------------
   The table */

/* The state goes first, in its order, whatever order it was set in. */
static void check_order(void)
{
    static const int permutations[24][4] = {
        {0, 1, 2, 3}, {0, 1, 3, 2}, {0, 2, 1, 3}, {0, 2, 3, 1},
        {0, 3, 1, 2}, {0, 3, 2, 1}, {1, 0, 2, 3}, {1, 0, 3, 2},
        {1, 2, 0, 3}, {1, 2, 3, 0}, {1, 3, 0, 2}, {1, 3, 2, 0},
        {2, 0, 1, 3}, {2, 0, 3, 1}, {2, 1, 0, 3}, {2, 1, 3, 0},
        {2, 3, 0, 1}, {2, 3, 1, 0}, {3, 0, 1, 2}, {3, 0, 2, 1},
        {3, 1, 0, 2}, {3, 1, 2, 0}, {3, 2, 0, 1}, {3, 2, 1, 0}};
    static BACKLOG backlog;
    static CHANNEL channel;
    unsigned p, i;

    for (p = 0; p < 24; p++) {
        memset(&backlog, 0, sizeof backlog);
        for (i = 0; i < 4; i++) {
            WINDOW_CONFIGURE_EVENT configure = configure_of(7);

            switch (permutations[p][i]) {
            case 0:
                backlog_configure(&backlog, &configure);
                break;
            case 1:
                backlog_release(&backlog, 5);
                break;
            case 2:
                backlog_frame(&backlog, 900);
                break;
            default:
                /* Events past the limit, so that a drop is owed too. */
                push_keys(&backlog, 1, WINDOW_BACKLOG + 2);
                break;
            }
        }
        flush(&backlog, &channel, 6, TIME_START);
        expect_took(
            &channel, "C7 R5 F900 D K3 K4",
            "the state, then the drop, then the events, in any order set");
    }
}

/* The state is kept once, as the latest. */
static void check_state(void)
{
    static BACKLOG backlog;
    static CHANNEL channel;
    WINDOW_CONFIGURE_EVENT configure;
    UINT32 status;

    memset(&backlog, 0, sizeof backlog);
    status = flush(&backlog, &channel, UNLIMITED, TIME_START);
    check(status == BACKLOG_IDLE && channel.logged == 0,
          "a new backlog owes nothing", status, channel.logged);
    check(backlog_deadline(&backlog) == BACKLOG_NO_DEADLINE,
          "and has no deadline", 0, 0);

    configure = configure_of(1);
    backlog_configure(&backlog, &configure);
    configure = configure_of(2);
    backlog_configure(&backlog, &configure);
    configure = configure_of(3);
    backlog_configure(&backlog, &configure);
    backlog_frame(&backlog, 10);
    backlog_frame(&backlog, 20);
    backlog_release(&backlog, 1);
    backlog_release(&backlog, 4);
    backlog_release(&backlog, 1);
    status = flush(&backlog, &channel, UNLIMITED, TIME_START);
    expect_took(&channel, "C3 R5 F20",
                "the latest of each, the buffers together");
    check(status == BACKLOG_IDLE, "all went", status, 0);
    check(memcmp(&channel.log[0].body.configure, &configure,
                 sizeof configure) == 0,
          "the configure sent is the latest, whole", 0, 0);
    flush(&backlog, &channel, UNLIMITED, TIME_START);
    expect_took(&channel, "", "and none is owed again");

    backlog_release(&backlog, 8);
    backlog_release(&backlog, 0);
    backlog_release(&backlog, 0xFFFFFFF8u);
    flush(&backlog, &channel, UNLIMITED, TIME_START);
    expect_took(&channel, "", "no buffer past the three is released");
    backlog_release(&backlog, 0xFFFFFFFFu);
    flush(&backlog, &channel, UNLIMITED, TIME_START);
    expect_took(&channel, "R7", "every buffer named whole, the others left");
    backlog_release(&backlog, 2);
    flush(&backlog, &channel, UNLIMITED, TIME_START);
    expect_took(&channel, "R2", "the set starts over once sent");

    configure = configure_of(4);
    backlog_configure(&backlog, &configure);
    backlog_frame(&backlog, 30);
    flush(&backlog, &channel, UNLIMITED, TIME_START);
    expect_took(&channel, "C4 F30", "state set after it went is owed again");

    backlog_frame(&backlog, 0);
    flush(&backlog, &channel, UNLIMITED, TIME_START);
    expect_took(&channel, "F0", "a frame at time 0 is a frame");

    /* A state message a full channel refuses stays owed, as the latest. */
    configure = configure_of(5);
    backlog_configure(&backlog, &configure);
    backlog_release(&backlog, 1);
    flush(&backlog, &channel, 0, TIME_START);
    expect_took(&channel, "", "a refused configure is not sent");
    configure = configure_of(6);
    backlog_configure(&backlog, &configure);
    flush(&backlog, &channel, 1, TIME_START);
    expect_took(&channel, "C6", "and is replaced by a later one");
    flush(&backlog, &channel, UNLIMITED, TIME_START);
    expect_took(&channel, "R1", "the state behind it goes next");
}

/* The limit, the drop and what it keeps. */
static void check_limit(void)
{
    static BACKLOG backlog;
    static CHANNEL channel;
    char want[LOG_MAX * 8];
    size_t at;
    UINT64 id;

    memset(&backlog, 0, sizeof backlog);
    for (id = 1; id <= WINDOW_BACKLOG; id++) {
        push_keys(&backlog, id, id);
        check(backlog.count == id, "each event waits", backlog.count,
              (long long)id);
    }
    check(backlog.dropped == 0, "WINDOW_BACKLOG events drop none", 0, 0);
    flush(&backlog, &channel, UNLIMITED, TIME_START);
    keys_text(want, sizeof want, 0, 1, WINDOW_BACKLOG);
    expect_took(&channel, want, "all of them are sent, the oldest first");

    memset(&backlog, 0, sizeof backlog);
    push_keys(&backlog, 1, WINDOW_BACKLOG + 1);
    check(backlog.count == WINDOW_BACKLOG && backlog.dropped == 1,
          "one more drops the oldest", backlog.count, backlog.dropped);
    flush(&backlog, &channel, UNLIMITED, TIME_START);
    at = (size_t)snprintf(want, sizeof want, "D");
    keys_text(want, sizeof want, at, 2, WINDOW_BACKLOG + 1);
    expect_took(&channel, want, "a drop is told before the events after it");

    /* Many drops are told once. */
    memset(&backlog, 0, sizeof backlog);
    push_keys(&backlog, 1, 200);
    flush(&backlog, &channel, UNLIMITED, TIME_START);
    at = (size_t)snprintf(want, sizeof want, "D");
    keys_text(want, sizeof want, at, 137, 200);
    expect_took(&channel, want,
                "200 events: one drop told, the newest 64 kept");

    /* A drop after one was told is told again, and one told is not told
       twice. */
    push_keys(&backlog, 1001, 1070);
    flush(&backlog, &channel, 3, TIME_START);
    expect_took(&channel, "D K1007 K1008", "told again for a later drop");
    push_keys(&backlog, 2000, 2000);
    flush(&backlog, &channel, UNLIMITED, TIME_START);
    check(channel.logged == 63 && channel.log[0].kind == WINDOW_KEY &&
              id_of(&channel.log[0].body.input) == 1009 &&
              id_of(&channel.log[62].body.input) == 2000,
          "a drop told is not told again, and the rest is in order",
          channel.logged, 0);

    /* The ring: events wait across the end of its array. */
    memset(&backlog, 0, sizeof backlog);
    push_keys(&backlog, 1, 40);
    flush(&backlog, &channel, 30, TIME_START);
    push_keys(&backlog, 41, 90);
    flush(&backlog, &channel, UNLIMITED, TIME_START);
    keys_text(want, sizeof want, 0, 31, 90);
    expect_took(&channel, want, "events wait across the ring's end, in order");
    memset(&backlog, 0, sizeof backlog);
    push_keys(&backlog, 1, WINDOW_BACKLOG);
    flush(&backlog, &channel, WINDOW_BACKLOG - 1, TIME_START);
    push_keys(&backlog, 65, 127);
    check(backlog.count == WINDOW_BACKLOG && backlog.dropped == 0,
          "one waits and 63 more come: none dropped", backlog.count,
          backlog.dropped);
    flush(&backlog, &channel, UNLIMITED, TIME_START);
    keys_text(want, sizeof want, 0, 64, 127);
    expect_took(&channel, want, "the ring, filled across its end");
    push_keys(&backlog, 128, 128 + WINDOW_BACKLOG);
    flush(&backlog, &channel, UNLIMITED, TIME_START);
    at = (size_t)snprintf(want, sizeof want, "D");
    keys_text(want, sizeof want, at, 129, 128 + WINDOW_BACKLOG);
    expect_took(&channel, want, "and a drop across its end");
}

/* Motions merge into the newest event only if it is a motion. */
static void check_merge(void)
{
    static BACKLOG backlog;
    static CHANNEL channel;
    char want[LOG_MAX * 8];
    UINT64 id;
    size_t at;

#define PUSH(event)                         \
    do {                                    \
        WINDOW_INPUT pushed = event;        \
        backlog_push(&backlog, &pushed);    \
    } while (0)
#define CASE(text, what, ...)                                    \
    do {                                                         \
        memset(&backlog, 0, sizeof backlog);                     \
        __VA_ARGS__;                                             \
        flush(&backlog, &channel, UNLIMITED, TIME_START);        \
        expect_took(&channel, text, what);                       \
    } while (0)

    CASE("M2", "two motions are one", PUSH(motion_event(1));
         PUSH(motion_event(2)));
    CASE("M5", "any number of motions are one",
         PUSH(motion_event(1)); PUSH(motion_event(2)); PUSH(motion_event(3));
         PUSH(motion_event(4)); PUSH(motion_event(5)));
    CASE("M1 K2 M3", "a key between two motions keeps both",
         PUSH(motion_event(1)); PUSH(key_event(2)); PUSH(motion_event(3)));
    CASE("K1 M4", "a motion after a key waits, and the next merges into it",
         PUSH(key_event(1)); PUSH(motion_event(2)); PUSH(motion_event(3));
         PUSH(motion_event(4)));
    CASE("P1 M2", "a motion is not merged into a wheel turn",
         PUSH(wheel_event(1)); PUSH(motion_event(2)));
    CASE("M1 P2", "a wheel turn is not merged into a motion",
         PUSH(motion_event(1)); PUSH(wheel_event(2)));
    CASE("P1 M2", "a motion is not merged into a button change",
         PUSH(click_event(1)); PUSH(motion_event(2)));
    CASE("M1 P2", "a button change is not merged into a motion",
         PUSH(motion_event(1)); PUSH(click_event(2)));
    CASE("E1 M2", "a motion is not merged into an entry",
         PUSH(enter_event(1)); PUSH(motion_event(2)));
    CASE("M1 E2 M3", "an entry between motions keeps both",
         PUSH(motion_event(1)); PUSH(enter_event(2)); PUSH(motion_event(3)));
    CASE("M1 L2 M3", "a leave between motions keeps both",
         PUSH(motion_event(1)); PUSH(leave_event(2)); PUSH(motion_event(3)));
    CASE("L1 M2", "a motion is not merged into a leave",
         PUSH(leave_event(1)); PUSH(motion_event(2)));
    CASE("L1 L2", "leaves are not merged",
         PUSH(leave_event(1)); PUSH(leave_event(2)));
    CASE("P1 P2", "wheel turns are not merged",
         PUSH(wheel_event(1)); PUSH(wheel_event(2)));

    /* A motion merged takes the newer one's place and time whole. */
    memset(&backlog, 0, sizeof backlog);
    PUSH(motion_event(1));
    PUSH(motion_event(2));
    check(backlog.count == 1, "the merged motion is one event", backlog.count,
          1);
    flush(&backlog, &channel, UNLIMITED, TIME_START);
    check(channel.log[0].body.input.body.pointer.x == 20 &&
              channel.log[0].body.input.body.pointer.y == 40 &&
              channel.log[0].body.input.body.pointer.time == 2,
          "it has the newer place and time", 0, 0);

    /* Merging happens in the backlog, whether or not a flush was tried. */
    memset(&backlog, 0, sizeof backlog);
    PUSH(motion_event(1));
    flush(&backlog, &channel, 0, TIME_START);
    PUSH(motion_event(2));
    flush(&backlog, &channel, UNLIMITED, TIME_START);
    expect_took(&channel, "M2", "a motion refused, then another: one");
    PUSH(motion_event(3));
    flush(&backlog, &channel, UNLIMITED, TIME_START);
    PUSH(motion_event(4));
    flush(&backlog, &channel, UNLIMITED, TIME_START);
    expect_took(&channel, "M4", "a motion sent is not merged into");

    /* At the limit. */
    memset(&backlog, 0, sizeof backlog);
    push_keys(&backlog, 1, WINDOW_BACKLOG);
    PUSH(motion_event(100));
    PUSH(motion_event(101));
    check(backlog.count == WINDOW_BACKLOG, "the limit holds with a motion",
          backlog.count, 0);
    flush(&backlog, &channel, UNLIMITED, TIME_START);
    at = (size_t)snprintf(want, sizeof want, "D");
    at = keys_text(want, sizeof want, at, 2, WINDOW_BACKLOG);
    snprintf(want + at, sizeof want - at, " M101");
    expect_took(&channel, want,
                "a motion at the limit drops the oldest once, the next merges");

    memset(&backlog, 0, sizeof backlog);
    push_keys(&backlog, 1, WINDOW_BACKLOG - 1);
    PUSH(motion_event(100));
    PUSH(motion_event(101));
    check(backlog.count == WINDOW_BACKLOG && backlog.dropped == 0,
          "a motion merged into the newest drops none", backlog.count,
          backlog.dropped);
    PUSH(key_event(102));
    check(backlog.dropped == 1, "and a key after it drops one", backlog.dropped,
          0);

    /* Many motions are one, and the count is of events, not of motions. */
    memset(&backlog, 0, sizeof backlog);
    for (id = 1; id <= 1000; id++)
        PUSH(motion_event(id));
    check(backlog.count == 1 && backlog.dropped == 0,
          "a thousand motions wait as one", backlog.count, backlog.dropped);
    for (id = 0; id < 200; id++) {
        PUSH(key_event(2000 + id));
        PUSH(motion_event(3000 + id));
    }
    check(backlog.count == WINDOW_BACKLOG && backlog.dropped == 1,
          "alternating keys and motions are kept to the limit", backlog.count,
          backlog.dropped);
#undef PUSH
#undef CASE
}

/* An event of a kind that is no input event's is sent once, as any is. */
static void check_odd_kind(void)
{
    static BACKLOG backlog;
    static CHANNEL channel;
    static const UINT32 kinds[] = {WINDOW_CONFIGURE, WINDOW_RELEASED,
                                   WINDOW_FRAME, WINDOW_DROPPED, 0, 0xFFFF};
    unsigned i;

    for (i = 0; i < sizeof kinds / sizeof kinds[0]; i++) {
        WINDOW_INPUT event = key_event(7);
        UINT32 status;

        memset(&backlog, 0, sizeof backlog);
        event.kind = kinds[i];
        backlog_push(&backlog, &event);
        backlog_push(&backlog, &event);
        status = flush(&backlog, &channel, UNLIMITED, TIME_START);
        check(status == BACKLOG_IDLE && channel.logged == 2 &&
                  channel.log[0].kind == kinds[i],
              "an event of any kind is sent once, as it is", kinds[i],
              channel.logged);
        check(backlog.count == 0, "and is gone", backlog.count, 0);
    }
}

/* A flush in pieces sends what one flush sends. */
static void fill_random(BACKLOG *backlog, UINT64 *id)
{
    unsigned n = (unsigned)below(150), i;

    for (i = 0; i < n; i++) {
        WINDOW_INPUT event;

        switch (below(8)) {
        case 0:
            event = click_event(++*id);
            break;
        case 1:
            event = enter_event(++*id);
            break;
        case 2:
            event = key_event(++*id);
            break;
        case 3:
            event = leave_event(++*id);
            break;
        default:
            event = motion_event(++*id);
            break;
        }
        backlog_push(backlog, &event);
    }
    if (below(2)) {
        WINDOW_CONFIGURE_EVENT configure = configure_of((UINT32)below(50) + 1);

        backlog_configure(backlog, &configure);
    }
    if (below(2))
        backlog_frame(backlog, below(1000000));
    backlog_release(backlog, (UINT32)below(8));
}

static void check_pieces(void)
{
    static BACKLOG whole, parts;
    static CHANNEL channel;
    static char all[LOG_MAX * 8], piece[LOG_MAX * 8], joined[LOG_MAX * 8];
    unsigned n;

    for (n = 0; n < PIECES_CASES; n++) {
        UINT64 id = 0, saved;
        unsigned room = 1 + (unsigned)below(PIECES_MOST);
        UINT32 status;
        unsigned flushes = 0;
        size_t at = 0;

        memset(&whole, 0, sizeof whole);
        memset(&parts, 0, sizeof parts);
        saved = random_state;
        fill_random(&whole, &id);
        random_state = saved;
        id = 0;
        fill_random(&parts, &id);

        status = flush(&whole, &channel, UNLIMITED, TIME_START);
        took(&channel, all, sizeof all);
        check(status == BACKLOG_IDLE, "one flush sends all", status, 0);

        joined[0] = 0;
        do {
            status = flush(&parts, &channel, (UINT32)room, TIME_START);
            took(&channel, piece, sizeof piece);
            flushes++;
            if (piece[0] != 0)
                at += (size_t)snprintf(joined + at, sizeof joined - at, "%s%s",
                                       at ? " " : "", piece);
            check(status == BACKLOG_IDLE || status == BACKLOG_WAITING,
                  "a refusing channel waits", status, 0);
            check(status == BACKLOG_IDLE || channel.logged == room,
                  "a flush that waits sent as many as the channel took",
                  channel.logged, room);
        } while (status != BACKLOG_IDLE && flushes < 1000);
        check(strcmp(all, joined) == 0,
              "a flush in pieces sends what one flush sends", room, flushes);
    }
}

/* The stall. */
static void check_stall(void)
{
    static BACKLOG backlog;
    static CHANNEL channel;
    UINT32 status;
    UINT64 t0 = TIME_START;

    memset(&backlog, 0, sizeof backlog);
    push_keys(&backlog, 1, 3);
    check(backlog_deadline(&backlog) == BACKLOG_NO_DEADLINE,
          "a channel not yet refused is not stalling", 0, 0);

    status = flush(&backlog, &channel, 0, t0);
    check(status == BACKLOG_WAITING, "the first refusal waits", status, 0);
    check(backlog_deadline(&backlog) == t0 + STALL,
          "its deadline is the stall after it", 0, 0);
    status = flush(&backlog, &channel, 0, t0 + STALL - 1);
    check(status == BACKLOG_WAITING, "a moment before the stall, it waits",
          status, 0);
    check(backlog_deadline(&backlog) == t0 + STALL,
          "a later refusal moves no deadline", 0, 0);
    status = flush(&backlog, &channel, 0, t0 + STALL);
    check(status == BACKLOG_STALLED, "at the stall it is stalled", status, 0);
    status = flush(&backlog, &channel, 0, t0 + 10 * STALL);
    check(status == BACKLOG_STALLED, "and after it", status, 0);
    status = flush(&backlog, &channel, 0, t0 - 5);
    check(status == BACKLOG_WAITING,
          "a time before the first refusal is no stall", status, 0);

    /* A channel that takes one has room, however long it was full. */
    status = flush(&backlog, &channel, 1, t0 + 10 * STALL);
    check(status == BACKLOG_WAITING && channel.logged == 1,
          "a channel that takes one and refuses the next waits", status,
          channel.logged);
    check(backlog_deadline(&backlog) == t0 + 11 * STALL,
          "its clock starts at the refusal after the one taken", 0, 0);
    status = flush(&backlog, &channel, 0, t0 + 11 * STALL - 1);
    check(status == BACKLOG_WAITING, "a moment before that deadline", status,
          0);
    status = flush(&backlog, &channel, UNLIMITED, t0 + 11 * STALL);
    check(status == BACKLOG_IDLE && channel.logged == 2,
          "a channel with room at the deadline is not stalled", status,
          channel.logged);
    check(backlog_deadline(&backlog) == BACKLOG_NO_DEADLINE,
          "and the backlog has no deadline", 0, 0);

    /* A refusal after a drain starts a new clock. */
    push_keys(&backlog, 4, 4);
    status = flush(&backlog, &channel, 0, t0 + 20 * STALL);
    check(status == BACKLOG_WAITING &&
              backlog_deadline(&backlog) == t0 + 21 * STALL,
          "a refusal after the backlog drained starts a new clock", status, 0);

    /* The clock's end of range. */
    memset(&backlog, 0, sizeof backlog);
    push_keys(&backlog, 1, 1);
    status = flush(&backlog, &channel, 0, UINT64_LARGEST - 5);
    check(status == BACKLOG_WAITING &&
              backlog_deadline(&backlog) == BACKLOG_NO_DEADLINE,
          "a deadline past the clock's range is none", status, 0);
    status = flush(&backlog, &channel, 0, UINT64_LARGEST);
    check(status == BACKLOG_WAITING, "and the clock does not wrap", status, 0);
}


/* A closed channel is not a full one: it is no stall, and it starts and
   stops no clock. */
static void check_closed(void)
{
    static BACKLOG backlog;
    static CHANNEL channel;
    UINT32 status;
    UINT64 t0 = TIME_START;

    memset(&backlog, 0, sizeof backlog);
    push_keys(&backlog, 1, 3);
    status = flush_ending(&backlog, &channel, 0, BACKLOG_SEND_CLOSED, t0);
    check(status == BACKLOG_CLOSED && channel.logged == 0 && backlog.count == 3,
          "a channel closed from the start is closed, and takes nothing",
          status, backlog.count);
    check(backlog_deadline(&backlog) == BACKLOG_NO_DEADLINE,
          "and starts no clock", 0, 0);
    status = flush_ending(&backlog, &channel, 0, BACKLOG_SEND_CLOSED,
                          t0 + 10 * STALL);
    check(status == BACKLOG_CLOSED, "however long after, it is no stall",
          status, 0);

    status = flush_ending(&backlog, &channel, 2, BACKLOG_SEND_CLOSED, t0);
    expect_took(&channel, "K1 K2", "a channel that closes after two took two");
    check(status == BACKLOG_CLOSED && backlog.count == 1,
          "and says closed, with the third still owed", status, backlog.count);

    /* A channel that was full and then closed: the clock stays as it was. */
    status = flush(&backlog, &channel, 0, t0 + STALL);
    check(status == BACKLOG_WAITING, "a full channel waits", status, 0);
    status = flush_ending(&backlog, &channel, 0, BACKLOG_SEND_CLOSED,
                          t0 + 5 * STALL);
    check(status == BACKLOG_CLOSED,
          "a full channel that then closes is closed, not stalled", status, 0);
    check(backlog_deadline(&backlog) == t0 + 2 * STALL,
          "and the clock it was running is as it was", 0, 0);
    status = flush(&backlog, &channel, UNLIMITED, t0 + 5 * STALL);
    expect_took(&channel, "K3", "what is owed is sent, were the channel open");
    check(status == BACKLOG_IDLE, "all went", status, 0);
}

static UINT32 send_fixed(void *context, const BACKLOG_MESSAGE *message)
{
    (void)message;
    return *(const UINT32 *)context;
}

/* What a channel's answer comes to, for every value it might give. */
static void check_send_results(void)
{
    static const UINT32 answers[] = {0,
                                     BACKLOG_SEND_TAKEN,
                                     BACKLOG_SEND_FULL,
                                     BACKLOG_SEND_CLOSED,
                                     4,
                                     7,
                                     0x80000000u,
                                     0xFFFFFFFFu};
    static BACKLOG backlog;
    unsigned i;

    for (i = 0; i < sizeof answers / sizeof answers[0]; i++) {
        UINT32 answer = answers[i];
        UINT32 status;

        memset(&backlog, 0, sizeof backlog);
        push_keys(&backlog, 1, 1);
        status = backlog_flush(&backlog, send_fixed, &answer, TIME_START);
        if (answer == BACKLOG_SEND_TAKEN)
            check(status == BACKLOG_IDLE && backlog.count == 0,
                  "taken: the message is gone", status, backlog.count);
        else if (answer == BACKLOG_SEND_CLOSED)
            check(status == BACKLOG_CLOSED && backlog.count == 1 &&
                      backlog_deadline(&backlog) == BACKLOG_NO_DEADLINE,
                  "closed: said so, with no clock", status, backlog.count);
        else
            check(status == BACKLOG_WAITING && backlog.count == 1 &&
                      backlog_deadline(&backlog) == TIME_START + STALL,
                  "full, or any other answer: the message stays owed", answer,
                  backlog.count);
    }
}

/* Random schedules of flushes, each with a message owed, judged by the
   rule's own words: the window is stalled once its channel has taken
   nothing for WINDOW_STALL_NS since it first refused one. The expected
   status is derived from what the channel took, not from the library's
   clock. */
static void check_stall_laws(void)
{
    static BACKLOG backlog;
    static CHANNEL channel;
    unsigned n, step;

    for (n = 0; n < STALL_SCHEDULES; n++) {
        /* In a steady schedule the channel takes a message in every flush
           and the flushes come under WINDOW_STALL_NS apart: the window is
           never stalled. */
        int steady = below(3) == 0;
        UINT64 now = TIME_START + below(1000);
        UINT64 last_taken = 0, refused_since = 0;
        int ever_taken = 0, refusing = 0;

        memset(&backlog, 0, sizeof backlog);
        for (step = 0; step < STALL_STEPS; step++) {
            static const UINT32 rooms[] = {0, 0, 1, 1, 2, 5, UNLIMITED};
            UINT32 room = steady ? 1 + (UINT32)below(3)
                                 : rooms[below(sizeof rooms / sizeof rooms[0])];
            UINT64 gap = steady            ? below(STALL)
                         : below(4) == 0   ? STALL - 1 + below(3)
                         : below(3) == 0   ? below(STALL * 3)
                                           : below(STALL / 4);
            UINT32 status;

            push_keys(&backlog, step + 1, step + 1);
            now += gap;
            status = flush(&backlog, &channel, room, now);
            if (channel.logged != 0) {
                last_taken = now;
                ever_taken = 1;
                refusing = 0;
            }
            if (status == BACKLOG_IDLE) {
                refusing = 0;
                check(backlog_deadline(&backlog) == BACKLOG_NO_DEADLINE,
                      "a channel that took all leaves no deadline", 0, 0);
                continue;
            }
            if (!refusing) {
                refusing = 1;
                refused_since = now;
            }
            check(status == (now - refused_since >= STALL ? BACKLOG_STALLED
                                                          : BACKLOG_WAITING),
                  "stalled once nothing was taken for the stall, not before",
                  (long long)(now - refused_since), status);
            check(backlog_deadline(&backlog) == refused_since + STALL,
                  "the deadline is the stall after the first refusal", 0, 0);
            if (ever_taken && now - last_taken < STALL)
                check(status != BACKLOG_STALLED,
                      "a channel that took a message under the stall ago is "
                      "never stalled",
                      (long long)(now - last_taken), 0);
            if (steady)
                check(status != BACKLOG_STALLED,
                      "a channel that takes a message in each flush is never "
                      "stalled",
                      step, 0);
        }
        /* A channel that takes none is stalled at the deadline, and not a
           nanosecond before. */
        if (refusing && refused_since + STALL - 1 >= now) {
            UINT32 before =
                flush(&backlog, &channel, 0, refused_since + STALL - 1);
            UINT32 at = flush(&backlog, &channel, 0, refused_since + STALL);

            check(before == BACKLOG_WAITING,
                  "a nanosecond before the deadline it waits", before, 0);
            check(at == BACKLOG_STALLED, "at the deadline it is stalled", at,
                  0);
        }
    }
}

static void check_tables(void)
{
    check_order();
    check_state();
    check_limit();
    check_merge();
    check_odd_kind();
    check_pieces();
    check_stall();
    check_closed();
    check_send_results();
    check_stall_laws();
}

/* ------------------------------------------------------------------------
   The model of the rules, written another way */

typedef struct {
    WINDOW_INPUT queue[WINDOW_BACKLOG]; /* oldest first, shifted left */
    unsigned length;
    int dropped, configured, framed;
    WINDOW_CONFIGURE_EVENT configure;
    UINT64 frame_time;
    UINT32 released;
    int full;
    UINT64 since;
} MODEL;

static int model_is_motion(const WINDOW_INPUT *input)
{
    return input->kind == WINDOW_POINTER && input->body.pointer.buttons == 0 &&
           input->body.pointer.wheel == 0;
}

static void model_push(MODEL *m, const WINDOW_INPUT *input)
{
    unsigned i;

    if (m->length > 0 && model_is_motion(input) &&
        model_is_motion(&m->queue[m->length - 1])) {
        m->queue[m->length - 1] = *input;
        return;
    }
    if (m->length == WINDOW_BACKLOG) {
        for (i = 1; i < m->length; i++)
            m->queue[i - 1] = m->queue[i];
        m->length--;
        m->dropped = 1;
    }
    m->queue[m->length++] = *input;
}

/* The message owed first, if any, with what to clear when it is taken. */
static int model_owed(const MODEL *m, BACKLOG_MESSAGE *out)
{
    memset(out, 0, sizeof *out);
    if (m->configured) {
        out->kind = WINDOW_CONFIGURE;
        out->body.configure = m->configure;
    } else if (m->released != 0) {
        out->kind = WINDOW_RELEASED;
        out->body.released.buffers = m->released;
    } else if (m->framed) {
        out->kind = WINDOW_FRAME;
        out->body.frame.time = m->frame_time;
    } else if (m->dropped) {
        out->kind = WINDOW_DROPPED;
    } else if (m->length > 0) {
        out->kind = m->queue[0].kind;
        out->body.input = m->queue[0];
    } else {
        return 0;
    }
    return 1;
}

static void model_take(MODEL *m, UINT32 kind)
{
    unsigned i;

    if (kind == WINDOW_CONFIGURE)
        m->configured = 0;
    else if (kind == WINDOW_RELEASED)
        m->released = 0;
    else if (kind == WINDOW_FRAME)
        m->framed = 0;
    else if (kind == WINDOW_DROPPED)
        m->dropped = 0;
    else {
        for (i = 1; i < m->length; i++)
            m->queue[i - 1] = m->queue[i];
        m->length--;
    }
}

/* What a flush does: the messages taken, the status it returns. A closed
   channel is said to be, with no clock touched. */
static UINT32 model_flush(MODEL *m, UINT32 room, UINT32 refusal, UINT64 now,
                          BACKLOG_MESSAGE *sent, unsigned *count)
{
    BACKLOG_MESSAGE next;

    *count = 0;
    while (model_owed(m, &next)) {
        if (room == 0) {
            if (refusal == BACKLOG_SEND_CLOSED)
                return BACKLOG_CLOSED;
            if (!m->full) {
                m->full = 1;
                m->since = now;
            }
            return now >= m->since && now - m->since >= STALL ? BACKLOG_STALLED
                                                              : BACKLOG_WAITING;
        }
        if (room != UNLIMITED)
            room--;
        sent[(*count)++] = next;
        model_take(m, next.kind);
        m->full = 0;
    }
    m->full = 0;
    return BACKLOG_IDLE;
}

static UINT64 model_deadline(const MODEL *m)
{
    if (!m->full || m->since + STALL < m->since)
        return BACKLOG_NO_DEADLINE;
    return m->since + STALL;
}

/* ------------------------------------------------------------------------
   Random runs */

typedef struct {
    UINT64 id;
    int motion;
    int sent;
} PUSHED;

#define PUSHED_MAX 1024

/* The messages a run sends at most: each event pushed, and the state and
   the drops told between flushes. */
#define STREAM_MAX (PUSHED_MAX * 8)

/* The step that ends a run, a flush of everything. */
#define FINAL_FLUSH 99

/* How far, in nanoseconds, the clock is sometimes set back. */
#define TIME_BACKWARD 5

/* Every non-motion event pushed was sent, or a drop was told between the
   events sent on either side of it. */
static void check_losses(const PUSHED *pushed, unsigned count,
                         const UINT64 *stream, unsigned stream_count)
{
    unsigned i, k;

    for (i = 0; i < count; i++) {
        int told = 0;
        int past = 0;

        if (pushed[i].sent || pushed[i].motion)
            continue;
        /* stream holds sent ids in order, with 0 for a WINDOW_DROPPED:
           look at it from the last event before this one to the first
           after. */
        for (k = 0; k < stream_count && !past; k++) {
            if (stream[k] == 0)
                told = 1;
            else if (stream[k] < pushed[i].id)
                told = 0;
            else
                past = 1;
        }
        check(told,
              "an event never sent has a drop told after the event before it",
              (long long)pushed[i].id, 0);
    }
}

/* A random event: mostly motions and keys, and each other kind. */
static WINDOW_INPUT random_event(UINT64 id, unsigned burst_kind)
{
    unsigned kind = burst_kind != 0 ? burst_kind : (unsigned)below(20);

    if (kind <= 7 || kind == 99)
        return motion_event(id);
    if (kind <= 12)
        return key_event(id);
    if (kind <= 14)
        return click_event(id);
    if (kind <= 16)
        return wheel_event(id);
    if (kind <= 18)
        return enter_event(id);
    return leave_event(id);
}

/* The kind of every event of a burst: all motions, all keys, or mixed. */
static unsigned burst_kind(void)
{
    switch (below(3)) {
    case 0:
        return 99;
    case 1:
        return 9;
    default:
        return 0;
    }
}

static void note_sent(const BACKLOG_MESSAGE *message, UINT64 *last,
                      UINT64 *stream, unsigned *stream_count)
{
    UINT64 id;

    if (message->kind == WINDOW_DROPPED) {
        stream[(*stream_count)++] = 0;
        return;
    }
    if (message->kind == WINDOW_CONFIGURE || message->kind == WINDOW_RELEASED ||
        message->kind == WINDOW_FRAME)
        return;
    id = id_of(&message->body.input);
    check(id > *last, "events are sent in the order they were pushed",
          (long long)id, (long long)*last);
    *last = id;
    stream[(*stream_count)++] = id;
}

static void run_one(void)
{
    static BACKLOG backlog;
    static MODEL model;
    static CHANNEL channel;
    static BACKLOG_MESSAGE expected[LOG_MAX];
    static PUSHED pushed[PUSHED_MAX];
    static UINT64 stream[STREAM_MAX];
    unsigned pushed_count = 0, stream_count = 0, step, i, expected_count;
    UINT64 id = 0, now = TIME_START, last_sent = 0;

    memset(&backlog, 0, sizeof backlog);
    memset(&model, 0, sizeof model);
    for (step = 0; step <= RUN_STEPS; step++) {
        unsigned action = step == RUN_STEPS ? FINAL_FLUSH : (unsigned)below(40);

        if (action < 18) {
            unsigned burst = below(12) == 0 ? 50 + (unsigned)below(100) : 1;
            unsigned kind = burst > 1 ? burst_kind() : 0;

            for (i = 0; i < burst && pushed_count < PUSHED_MAX; i++) {
                WINDOW_INPUT event = random_event(++id, kind);

                pushed[pushed_count].id = id;
                pushed[pushed_count].motion = model_is_motion(&event);
                pushed[pushed_count].sent = 0;
                pushed_count++;
                backlog_push(&backlog, &event);
                model_push(&model, &event);
            }
        } else if (action < 22) {
            WINDOW_CONFIGURE_EVENT configure = configure_of((UINT32)below(100));

            backlog_configure(&backlog, &configure);
            model.configure = configure;
            model.configured = 1;
        } else if (action < 26) {
            UINT64 time = below(1000000);

            backlog_frame(&backlog, time);
            model.frame_time = time;
            model.framed = 1;
        } else if (action < 30) {
            UINT32 buffers = (UINT32)below(16);

            backlog_release(&backlog, buffers);
            model.released |= buffers & WINDOW_BUFFERS_ALL;
        } else if (action < 34) {
            now += below(STALL * 3 / 2);
        } else if (action < 35) {
            now -= now > TIME_BACKWARD ? TIME_BACKWARD : 0;
        } else {
            UINT32 room = action == FINAL_FLUSH || below(4) == 0
                              ? UNLIMITED
                              : (UINT32)below(below(2) ? 4 : 80);
            UINT32 refusal = action != FINAL_FLUSH && below(6) == 0
                                 ? BACKLOG_SEND_CLOSED
                                 : BACKLOG_SEND_FULL;
            UINT32 got = flush_ending(&backlog, &channel, room, refusal, now);
            UINT32 want = model_flush(&model, room, refusal, now, expected,
                                      &expected_count);

            check(got == want, "a flush returns what the model's does", got,
                  want);
            check(channel.logged == expected_count,
                  "a flush sends as many as the model's", channel.logged,
                  expected_count);
            for (i = 0; i < channel.logged && i < expected_count; i++) {
                check(memcmp(&channel.log[i], &expected[i],
                             sizeof expected[i]) == 0,
                      "each message is the one the model sends", i, 0);
                note_sent(&channel.log[i], &last_sent, stream, &stream_count);
            }
        }
        check(backlog.count <= WINDOW_BACKLOG && backlog.count == model.length,
              "no more than the limit wait, as many as the model's",
              backlog.count, model.length);
        check(backlog_deadline(&backlog) == model_deadline(&model),
              "the deadline is the model's", 0, 0);
    }
    check(model.length == 0 && !model.dropped && !model.configured &&
              !model.framed && model.released == 0,
          "the last flush sent all", 0, 0);
    check(backlog_deadline(&backlog) == BACKLOG_NO_DEADLINE,
          "and left no deadline", 0, 0);
    for (i = 0; i < pushed_count; i++)
        for (step = 0; step < stream_count; step++)
            if (stream[step] == pushed[i].id)
                pushed[i].sent = 1;
    check_losses(pushed, pushed_count, stream, stream_count);
}

static void check_runs(void)
{
    unsigned n;

    for (n = 0; n < RUNS; n++)
        run_one();
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    check_tables();
    check_runs();
    printf("%llu checks, %u failed\n", checks, failures);
    return failures != 0;
}
