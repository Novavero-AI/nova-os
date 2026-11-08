/* Checks the header tools/mkinterface.c makes of ci/probe/probe.desc
   (docs/spec/interface.md P2 to P4, P6 to P10 and P14), as a program
   includes it: constants that pass what a C int holds are computed in
   unsigned 64-bit arithmetic and the bounds built from them compare right,
   strings that hold question marks form no trigraph, the constraints of an
   array's structure hold for every element, a structure tail's accessor
   returns an aligned pointer, a request's reply on another channel has its
   functions, every handle of a run of a fixed number is checked and
   transferred, a reply with a status other than 0 is encoded with no handles
   and one with status 0 only with its handles, a tail that takes the rest of
   a message and the field that holds the bytes after the body hold what they
   should, text in an array and in a tail is UTF-8, counts of each width are
   read whole, constraints on signed fields compare as signed, and the
   dispatcher names no retired or first message. Each message is built byte
   by byte from the layout the description gives and from nothing the
   generator wrote. Prints each failure and a count, and exits nonzero if any
   check fails.

   ci/check-mkinterface.sh makes the header and runs this under the
   sanitizers:

   cc -std=c11 -Wall -Wextra -pedantic -Werror -Wtrigraphs \
     -fsanitize=address,undefined -fno-sanitize-recover=all \
     -I include -I DIRECTORY_OF_probe_protocol.h \
     -o check-probe-protocol tools/check-probe-protocol.c &&
     ./check-probe-protocol */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "probe_protocol.h"

static unsigned failures;

static void check(int passed, const char *what)
{
    if (passed)
        return;
    printf("failed: %s\n", what);
    failures++;
}

/* Whether a constant is an unsigned 64-bit integer: its size, and 0 less 1
   wrapping past 0. */
#define IS_UNSIGNED_64(x) (sizeof(x) == 8 && (x) - (x) - 1 > 0)

/* The text a macro expands to, as the header spells it (P4, rule 2). */
#define TEXT(x) TEXT_OF(x)
#define TEXT_OF(x) #x

_Static_assert(PROBE_SUM == 4000000000ULL && IS_UNSIGNED_64(PROBE_SUM),
               "a sum past a C int is computed in unsigned 64 bits");
_Static_assert(PROBE_PRODUCT == 4294967296ULL && IS_UNSIGNED_64(PROBE_PRODUCT),
               "a product past 32 bits is computed in unsigned 64 bits");
_Static_assert(PROBE_WRAP == 4294967296ULL && IS_UNSIGNED_64(PROBE_WRAP),
               "a hexadecimal sum is not computed in 32 bits");
_Static_assert(PROBE_NESTED == 2000000000ULL && IS_UNSIGNED_64(PROBE_NESTED),
               "a level inside another is computed in unsigned 64 bits");
_Static_assert(PROBE_NAMES == 8294967296ULL && IS_UNSIGNED_64(PROBE_NAMES),
               "a sum of constants is computed in unsigned 64 bits");
_Static_assert(PROBE_SMALL == 21, "a product within a C int is its value");

#if PROBE_SUM != 4000000000 || PROBE_NAMES != 8294967296
#error "a constant past a C int is not its value in #if"
#endif

/* The bytes of a message, aligned to 8 bytes as the generated functions
   require, and a second buffer to damage a copy of one in. */
static union {
    UINT64 alignment;
    UINT8 bytes[PROBE_LINK_TO_SERVER_BYTES_MAX];
} buffer, damaged;

static const RECEIVED_HANDLE none[1] = {{0, 0, 0}};

/* Store value in width bytes at at, least significant first: the layout of
   the structures on a little-endian machine, the only one nova-os runs on. */
static void put_le(UINT8 *at, UINT64 value, unsigned width)
{
    unsigned i;

    for (i = 0; i < width; i++)
        at[i] = (UINT8)(value >> (8 * i));
}

/* Whether the width bytes at at hold value, least significant first. */
static int holds_le(const UINT8 *at, UINT64 value, unsigned width)
{
    unsigned i;

    for (i = 0; i < width; i++)
        if (at[i] != (UINT8)(value >> (8 * i)))
            return 0;
    return 1;
}

typedef UINT32 (*CheckFn)(const void *bytes, UINT32 byte_count,
                          const RECEIVED_HANDLE *handles,
                          UINT32 handle_count);

/* Check size bytes on a copy of exactly that many, so that a check that
   reads a byte past them is found by the sanitizer. */
static UINT32 checked_bytes(CheckFn check_message, const UINT8 *bytes,
                            UINT32 size, const RECEIVED_HANDLE *handles,
                            UINT32 count)
{
    UINT8 *copy = (UINT8 *)malloc(size ? size : 1u);
    UINT32 status;

    if (copy == NULL)
        abort();
    memcpy(copy, bytes, size);
    status = check_message(copy, size, handles, count);
    free(copy);
    return status;
}

/* The first size bytes of buffer, checked. */
static UINT32 checked(CheckFn check_message, UINT32 size,
                      const RECEIVED_HANDLE *handles, UINT32 count)
{
    return checked_bytes(check_message, buffer.bytes, size, handles, count);
}

/* Fill buffer with bytes an encoder must overwrite or leave out. */
static void poison(void)
{
    memset(buffer.bytes, 0xA5, sizeof buffer.bytes);
}

/* The bytes of a message of size bytes that are 0 but a transaction and a
   selector's value, to which the caller adds the rest. */
static void want_begin(UINT8 *want, UINT32 size, UINT32 transaction,
                       UINT32 kind)
{
    memset(want, 0, size);
    put_le(want, transaction, 4);
    put_le(want + 4, kind, 4);
}


static void check_constants(void)
{
    check(strcmp(TEXT(PROBE_SMALL), "(3 * 7)") == 0,
          "a constant within a C int is written as the description has it");
    check(strcmp(TEXT(PROBE_SUM), "(1ULL * 2000000000 + 2000000000)") == 0,
          "a constant past a C int starts with the unsigned 64-bit factor");
}

static void check_strings(void)
{
    check(strcmp(PROBE_SLASH, "a\?\?/") == 0,
          "a string ending in the trigraph for a backslash is its text");
    check(strcmp(PROBE_BAR, "what\?\?!") == 0,
          "a string holding the trigraph for a bar is its text");
    check(strcmp(PROBE_ALL, "\?\?=\?\?/\?\?'\?\?(\?\?)\?\?!\?\?<\?\?>\?\?-") ==
              0,
          "a string of every trigraph is its text");
    check(strcmp(PROBE_TEXT_DEFAULT, "what\?\?!") == 0,
          "a text setting's default holding a trigraph is its text");
}

/* A store message whose every field holds. */
static PROBE_BODY valid_store(void)
{
    PROBE_BODY body;

    memset(&body, 0, sizeof body);
    body.transaction = 1;
    body.count = 2;
    body.items[0].value = 5;
    body.items[0].delta = 5;
    body.items[1].value = 0;
    body.items[1].delta = 1;
    body.fixed[0].value = 3;
    body.fixed[0].delta = 2;
    body.fixed[1].value = 5;
    body.fixed[1].delta = 5;
    body.big = PROBE_SUM;
    body.wrapped = PROBE_WRAP;
    return body;
}

/* The status of encoding body, and of checking the bytes it wrote, which
   are 0 and 0 where the encoding refused. */
static UINT32 store_encoded(const PROBE_BODY *body, UINT32 *size)
{
    poison();
    return probe_link_store_encode(body, buffer.bytes, size);
}

static UINT32 store_checked(UINT32 size)
{
    return checked(probe_link_store_check, size, none, 0);
}

/* Whether a store with one field changed is refused by both the encoding
   and the check of the bytes the valid message encodes to, with the same
   change made there. */
static void refused(PROBE_BODY body, UINT32 offset, UINT32 value,
                    const char *what)
{
    PROBE_BODY good = valid_store();
    UINT32 size = 0, written = 0, at;

    check(store_encoded(&body, &size) == ERR_INVALID_ARGS && size == 0, what);
    check(store_encoded(&good, &written) == OK, "a valid store encodes");
    memcpy(damaged.bytes, buffer.bytes, written);
    for (at = 0; at < 4; at++)
        damaged.bytes[offset + at] = (UINT8)(value >> (8 * at));
    check(checked_bytes(probe_link_store_check, damaged.bytes, written, none, 0) ==
              ERR_INVALID_ARGS,
          what);
}

static void check_elements(void)
{
    PROBE_BODY body = valid_store();
    UINT32 size = 0;
    const UINT32 items = offsetof(PROBE_BODY, items);
    const UINT32 fixed = offsetof(PROBE_BODY, fixed);

    check(store_encoded(&body, &size) == OK && size == sizeof body &&
              store_checked(size) == OK,
          "a store whose elements hold is accepted");

    body.items[0].value = 9;
    refused(body, items + offsetof(PROBE_ELEMENT, value), 9,
            "a counted element's value above its maximum is refused");
    body = valid_store();
    body.items[1].value = 6;
    refused(body, items + sizeof(PROBE_ELEMENT) +
                      offsetof(PROBE_ELEMENT, value),
            6, "the last counted element is checked");
    body = valid_store();
    body.items[1].delta = 0;
    refused(body, items + sizeof(PROBE_ELEMENT) +
                      offsetof(PROBE_ELEMENT, delta),
            0, "a signed element's value below its minimum is refused");
    body = valid_store();
    body.items[0].delta = -1;
    refused(body, items + offsetof(PROBE_ELEMENT, delta), (UINT32)-1,
            "a negative value is below a signed minimum");
    body = valid_store();
    body.items[0].delta = 6;
    refused(body, items + offsetof(PROBE_ELEMENT, delta), 6,
            "a signed element's value above its maximum is refused");
    body = valid_store();
    body.fixed[1].value = 9;
    refused(body, fixed + sizeof(PROBE_ELEMENT) +
                      offsetof(PROBE_ELEMENT, value),
            9, "an array with no count has every element checked");
    body = valid_store();
    body.fixed[0].delta = 0;
    refused(body, fixed + offsetof(PROBE_ELEMENT, delta), 0,
            "the first element of an array with no count is checked");

    body = valid_store();
    body.count = 1;
    check(store_encoded(&body, &size) == ERR_INVALID_ARGS,
          "an element past the count that is not 0 is refused");
    body.items[1].delta = 0;
    check(store_encoded(&body, &size) == OK && store_checked(size) == OK,
          "an element past the count is not held to its constraints");

    body = valid_store();
    body.big = PROBE_SUM + 1;
    refused(body, offsetof(PROBE_BODY, big), (UINT32)(PROBE_SUM + 1),
            "a bound past a C int is not wrapped: one above it is refused");
    body = valid_store();
    body.wrapped = PROBE_WRAP - 1;
    check(store_encoded(&body, &size) == ERR_INVALID_ARGS,
          "a minimum that wraps in 32 bits is not 0: one below it is "
          "refused");
}

/* Whether p is aligned for a pair. */
static int aligned(const void *p)
{
    return (uintptr_t)p % _Alignof(PROBE_PAIR) == 0;
}

/* A head and the pair after it, with no run. */
#define LEAD_ONLY_BYTES (sizeof(PROBE_HEAD) + sizeof(PROBE_PAIR))

static void check_tails(void)
{
    PROBE_HEAD head;
    PROBE_PAIR lead, run[4];
    const PROBE_PAIR *at_lead, *at_run;
    UINT32 size = 0;

    memset(&head, 0, sizeof head);
    head.transaction = 1;
    head.count = 2;
    lead.first = 1;
    lead.second = 2;
    run[0].first = 3;
    run[0].second = 4;
    run[1].first = 5;
    run[1].second = 6;
    run[2].first = 7;
    run[2].second = 8;
    run[3].first = 9;
    run[3].second = 10;
    poison();
    check(probe_link_pairs_encode(&head, &lead, run, 2, buffer.bytes,
                                  &size) == OK &&
              size == sizeof head + 3 * sizeof(PROBE_PAIR) &&
              checked(probe_link_pairs_check, size, none, 0) == OK,
          "pairs after a body that aligns them are accepted");
    at_lead = probe_link_pairs_lead(buffer.bytes);
    at_run = probe_link_pairs_run(buffer.bytes);
    check(aligned(at_lead) && aligned(at_run),
          "a structure tail's accessor returns an aligned pointer");
    check(at_lead->first == 1 && at_lead->second == 2 &&
              at_run[0].first == 3 && at_run[0].second == 4 &&
              at_run[1].first == 5 && at_run[1].second == 6,
          "a structure tail's accessor reads the pairs written");

    /* The pair after the body is the last tail where the run is empty. */
    head.count = 0;
    poison();
    check(probe_link_pairs_encode(&head, &lead, run, 0, buffer.bytes,
                                  &size) == OK &&
              size == LEAD_ONLY_BYTES &&
              checked(probe_link_pairs_check, size, none, 0) == OK,
          "a pair with an empty run after it is accepted");
    check(checked(probe_link_pairs_check, LEAD_ONLY_BYTES - 1, none, 0) ==
              ERR_INVALID_ARGS,
          "a pair cut short by a byte is refused");
    check(checked(probe_link_pairs_check, LEAD_ONLY_BYTES + 1, none, 0) ==
              ERR_INVALID_ARGS,
          "a byte after the last pair is refused");
    head.count = 3;
    poison();
    check(probe_link_pairs_encode(&head, &lead, run, 3, buffer.bytes,
                                  &size) == OK &&
              size == sizeof head + 4 * sizeof(PROBE_PAIR) &&
              checked(probe_link_pairs_check, size, none, 0) == OK,
          "a run of its maximum is accepted");
    check(probe_link_pairs_encode(&head, &lead, run, 4, buffer.bytes,
                                  &size) == ERR_INVALID_ARGS &&
              size == 0,
          "a run past its maximum is not encoded");
    put_le(buffer.bytes + offsetof(PROBE_HEAD, count), 4, 4);
    check(checked(probe_link_pairs_check, sizeof(PROBE_HEAD) +
                                              5 * sizeof(PROBE_PAIR),
                  none, 0) == ERR_INVALID_ARGS,
          "a run count past its maximum is refused");
}

static void check_open(void)
{
    PROBE_HEAD head;
    HANDLE_TRANSFER transfer;
    RECEIVED_HANDLE received;
    UINT32 handle = 9, size = 0;

    memset(&head, 0, sizeof head);
    memset(&transfer, 0, sizeof transfer);
    head.transaction = 1;
    poison();
    check(probe_link_open_encode(&head, &handle, 1, &transfer, buffer.bytes,
                                 &size) == OK &&
              size == sizeof head && transfer.handle == 9 &&
              transfer.rights == (CHANNEL_RIGHT_READ | CHANNEL_RIGHT_WRITE),
          "an open carrying a channel end encodes");
    received.handle = 9;
    received.type = TYPE_CHANNEL;
    received.rights = CHANNEL_RIGHT_READ | CHANNEL_RIGHT_WRITE;
    check(checked(probe_link_open_check, size, &received, 1) == OK &&
              checked(probe_link_open_check, size, none, 0) ==
                  ERR_INVALID_ARGS,
          "an open is accepted with its channel end and refused without");
    check(checked(probe_endpoint_opened_check, 0, none, 0) == OK &&
              checked(probe_endpoint_opened_check, 1, none, 0) ==
                  ERR_INVALID_ARGS,
          "the reply on the opened end is its first message, no bytes");
}

/* ------------------------------------------------------------------------
   Messages built by hand. */

/* Start a message in buffer: every byte 0 but a transaction of 1 and the
   selector's value, as every message here begins. */
static void begin(UINT32 kind)
{
    memset(buffer.bytes, 0, sizeof buffer.bytes);
    put_le(buffer.bytes, 1, 4);
    put_le(buffer.bytes + 4, kind, 4);
}

#define SENTINEL 0xDEADBEEFu
#define ANY_RIGHTS 0xFFFFFFFFu

/* The rights a handle of the runs travels with, which the description gives
   more than the check needs of a handle it receives. */
#define SECTION_TRAVELS (SECTION_RIGHT_READ | SECTION_RIGHT_WRITE)
#define PORT_TRAVELS (PORT_RIGHT_QUEUE | PORT_RIGHT_READ)

static const RECEIVED_HANDLE section = {1, TYPE_SECTION, SECTION_RIGHT_READ};
static const RECEIVED_HANDLE bare_section = {2, TYPE_SECTION, 0};
static const RECEIVED_HANDLE port = {3, TYPE_PORT, PORT_RIGHT_QUEUE};
static const RECEIVED_HANDLE bare_port = {4, TYPE_PORT, 0};
static const RECEIVED_HANDLE channel_end = {
    5, TYPE_CHANNEL, CHANNEL_RIGHT_READ | CHANNEL_RIGHT_WRITE};
static const RECEIVED_HANDLE read_only_end = {6, TYPE_CHANNEL,
                                              CHANNEL_RIGHT_READ};
/* Every right, of a type no message here takes in any position. */
static const RECEIVED_HANDLE process = {7, TYPE_PROCESS, ANY_RIGHTS};

/* The handles listed, as an array and its length. */
#define HANDLES(...) ((const RECEIVED_HANDLE[]){__VA_ARGS__})
#define COUNT(...)                                                           \
    ((UINT32)(sizeof HANDLES(__VA_ARGS__) / sizeof(RECEIVED_HANDLE)))

static void clear_transfers(HANDLE_TRANSFER *transfers, UINT32 count)
{
    UINT32 at;

    for (at = 0; at < count; at++) {
        transfers[at].handle = SENTINEL;
        transfers[at].rights = SENTINEL;
    }
}

/* The names a retired message would have. The header makes none, so these
   are the only definitions: a header that made them would not compile. */
static UINT32 probe_link_old_check(void)
{
    return PROBE_OLD;
}

static UINT32 probe_link_old_encode(void)
{
    return PROBE_OLD;
}

_Static_assert(PROBE_OLD == 10, "a retired message keeps its value");

/* ------------------------------------------------------------------------
   Runs of handles (P9, rule 1: a fixed run of any length is every slot). */

static UINT32 run_checked(const RECEIVED_HANDLE *handles, UINT32 count)
{
    begin(PROBE_RUN);
    return checked(probe_link_run_check, 8, handles, count);
}

static UINT32 trio_checked(const RECEIVED_HANDLE *handles, UINT32 count)
{
    begin(PROBE_TRIO);
    return checked(probe_link_trio_check, 8, handles, count);
}

#define RUN(...) run_checked(HANDLES(__VA_ARGS__), COUNT(__VA_ARGS__))
#define TRIO(...) trio_checked(HANDLES(__VA_ARGS__), COUNT(__VA_ARGS__))

static void check_runs(void)
{
    check(RUN(section, section) == OK, "a pair of sections is accepted");
    check(RUN(section, section, port, port, port) == OK,
          "a pair of sections and three ports is accepted");
    check(RUN(section) == ERR_INVALID_ARGS, "one section is fewer than the pair");
    check(RUN(section, section, port, port, port, port) == ERR_INVALID_ARGS,
          "six handles are more than the pair and three ports");
    check(RUN(process, section) == ERR_INVALID_ARGS,
          "the first of a pair of sections is checked for its type");
    check(RUN(section, process) == ERR_INVALID_ARGS,
          "the second of a pair of sections is checked for its type");
    check(RUN(section, port) == ERR_INVALID_ARGS,
          "a port is no second section");
    check(RUN(bare_section, section) == ERR_INVALID_ARGS,
          "the first of a pair is checked for its rights");
    check(RUN(section, bare_section) == ERR_INVALID_ARGS,
          "the second of a pair is checked for its rights");
    check(RUN(section, section, bare_port) == ERR_INVALID_ARGS,
          "the first port is checked for its rights");
    check(RUN(section, section, port, process) == ERR_INVALID_ARGS,
          "a port among ports is checked for its type");
    check(RUN(section, section, port, port, bare_port) == ERR_INVALID_ARGS,
          "the last port is checked for its rights");
    check(RUN(section, section, port, port, section) == ERR_INVALID_ARGS,
          "a section is no port in the last place");

    check(TRIO(channel_end, channel_end, channel_end) == OK,
          "three ends are accepted");
    check(TRIO(channel_end, channel_end) == ERR_INVALID_ARGS,
          "two ends are fewer than three");
    check(TRIO(channel_end, channel_end, channel_end, channel_end) ==
              ERR_INVALID_ARGS,
          "four ends are more than three");
    check(TRIO(read_only_end, channel_end, channel_end) == ERR_INVALID_ARGS,
          "the first of three ends is checked for its rights");
    check(TRIO(channel_end, read_only_end, channel_end) == ERR_INVALID_ARGS,
          "the second of three ends is checked for its rights");
    check(TRIO(channel_end, channel_end, read_only_end) == ERR_INVALID_ARGS,
          "the third of three ends is checked for its rights");
    check(TRIO(channel_end, process, channel_end) == ERR_INVALID_ARGS,
          "the second of three ends is checked for its type");
    check(TRIO(channel_end, channel_end, process) == ERR_INVALID_ARGS,
          "the third of three ends is checked for its type");
}

static void check_transfers(void)
{
    static const UINT32 handles[5] = {11, 12, 13, 14, 15};
    HANDLE_TRANSFER transfers[5];
    PROBE_HDR head = {7, 0};
    UINT8 want[8];
    UINT32 size = 99, at;

    clear_transfers(transfers, 5);
    poison();
    check(probe_link_run_encode(&head, handles, 5, transfers, buffer.bytes,
                                &size) == OK &&
              size == 8 && holds_le(buffer.bytes, 7, 4) &&
              holds_le(buffer.bytes + 4, PROBE_RUN, 4),
          "a pair of sections and three ports is encoded");
    want_begin(want, 8, 7, PROBE_RUN);
    check(memcmp(buffer.bytes, want, 8) == 0,
          "a run's message is its transaction and its selector");
    for (at = 0; at < 5; at++)
        check(transfers[at].handle == handles[at],
              "every handle of the runs is a transfer");
    check(transfers[0].rights == SECTION_TRAVELS &&
              transfers[1].rights == SECTION_TRAVELS,
          "each of a pair travels with its rights");
    check(transfers[2].rights == PORT_TRAVELS &&
              transfers[3].rights == PORT_TRAVELS &&
              transfers[4].rights == PORT_TRAVELS,
          "each of three ports travels with its rights");

    clear_transfers(transfers, 5);
    poison();
    check(probe_link_run_encode(&head, handles, 2, transfers, buffer.bytes,
                                &size) == OK &&
              size == 8 && transfers[0].handle == 11 &&
              transfers[1].handle == 12 && transfers[2].handle == SENTINEL,
          "a pair of sections alone transfers two handles");
    poison();
    check(probe_link_run_encode(&head, handles, 1, transfers, buffer.bytes,
                                &size) == ERR_INVALID_ARGS &&
              size == 0,
          "one handle is fewer than the pair");
    poison();
    check(probe_link_run_encode(&head, handles, 6, transfers, buffer.bytes,
                                &size) == ERR_INVALID_ARGS &&
              size == 0,
          "six handles are more than the pair and three ports");

    clear_transfers(transfers, 5);
    poison();
    check(probe_link_trio_encode(&head, handles, 3, transfers, buffer.bytes,
                                 &size) == OK &&
              size == 8 && transfers[0].handle == 11 &&
              transfers[1].handle == 12 && transfers[2].handle == 13 &&
              transfers[3].handle == SENTINEL,
          "three ends are three transfers");
    want_begin(want, 8, 7, PROBE_TRIO);
    check(memcmp(buffer.bytes, want, 8) == 0,
          "three ends' message is its transaction and its selector");
    for (at = 0; at < 3; at++)
        check(transfers[at].rights ==
                  (CHANNEL_RIGHT_READ | CHANNEL_RIGHT_WRITE),
              "each of three ends travels with its rights");
    poison();
    check(probe_link_trio_encode(&head, handles, 2, transfers, buffer.bytes,
                                 &size) == ERR_INVALID_ARGS &&
              size == 0,
          "two ends are fewer than three");
    poison();
    check(probe_link_trio_encode(&head, handles, 4, transfers, buffer.bytes,
                                 &size) == ERR_INVALID_ARGS &&
              size == 0,
          "four ends are more than three");
}

/* ------------------------------------------------------------------------
   A reply with a status (P7, rule 6; P10, rule 1, step 5; P14, rule 5). */

#define RESULT_BYTES 16u
#define RESULT_STATUS_OFFSET 8u
#define RESULT_LENGTH_OFFSET 12u

static UINT32 fetched_checked(UINT32 status, UINT32 length, UINT32 size,
                              const RECEIVED_HANDLE *handles, UINT32 count)
{
    begin(0);
    put_le(buffer.bytes + RESULT_STATUS_OFFSET, status, 4);
    put_le(buffer.bytes + RESULT_LENGTH_OFFSET, length, 4);
    return checked(probe_link_fetched_check, size, handles, count);
}

#define FETCHED(status, length, size, ...)                                   \
    fetched_checked(status, length, size, HANDLES(__VA_ARGS__),              \
                    COUNT(__VA_ARGS__))

static void check_replies(void)
{
    static const UINT32 handles[3] = {21, 22, 23};
    static const UINT8 data[3] = {'a', 'b', 'c'};
    HANDLE_TRANSFER transfers[3];
    PROBE_RESULT body = {5, 0, 0, 0};
    UINT8 want[RESULT_BYTES + 3];
    UINT32 size = 99;

    check(FETCHED(0, 3, RESULT_BYTES + 3, section) == OK,
          "a reply with status 0 takes its data and a section");
    check(FETCHED(0, 3, RESULT_BYTES + 3, section, section) == OK,
          "a reply with status 0 takes two sections");
    check(fetched_checked(0, 3, RESULT_BYTES + 3, none, 0) == ERR_INVALID_ARGS,
          "a reply with status 0 takes at least one section");
    check(FETCHED(0, 3, RESULT_BYTES + 3, section, section, section) ==
              ERR_INVALID_ARGS,
          "a reply with status 0 takes at most two sections");
    check(FETCHED(0, 3, RESULT_BYTES + 3, bare_section) == ERR_INVALID_ARGS,
          "a section of a reply is checked for its rights");
    check(FETCHED(0, 11, RESULT_BYTES + 11, section) == ERR_INVALID_ARGS,
          "a reply's data is at most its maximum");
    check(fetched_checked(3, 0, RESULT_BYTES, none, 0) == OK,
          "a reply with a status other than 0 is its body alone");
    check(FETCHED(3, 0, RESULT_BYTES, section) == ERR_INVALID_ARGS,
          "a reply with a status other than 0 takes no handle");
    check(fetched_checked(3, 0, RESULT_BYTES + 1, none, 0) == ERR_INVALID_ARGS,
          "a reply with a status other than 0 takes no byte after its body");

    /* The encoder agrees: an error reply is its body with no handle, a
       reply with status 0 has its handles and no others. */
    clear_transfers(transfers, 3);
    body.status = 3;
    poison();
    check(probe_link_fetched_encode(&body, data, 0, handles, 0, transfers,
                                    buffer.bytes, &size) == OK &&
              size == RESULT_BYTES && holds_le(buffer.bytes, 5, 4) &&
              holds_le(buffer.bytes + RESULT_STATUS_OFFSET, 3, 4) &&
              holds_le(buffer.bytes + RESULT_LENGTH_OFFSET, 0, 4) &&
              checked(probe_link_fetched_check, size, none, 0) == OK,
          "an error reply is encoded with no handle");
    want_begin(want, RESULT_BYTES, 5, 0);
    put_le(want + RESULT_STATUS_OFFSET, 3, 4);
    check(memcmp(buffer.bytes, want, RESULT_BYTES) == 0,
          "an error reply is its body: the status and no length");
    poison();
    check(probe_link_fetched_encode(&body, data, 0, handles, 1, transfers,
                                    buffer.bytes, &size) == ERR_INVALID_ARGS &&
              size == 0 && transfers[0].handle == SENTINEL,
          "an error reply is refused one handle");
    poison();
    check(probe_link_fetched_encode(&body, data, 3, handles, 0, transfers,
                                    buffer.bytes, &size) == ERR_INVALID_ARGS &&
              size == 0,
          "an error reply is refused data");

    body.status = 0;
    poison();
    check(probe_link_fetched_encode(&body, data, 3, handles, 0, transfers,
                                    buffer.bytes, &size) == ERR_INVALID_ARGS &&
              size == 0,
          "a reply with status 0 is refused no section");
    poison();
    check(probe_link_fetched_encode(&body, data, 3, handles, 3, transfers,
                                    buffer.bytes, &size) == ERR_INVALID_ARGS &&
              size == 0,
          "a reply with status 0 is refused three sections");
    clear_transfers(transfers, 3);
    poison();
    check(probe_link_fetched_encode(&body, data, 3, handles, 1, transfers,
                                    buffer.bytes, &size) == OK &&
              size == RESULT_BYTES + 3 &&
              holds_le(buffer.bytes + RESULT_LENGTH_OFFSET, 3, 4) &&
              memcmp(buffer.bytes + RESULT_BYTES, data, 3) == 0 &&
              transfers[0].handle == 21 &&
              transfers[0].rights == SECTION_TRAVELS &&
              transfers[1].handle == SENTINEL,
          "a reply with status 0 is its data and its section");
    want_begin(want, sizeof want, 5, 0);
    put_le(want + RESULT_LENGTH_OFFSET, 3, 4);
    memcpy(want + RESULT_BYTES, data, 3);
    check(memcmp(buffer.bytes, want, sizeof want) == 0,
          "a reply with status 0 is its body and its data");
    clear_transfers(transfers, 3);
    poison();
    check(probe_link_fetched_encode(&body, data, 3, handles, 2, transfers,
                                    buffer.bytes, &size) == OK &&
              transfers[0].handle == 21 && transfers[1].handle == 22 &&
              transfers[1].rights == SECTION_TRAVELS &&
              transfers[2].handle == SENTINEL,
          "a reply with status 0 transfers two sections");
}

static UINT32 claimed_checked(UINT32 status, UINT32 size,
                              const RECEIVED_HANDLE *handles, UINT32 count)
{
    begin(0);
    put_le(buffer.bytes + RESULT_STATUS_OFFSET, status, 4);
    return checked(probe_link_claimed_check, size, handles, count);
}

#define CLAIMED(status, ...)                                                 \
    claimed_checked(status, RESULT_BYTES, HANDLES(__VA_ARGS__),              \
                    COUNT(__VA_ARGS__))

/* A reply with one end, a fixed run of one handle, that has status 0 only. */
static void check_claimed(void)
{
    static const UINT32 handles[2] = {31, 32};
    HANDLE_TRANSFER transfers[2];
    PROBE_RESULT body = {5, 0, 3, 0};
    UINT8 want[RESULT_BYTES];
    UINT32 size = 99;

    check(CLAIMED(0, channel_end) == OK,
          "a reply with status 0 takes its one end");
    check(claimed_checked(0, RESULT_BYTES, none, 0) == ERR_INVALID_ARGS,
          "a reply with status 0 takes an end");
    check(CLAIMED(0, channel_end, channel_end) == ERR_INVALID_ARGS,
          "a reply with status 0 takes only one end");
    check(CLAIMED(0, read_only_end) == ERR_INVALID_ARGS,
          "the end of a reply is checked for its rights");
    check(claimed_checked(3, RESULT_BYTES, none, 0) == OK,
          "a reply with a status other than 0 is its body alone");
    check(CLAIMED(3, channel_end) == ERR_INVALID_ARGS,
          "a reply with a status other than 0 takes no end");

    /* An error reply has no handle, so the encoder is given none and must
       not read or write through the pointers it was given. */
    poison();
    check(probe_link_claimed_encode(&body, NULL, 0, NULL, buffer.bytes,
                                    &size) == OK &&
              size == RESULT_BYTES &&
              holds_le(buffer.bytes + RESULT_STATUS_OFFSET, 3, 4) &&
              checked(probe_link_claimed_check, size, none, 0) == OK,
          "an error reply is encoded with no end and no pointers");
    want_begin(want, RESULT_BYTES, 5, 0);
    put_le(want + RESULT_STATUS_OFFSET, 3, 4);
    check(memcmp(buffer.bytes, want, RESULT_BYTES) == 0,
          "an error reply with no end is its body");
    poison();
    check(probe_link_claimed_encode(&body, handles, 1, NULL, buffer.bytes,
                                    &size) == ERR_INVALID_ARGS &&
              size == 0,
          "an error reply is refused an end");
    body.status = 0;
    clear_transfers(transfers, 2);
    poison();
    check(probe_link_claimed_encode(&body, handles, 1, transfers, buffer.bytes,
                                    &size) == OK &&
              size == RESULT_BYTES && transfers[0].handle == 31 &&
              transfers[0].rights ==
                  (CHANNEL_RIGHT_READ | CHANNEL_RIGHT_WRITE) &&
              transfers[1].handle == SENTINEL,
          "a reply with status 0 transfers its one end");
    poison();
    check(probe_link_claimed_encode(&body, handles, 0, transfers, buffer.bytes,
                                    &size) == ERR_INVALID_ARGS &&
              size == 0,
          "a reply with status 0 is refused no end");
    poison();
    check(probe_link_claimed_encode(&body, handles, 2, transfers, buffer.bytes,
                                    &size) == ERR_INVALID_ARGS &&
              size == 0,
          "a reply with status 0 is refused two ends");
}

/* ------------------------------------------------------------------------
   A tail that takes the rest, and the length of the tails (P8, rules 3 and
   4 and 6). */

#define NOTE_BYTES 12u
#define NOTE_LENGTH_OFFSET 8u
#define NOTE_TEXT_MAX 20u

static UINT32 note_checked(UINT32 length, const UINT8 *text, UINT32 count)
{
    begin(PROBE_NOTE_REQUEST);
    put_le(buffer.bytes + NOTE_LENGTH_OFFSET, length, 4);
    memcpy(buffer.bytes + NOTE_BYTES, text, count);
    return checked(probe_link_note_check, NOTE_BYTES + count, none, 0);
}

static void check_note(void)
{
    /* "h", e with an acute accent, "llo": six bytes. */
    static const UINT8 accented[6] = {'h', 0xC3, 0xA9, 'l', 'l', 'o'};
    static const UINT8 overlong[2] = {0xC0, 0x80};
    static const UINT8 with_nul[3] = {'a', 0, 'b'};
    /* RFC 3629's characters at the edge of each form, and what it refuses:
       a continuation byte or a byte the form has no use for as the first,
       an overlong form, a surrogate, a character past U+10FFFF, and a
       character cut short or with a byte that is no continuation. */
    static const struct {
        UINT8 bytes[4];
        UINT32 count;
        int text;
        const char *what;
    } utf8[] = {
        {{0x7F}, 1, 1, "U+007F is text"},
        {{0xC2, 0x80}, 2, 1, "U+0080 is text"},
        {{0xDF, 0xBF}, 2, 1, "U+07FF is text"},
        {{0xE0, 0xA0, 0x80}, 3, 1, "U+0800 is text"},
        {{0xED, 0x9F, 0xBF}, 3, 1, "U+D7FF is text"},
        {{0xEE, 0x80, 0x80}, 3, 1, "U+E000 is text"},
        {{0xEF, 0xBF, 0xBF}, 3, 1, "U+FFFF is text"},
        {{0xF0, 0x90, 0x80, 0x80}, 4, 1, "U+10000 is text"},
        {{0xF0, 0x9F, 0x98, 0x80}, 4, 1, "U+1F600 is text"},
        {{0xF4, 0x8F, 0xBF, 0xBF}, 4, 1, "U+10FFFF is text"},
        {{0x80}, 1, 0, "a continuation byte alone is no text"},
        {{0xBF}, 1, 0, "the last continuation byte alone is no text"},
        {{0xC0, 0x80}, 2, 0, "C0 is an overlong lead"},
        {{0xC1, 0xBF}, 2, 0, "C1 is an overlong lead"},
        {{0xE0, 0x80, 0x80}, 3, 0, "a three byte overlong form is no text"},
        {{0xE0, 0x9F, 0xBF}, 3, 0, "the last three byte overlong form"},
        {{0xED, 0xA0, 0x80}, 3, 0, "U+D800 is no text"},
        {{0xED, 0xBF, 0xBF}, 3, 0, "U+DFFF is no text"},
        {{0xF0, 0x80, 0x80, 0x80}, 4, 0, "a four byte overlong form"},
        {{0xF0, 0x8F, 0xBF, 0xBF}, 4, 0, "the last four byte overlong form"},
        {{0xF4, 0x90, 0x80, 0x80}, 4, 0, "U+110000 is no text"},
        {{0xF5, 0x80, 0x80, 0x80}, 4, 0, "F5 leads no character"},
        {{0xF8, 0x80, 0x80, 0x80}, 4, 0, "F8 leads no character"},
        {{0xFF}, 1, 0, "FF is no text"},
        {{0xC3}, 1, 0, "a two byte character cut short"},
        {{0xE2, 0x82}, 2, 0, "a three byte character cut short"},
        {{0xF0, 0x9F, 0x98}, 3, 0, "a four byte character cut short"},
        {{0xC3, 0x41}, 2, 0, "a second byte that is no continuation"},
        {{0xE2, 0x82, 0x41}, 3, 0, "a third byte that is no continuation"},
        {{0xF0, 0x9F, 0x98, 0x41}, 4, 0, "a fourth byte that is no continuation"},
        {{0xE2, 0x82, 0xC1}, 3, 0, "a third byte with its high bit set is no continuation"},
        {{0xF0, 0x9F, 0xC1, 0x80}, 4, 0, "a third byte of four that is no continuation"},
        {{0xF0, 0x9F, 0x98, 0xC1}, 4, 0, "a fourth byte with its high bit set"},
    };
    UINT8 many[NOTE_TEXT_MAX + 1];
    PROBE_NOTE note = {3, 0, 0};
    UINT8 want[NOTE_BYTES + 6];
    UINT32 size = 99;
    unsigned v;

    memset(many, 'a', sizeof many);
    check(note_checked(6, accented, 6) == OK,
          "a text that takes the rest is accepted with its length");
    check(note_checked(0, accented, 0) == OK,
          "the rest may be empty");
    check(note_checked(5, accented, 6) == ERR_INVALID_ARGS,
          "a length one under the bytes after the body is refused");
    check(note_checked(7, accented, 6) == ERR_INVALID_ARGS,
          "a length one over the bytes after the body is refused");
    check(note_checked(NOTE_TEXT_MAX, many, NOTE_TEXT_MAX) == OK,
          "a rest of its maximum is accepted");
    check(note_checked(NOTE_TEXT_MAX + 1, many, NOTE_TEXT_MAX + 1) ==
              ERR_INVALID_ARGS,
          "a rest past its maximum is refused");
    for (v = 0; v < sizeof utf8 / sizeof utf8[0]; v++)
        check(note_checked(utf8[v].count, utf8[v].bytes, utf8[v].count) ==
                  (utf8[v].text ? OK : ERR_INVALID_ARGS),
              utf8[v].what);
    check(note_checked(3, with_nul, 3) == ERR_INVALID_ARGS,
          "text holds no NUL");

    poison();

    check(probe_link_note_encode(&note, accented, 6, buffer.bytes, &size) ==
                  OK &&
              size == NOTE_BYTES + 6 && holds_le(buffer.bytes, 3, 4) &&
              holds_le(buffer.bytes + 4, PROBE_NOTE_REQUEST, 4) &&
              holds_le(buffer.bytes + NOTE_LENGTH_OFFSET, 6, 4) &&
              memcmp(buffer.bytes + NOTE_BYTES, accented, 6) == 0 &&
              checked(probe_link_note_check, size, none, 0) == OK,
          "the length of a text is written as the bytes after the body");
    want_begin(want, sizeof want, 3, PROBE_NOTE_REQUEST);
    put_le(want + NOTE_LENGTH_OFFSET, 6, 4);
    memcpy(want + NOTE_BYTES, accented, 6);
    check(memcmp(buffer.bytes, want, sizeof want) == 0,
          "a note is its body and its text");
    check(probe_link_note_encode(&note, many, NOTE_TEXT_MAX, buffer.bytes,
                                 &size) == OK &&
              size == NOTE_BYTES + NOTE_TEXT_MAX &&
              holds_le(buffer.bytes + NOTE_LENGTH_OFFSET, NOTE_TEXT_MAX, 4),
          "a text of its maximum is encoded");
    poison();
    check(probe_link_note_encode(&note, many, NOTE_TEXT_MAX + 1, buffer.bytes,
                                 &size) == ERR_INVALID_ARGS &&
              size == 0,
          "a text past its maximum is not encoded");
    poison();
    check(probe_link_note_encode(&note, overlong, 2, buffer.bytes, &size) ==
                  ERR_INVALID_ARGS &&
              size == 0,
          "text that is not text is not encoded");
}

/* ------------------------------------------------------------------------
   Counts of each width, and text in an array (P6, rule 4; P8, rule 2). */

#define WIDE_BYTES 32u
#define WIDE_SIZE_OFFSET 8u
#define WIDE_EXTRA_OFFSET 16u
#define WIDE_LABEL_LENGTH_OFFSET 18u
#define WIDE_LABEL_OFFSET 20u
#define WIDE_LABEL_MAX 8u
#define WIDE_PAD_OFFSET 28u
#define WIDE_DATA_MAX 6u
#define WIDE_MORE_MAX 4u

/* A wide message of a label, with size, extra and label_length as given. */
static void wide_begin(UINT64 size, UINT32 extra, UINT32 label_length,
                       const UINT8 *label, UINT32 label_bytes)
{
    begin(PROBE_WIDE_REQUEST);
    put_le(buffer.bytes + WIDE_SIZE_OFFSET, size, 8);
    put_le(buffer.bytes + WIDE_EXTRA_OFFSET, extra, 2);
    put_le(buffer.bytes + WIDE_LABEL_LENGTH_OFFSET, label_length, 2);
    memcpy(buffer.bytes + WIDE_LABEL_OFFSET, label, label_bytes);
}

static UINT32 wide_checked(UINT32 total)
{
    return checked(probe_link_wide_check, total, none, 0);
}

static void check_wide(void)
{
    static const UINT8 abc[3] = {'a', 'b', 'c'};
    static const UINT8 accented[3] = {'h', 0xC3, 0xA9};
    static const UINT8 overlong[2] = {0xC0, 0x80};
    static const UINT8 truncated[1] = {0xC3};
    static const UINT8 with_nul[3] = {'a', 0, 'b'};
    static const UINT8 full[WIDE_LABEL_MAX] = {'a', 'b', 'c', 'd',
                                               'e', 'f', 'g', 'h'};
    static const UINT8 two[2] = {'a', 'b'};
    static const UINT8 data[2] = {1, 2}, more[1] = {3};
    static const UINT8 tails[WIDE_DATA_MAX] = {1, 2, 3, 4, 5, 6};
    UINT8 want[WIDE_BYTES + 3];
    PROBE_WIDE body;
    UINT32 size = 99;

    wide_begin(2, 1, 3, abc, 3);
    check(wide_checked(WIDE_BYTES + 3) == OK,
          "tails counted by a u64 and a u16 are accepted");
    wide_begin(WIDE_DATA_MAX, WIDE_MORE_MAX, 3, abc, 3);
    check(wide_checked(WIDE_BYTES + WIDE_DATA_MAX + WIDE_MORE_MAX) == OK,
          "tails of their maximums are accepted");
    wide_begin(WIDE_DATA_MAX + 1, 0, 3, abc, 3);
    check(wide_checked(WIDE_BYTES + WIDE_DATA_MAX + 1) == ERR_INVALID_ARGS,
          "a u64 count past its maximum is refused");
    wide_begin(0, WIDE_MORE_MAX + 1, 3, abc, 3);
    check(wide_checked(WIDE_BYTES + WIDE_MORE_MAX + 1) == ERR_INVALID_ARGS,
          "a u16 count past its maximum is refused");
    /* 2^32 + 2 is 2 in its low 32 bits: all of a u64 is the count. */
    wide_begin(((UINT64)1 << 32) + 2, 0, 3, abc, 3);
    check(wide_checked(WIDE_BYTES + 2) == ERR_INVALID_ARGS,
          "a u64 count is read in all its bytes");
    wide_begin(2, 0, 3, abc, 3);
    check(wide_checked(WIDE_BYTES + 1) == ERR_INVALID_ARGS,
          "a message a byte short of its counted tails is refused");
    check(wide_checked(WIDE_BYTES + 3) == ERR_INVALID_ARGS,
          "a message a byte over its counted tails is refused");
    wide_begin(1, 0, 3, abc, 3);
    put_le(buffer.bytes + WIDE_PAD_OFFSET, 1, 4);
    check(wide_checked(WIDE_BYTES + 1) == ERR_INVALID_ARGS,
          "padding that is not 0 is refused");

    wide_begin(0, 0, 3, accented, 3);
    check(wide_checked(WIDE_BYTES) == OK, "a text array holds text");
    wide_begin(0, 0, WIDE_LABEL_MAX, full, WIDE_LABEL_MAX);
    check(wide_checked(WIDE_BYTES) == OK, "a text array may be full");
    wide_begin(0, 0, 0, abc, 0);
    check(wide_checked(WIDE_BYTES) == OK, "a text array may be empty");
    wide_begin(0, 0, WIDE_LABEL_MAX + 1, full, WIDE_LABEL_MAX);
    check(wide_checked(WIDE_BYTES) == ERR_INVALID_ARGS,
          "a text array's count past its size is refused");
    wide_begin(0, 0, 2, overlong, 2);
    check(wide_checked(WIDE_BYTES) == ERR_INVALID_ARGS,
          "an overlong form in a text array is refused");
    wide_begin(0, 0, 1, truncated, 1);
    check(wide_checked(WIDE_BYTES) == ERR_INVALID_ARGS,
          "a character cut short in a text array is refused");
    wide_begin(0, 0, 3, with_nul, 3);
    check(wide_checked(WIDE_BYTES) == ERR_INVALID_ARGS,
          "a NUL in a text array is refused");
    wide_begin(0, 0, 2, two, 2);
    buffer.bytes[WIDE_LABEL_OFFSET + 5] = 'x';
    check(wide_checked(WIDE_BYTES) == ERR_INVALID_ARGS,
          "a byte past a text array's count that is not 0 is refused");

    memset(&body, 0, sizeof body);
    body.transaction = 4;
    body.size = 99;
    body.extra = 99;
    body.label_length = 3;
    memcpy(body.label, abc, 3);
    poison();
    check(probe_link_wide_encode(&body, data, 2, more, 1, buffer.bytes,
                                 &size) == OK &&
              size == WIDE_BYTES + 3 &&
              holds_le(buffer.bytes + WIDE_SIZE_OFFSET, 2, 8) &&
              holds_le(buffer.bytes + WIDE_EXTRA_OFFSET, 1, 2) &&
              holds_le(buffer.bytes + WIDE_LABEL_LENGTH_OFFSET, 3, 2) &&
              memcmp(buffer.bytes + WIDE_BYTES, data, 2) == 0 &&
              buffer.bytes[WIDE_BYTES + 2] == 3 &&
              checked(probe_link_wide_check, size, none, 0) == OK,
          "counts of each width are written from the tails' counts");
    want_begin(want, sizeof want, 4, PROBE_WIDE_REQUEST);
    put_le(want + WIDE_SIZE_OFFSET, 2, 8);
    put_le(want + WIDE_EXTRA_OFFSET, 1, 2);
    put_le(want + WIDE_LABEL_LENGTH_OFFSET, 3, 2);
    memcpy(want + WIDE_LABEL_OFFSET, abc, 3);
    memcpy(want + WIDE_BYTES, data, 2);
    want[WIDE_BYTES + 2] = 3;
    check(memcmp(buffer.bytes, want, sizeof want) == 0,
          "a wide message is its body and its tails");
    check(probe_link_wide_encode(&body, tails, WIDE_DATA_MAX, tails,
                                 WIDE_MORE_MAX, buffer.bytes, &size) == OK &&
              size == WIDE_BYTES + WIDE_DATA_MAX + WIDE_MORE_MAX,
          "tails of their maximums are encoded");
    poison();
    check(probe_link_wide_encode(&body, data, WIDE_DATA_MAX + 1, more, 1,
                                 buffer.bytes, &size) == ERR_INVALID_ARGS &&
              size == 0,
          "a u64 counted tail past its maximum is not encoded");
    poison();
    check(probe_link_wide_encode(&body, data, 2, more, WIDE_MORE_MAX + 1,
                                 buffer.bytes, &size) == ERR_INVALID_ARGS &&
              size == 0,
          "a u16 counted tail past its maximum is not encoded");
}

/* ------------------------------------------------------------------------
   Signed and unsigned constraints (P6, rule 3, and P21, rule 2): a row of an
   i32 under 5, a u32 over 0, an i64 under 5 and an i64 over 0, in the body,
   in each element of an array and in each element of a counted tail. */

#define ROW_BYTES 24u
#define ROW_HIGH_OFFSET 4u
#define ROW_WIDE_OFFSET 8u
#define ROW_ABOVE_OFFSET 16u
#define SIGNED_COUNT_OFFSET 8u
#define SIGNED_ROW_OFFSET 16u
#define SIGNED_ROWS_OFFSET 40u
#define SIGNED_BYTES 88u
#define SIGNED_LIST_MAX 2u

#define MINUS_ONE32 0xFFFFFFFFu
#define MINUS_ONE64 (~(UINT64)0)
#define MOST_NEGATIVE32 0x80000000u
#define MOST_NEGATIVE64 ((UINT64)1 << 63)
#define MOST_POSITIVE64 (MOST_NEGATIVE64 - 1)

typedef struct {
    UINT32 low;
    UINT32 high;
    UINT64 wide;
    UINT64 above;
} Row;

/* Write a row at at. */
static void put_row(UINT8 *at, const Row *row)
{
    put_le(at, row->low, 4);
    put_le(at + ROW_HIGH_OFFSET, row->high, 4);
    put_le(at + ROW_WIDE_OFFSET, row->wide, 8);
    put_le(at + ROW_ABOVE_OFFSET, row->above, 8);
}

/* Whether a row holds as the description says: low and wide at most 5 and
   above at least 1, compared as signed, and high at least 1 as unsigned. */
static int row_holds(const Row *row)
{
    return (int32_t)row->low <= 5 && row->high >= 1 &&
           (int64_t)row->wide <= 5 && (int64_t)row->above >= 1;
}

/* A message of the body's row, two array rows and a list of list_count
   rows, each as given, and whether its check accepts it. */
static UINT32 signed_checked(const Row *body_row, const Row *first,
                             const Row *second, const Row *list,
                             UINT32 list_count)
{
    UINT32 at;

    begin(PROBE_SIGNED_REQUEST);
    put_le(buffer.bytes + SIGNED_COUNT_OFFSET, list_count, 4);
    put_row(buffer.bytes + SIGNED_ROW_OFFSET, body_row);
    put_row(buffer.bytes + SIGNED_ROWS_OFFSET, first);
    put_row(buffer.bytes + SIGNED_ROWS_OFFSET + ROW_BYTES, second);
    for (at = 0; at < list_count; at++)
        put_row(buffer.bytes + SIGNED_BYTES + at * ROW_BYTES, list);
    return checked(probe_link_signed_check, SIGNED_BYTES + list_count * ROW_BYTES, none,
                                   0);
}

static void check_signed(void)
{
    static const Row valid = {0, 1, 0, 1};
    static const Row rows[] = {
        {0, 1, 0, 1},
        {5, 1, 5, 1},
        {6, 1, 0, 1},
        {MINUS_ONE32, 1, 0, 1},
        {MOST_NEGATIVE32, 1, 0, 1},
        {0, 0, 0, 1},
        {0, MINUS_ONE32, 0, 1},
        {0, 1, 6, 1},
        {0, 1, MINUS_ONE64, 1},
        {0, 1, MOST_NEGATIVE64, 1},
        {0, 1, 0, 0},
        {0, 1, 0, MINUS_ONE64},
        {0, 1, 0, MOST_NEGATIVE64},
        {0, 1, 0, MOST_POSITIVE64},
    };
    unsigned i;

    check(signed_checked(&valid, &valid, &valid, &valid, 0) == OK,
          "a message of rows that hold is accepted");
    for (i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        const UINT32 expected = row_holds(&rows[i]) ? OK : ERR_INVALID_ARGS;

        check(signed_checked(&rows[i], &valid, &valid, &valid, 0) == expected,
              "a row in the body holds as the description says");
        check(signed_checked(&valid, &rows[i], &valid, &valid, 0) == expected,
              "the first row of an array holds as the description says");
        check(signed_checked(&valid, &valid, &rows[i], &valid, 0) == expected,
              "the last row of an array holds as the description says");
        check(signed_checked(&valid, &valid, &valid, &rows[i], 1) == expected,
              "a row of a counted tail holds as the description says");
        check(signed_checked(&valid, &valid, &valid, &rows[i],
                             SIGNED_LIST_MAX) == expected,
              "every row of a counted tail holds as the description says");
    }
}

/* ------------------------------------------------------------------------
   Transactions (P7, rule 4): on a channel with replies and events, a request
   with a reply carries one other than 0, and an event carries 0. */

static void check_transactions(void)
{
    begin(PROBE_FETCH);
    put_le(buffer.bytes, 0, 4);
    check(checked(probe_link_fetch_check, 8, none, 0) == ERR_INVALID_ARGS,
          "a request with a reply carries a transaction other than 0");
    begin(PROBE_FETCH);
    check(checked(probe_link_fetch_check, 8, none, 0) == OK,
          "a request with a reply is accepted with a transaction");
    begin(PROBE_HELLO);
    put_le(buffer.bytes, 0, 4);
    check(checked(probe_link_hello_check, 8, none, 0) == OK,
          "a request with no reply may carry 0");
    begin(PROBE_TICK);
    put_le(buffer.bytes, 0, 4);
    check(checked(probe_link_tick_check, 8, none, 0) == OK,
          "an event carries 0");
    begin(PROBE_TICK);
    check(checked(probe_link_tick_check, 8, none, 0) == ERR_INVALID_ARGS,
          "an event carries no transaction other than 0");
}

/* ------------------------------------------------------------------------
   The request dispatcher (P10, rule 2). */

#define SELECTOR_UNSET 0xFFFFFFFFu

static UINT32 dispatched(UINT32 kind, UINT32 size,
                         const RECEIVED_HANDLE *handles, UINT32 count,
                         UINT32 *which)
{
    UINT8 *copy;
    UINT32 status;

    begin(kind);
    copy = (UINT8 *)malloc(size);
    if (copy == NULL)
        abort();
    memcpy(copy, buffer.bytes, size);
    *which = SELECTOR_UNSET;
    status = probe_link_request_check(copy, size, handles, count, which);
    free(copy);
    return status;
}

/* The event dispatcher's answer to a message of kind and size bytes. */
static UINT32 event_dispatched(UINT32 kind, UINT32 size, UINT32 *which)
{
    UINT8 *copy;
    UINT32 status;

    begin(kind);
    put_le(buffer.bytes, 0, 4);
    copy = (UINT8 *)malloc(size);
    if (copy == NULL)
        abort();
    memcpy(copy, buffer.bytes, size);
    *which = SELECTOR_UNSET;
    status = probe_link_event_check(copy, size, none, 0, which);
    free(copy);
    return status;
}

static void check_dispatch(void)
{
    UINT32 which;

    check(event_dispatched(PROBE_TICK, 8, &which) == OK &&
              which == PROBE_TICK,
          "an event is given to its check by its selector");
    check(event_dispatched(PROBE_TRIO, 8, &which) == ERR_NOT_SUPPORTED &&
              which == PROBE_TRIO,
          "a request is no event");
    check(event_dispatched(99, 8, &which) == ERR_NOT_SUPPORTED &&
              which == 99,
          "an event selector no event has is not supported");

    check(dispatched(PROBE_TRIO, 8, HANDLES(channel_end, channel_end,
                                            channel_end), 3, &which) == OK &&
              which == PROBE_TRIO,
          "a message is given to its check by its selector");
    check(dispatched(PROBE_RUN, 8, HANDLES(channel_end, channel_end,
                                           channel_end), 3, &which) ==
                  ERR_INVALID_ARGS &&
              which == PROBE_RUN,
          "a message is checked as its selector's message");
    check(dispatched(PROBE_TRIO, 9, HANDLES(channel_end, channel_end,
                                            channel_end), 3, &which) ==
                  ERR_INVALID_ARGS &&
              which == PROBE_TRIO,
          "a message that breaks its check is refused with its selector");
    check(dispatched(PROBE_TRIO, 7, none, 0, &which) == ERR_INVALID_ARGS &&
              which == PROBE_LINK_UNNAMED,
          "bytes too short to hold the selector are unnamed");
    check(dispatched(99, 8, none, 0, &which) == ERR_NOT_SUPPORTED &&
              which == 99,
          "a selector no message has is not supported");
    check(dispatched(PROBE_OLD, 8, none, 0, &which) == ERR_NOT_SUPPORTED &&
              which == PROBE_OLD,
          "a retired message is not supported");
    check(dispatched(PROBE_HELLO, 8, none, 0, &which) == ERR_NOT_SUPPORTED &&
              which == PROBE_HELLO,
          "a first message is no dispatcher's");
    begin(PROBE_HELLO);
    check(checked(probe_link_hello_check, 8, none, 0) == OK,
          "a first message is checked by its own check");
    check(probe_link_old_check() + probe_link_old_encode() == 2 * PROBE_OLD,
          "a retired message has no functions");
}

int main(void)
{
    check_constants();
    check_strings();
    check_elements();
    check_tails();
    check_open();
    check_runs();
    check_transfers();
    check_replies();
    check_claimed();
    check_note();
    check_wide();
    check_signed();
    check_transactions();
    check_dispatch();
    printf("%u failure%s\n", failures, failures == 1 ? "" : "s");
    return failures != 0;
}
