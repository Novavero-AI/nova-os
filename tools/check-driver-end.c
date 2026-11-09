/* Checks on the host what follows an input driver's end
   (lib/driver_end.c, docs/spec/input.md K4): the decision for every
   combination of its facts, and the supervision it takes part in, run
   with a driver's end read before its client's and after it, which the
   kernel's order of closing a process's handles and asserting its
   TERMINATED leaves open. A driver ended for its client's change is
   started again without counting a restart, with the client's change as
   the reason, whichever end is read first. Prints each failure and a
   count, and exits nonzero if any check fails.

   The supervision here repeats what init/platform.c does with the
   decision, which no host program can run: what it proves is that the
   decision gives the same outcome in both orders when used so. The
   platform driver's use of it is exercised by the boots, in whichever
   order of the two ends QEMU gives. The model has one driver whose
   endpoint a client took, and a driver's own end always starts it
   again, where init's restart policy may leave it down.

   cc -std=c99 -Wall -Wextra -pedantic -I include -I lib \
     -o check-driver-end tools/check-driver-end.c lib/driver_end.c &&
     ./check-driver-end */

#include <stdio.h>
#include <string.h>

#include "driver_end.h"

/* How many values each fact takes in the table of the decision: a flag
   is 0 or 1; a client is the compositor or the console; a start is one of
   three numbers, enough for one that is the same as another, earlier or
   later. */
#define CLIENT_VALUES 2
#define START_VALUES 3
#define FLAG_VALUES 2

/* The most changes of a client a sequence has: each is a start of the
   same client or a replacement, and every order of the two ends is run
   for each. */
#define SEQUENCE_MAX 3

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

/* ------------------------------------------------------------------------
   The decision, for every combination of its facts. */

/* The facts of a driver's end, from their values. */
static DRIVER_END facts_of(UINT32 closed, UINT32 served, UINT32 served_client,
                           UINT32 served_start, UINT32 client, UINT32 start,
                           UINT32 given_up)
{
    DRIVER_END facts;

    facts.endpoint_closed = closed;
    facts.served = served;
    facts.served_client = served_client;
    facts.served_start = served_start;
    facts.client = client;
    facts.start = start;
    facts.client_given_up = given_up;
    return facts;
}

/* What the facts call for, as input.md K4 says it in cases: a client
   given up leaves the driver down; an end that is not the closing of the
   endpoint, or of an endpoint no client took, is the driver's own; a
   client as it was cannot be told from the closing of the endpoint by a
   client that runs, which waits; any other client change is a restart of
   the same client, or a new client. */
static UINT32 expected(const DRIVER_END *facts)
{
    UINT32 same_client = facts->client == facts->served_client;
    UINT32 same_start = facts->start == facts->served_start;

    if (facts->client_given_up)
        return DRIVER_END_GIVEN_UP;
    if (!facts->endpoint_closed || !facts->served)
        return DRIVER_END_RESTART;
    if (same_client && same_start)
        return DRIVER_END_WAIT;
    if (same_client)
        return DRIVER_END_CLIENT_RESTARTED;
    return DRIVER_END_CLIENT_CHANGED;
}

/* The decision is the one expected() gives, for every combination of
   facts. */
static void check_decision(void)
{
    UINT32 closed, served, served_client, served_start, client, start, given;
    unsigned wrong = 0, total = 0;

    for (closed = 0; closed < FLAG_VALUES; closed++)
        for (served = 0; served < FLAG_VALUES; served++)
            for (served_client = 0; served_client < CLIENT_VALUES;
                 served_client++)
                for (served_start = 0; served_start < START_VALUES;
                     served_start++)
                    for (client = 0; client < CLIENT_VALUES; client++)
                        for (start = 0; start < START_VALUES; start++)
                            for (given = 0; given < FLAG_VALUES; given++) {
                                DRIVER_END facts =
                                    facts_of(closed, served, served_client,
                                             served_start, client, start,
                                             given);

                                total++;
                                if (driver_end_decide(&facts) !=
                                    expected(&facts))
                                    wrong++;
                            }
    check(total == FLAG_VALUES * FLAG_VALUES * CLIENT_VALUES * START_VALUES *
                       CLIENT_VALUES * START_VALUES * FLAG_VALUES,
          "the decision is checked for every combination of facts");
    check(wrong == 0, "the decision is the one K4 gives for every combination");
}

/* ------------------------------------------------------------------------
   The supervision. The clients are told apart by a number and their
   starts by one that no other start has. */

#define COMPOSITOR 0
#define CONSOLE 1

typedef struct {
    UINT32 client;     /* the client the input drivers have */
    UINT32 start;      /* its start */
    UINT32 given_up;   /* it is given up */
    UINT32 next_start; /* the last start number any client has had */
} WORLD;

#define DRIVER_RUNNING 0
#define DRIVER_WAITING 1
#define DRIVER_DOWN 2

/* An input driver as the platform driver records it, with what its
   supervision has done to it. */
typedef struct {
    UINT32 state;
    UINT32 served_client;
    UINT32 served_start;
    UINT32 counted;  /* restarts counted against it */
    UINT32 restarts; /* starts again for its client's change */
    UINT32 changed;  /* the last of those was for another client */
} DRIVER;

typedef enum {
    CLIENT_STARTED_AGAIN,
    CLIENT_REPLACED,
    CLIENT_GIVEN_UP
} CHANGE;

/* A driver started, its endpoint taken by the client the drivers have. */
static void start_driver(const WORLD *world, DRIVER *driver)
{
    driver->state = DRIVER_RUNNING;
    driver->served_client = world->client;
    driver->served_start = world->start;
}

/* The client is the compositor at its first start, and driver has
   started and has had its endpoint taken. */
static void world_start(WORLD *world, DRIVER *driver)
{
    world->client = COMPOSITOR;
    world->start = 1;
    world->given_up = 0;
    world->next_start = 1;
    memset(driver, 0, sizeof *driver);
    start_driver(world, driver);
}

/* The driver ends, with the code of its client's closing its endpoint or
   not, and the platform driver acts on the decision as platform.c does. */
static void driver_ends(const WORLD *world, DRIVER *driver, UINT32 closed)
{
    DRIVER_END facts =
        facts_of(closed, 1, driver->served_client, driver->served_start,
                 world->client, world->start, world->given_up);

    switch (driver_end_decide(&facts)) {
    case DRIVER_END_WAIT:
        driver->state = DRIVER_WAITING;
        break;
    case DRIVER_END_CLIENT_RESTARTED:
    case DRIVER_END_CLIENT_CHANGED:
        driver->restarts++;
        driver->changed = world->client != driver->served_client;
        start_driver(world, driver);
        break;
    case DRIVER_END_GIVEN_UP:
        driver->state = DRIVER_DOWN;
        break;
    case DRIVER_END_RESTART:
        driver->counted++;
        start_driver(world, driver);
        break;
    }
}

/* The client's end is read: it is started again, replaced by the other
   client or given up, and a driver that waits is decided again. */
static void client_ends(WORLD *world, DRIVER *driver, CHANGE change)
{
    if (change == CLIENT_GIVEN_UP) {
        world->given_up = 1;
    } else {
        if (change == CLIENT_REPLACED)
            world->client = CONSOLE;
        world->start = ++world->next_start;
    }
    if (driver->state == DRIVER_WAITING)
        driver_ends(world, driver, 1);
}

/* The same state, for the comparison of two runs. */
static int same(const DRIVER *a, const DRIVER *b)
{
    return memcmp(a, b, sizeof *a) == 0;
}

/* One change of the client, with the driver's end read before the
   client's or after it. */
static void change_client(WORLD *world, DRIVER *driver, CHANGE change,
                          int driver_first)
{
    if (driver_first)
        driver_ends(world, driver, 1);
    client_ends(world, driver, change);
    if (!driver_first)
        driver_ends(world, driver, 1);
}

/* Each kind of change of the client, with the driver's end read before
   the client's and after it. */
static void check_one_change(void)
{
    static const CHANGE changes[] = {CLIENT_STARTED_AGAIN, CLIENT_REPLACED,
                                     CLIENT_GIVEN_UP};
    unsigned i;

    for (i = 0; i < sizeof changes / sizeof changes[0]; i++) {
        WORLD world_first, world_second;
        DRIVER client_first, driver_first;

        world_start(&world_first, &client_first);
        change_client(&world_first, &client_first, changes[i], 0);
        world_start(&world_second, &driver_first);
        change_client(&world_second, &driver_first, changes[i], 1);
        check(same(&client_first, &driver_first),
              "a driver's end gives the same outcome read before its "
              "client's end as after it");
        check(client_first.counted == 0,
              "a driver's end for its client's change counts no restart");
        if (changes[i] == CLIENT_GIVEN_UP)
            check(client_first.state == DRIVER_DOWN &&
                      client_first.restarts == 0,
                  "a driver whose client is given up is left down");
        else
            check(client_first.state == DRIVER_RUNNING &&
                      client_first.restarts == 1 &&
                      client_first.served_client == world_first.client &&
                      client_first.served_start == world_first.start &&
                      client_first.changed ==
                          (UINT32)(changes[i] == CLIENT_REPLACED),
                  "a driver is started again for its client's change, as "
                  "its restarted client or its new one");
    }
}

/* Every sequence of up to SEQUENCE_MAX changes, then in each every
   choice of which end is read first for each change, is run and compared
   with the run that reads the client's end first throughout. */
static void check_sequences(void)
{
    static const CHANGE changes[] = {CLIENT_STARTED_AGAIN, CLIENT_REPLACED};
    unsigned count, choice, sequence, step;
    unsigned kinds = sizeof changes / sizeof changes[0];

    for (count = 1; count <= SEQUENCE_MAX; count++) {
        unsigned sequences = 1, orders = 1u << count;

        for (step = 0; step < count; step++)
            sequences *= kinds;
        for (sequence = 0; sequence < sequences; sequence++) {
            WORLD reference_world;
            DRIVER reference;

            world_start(&reference_world, &reference);
            for (step = 0, choice = sequence; step < count; step++) {
                change_client(&reference_world, &reference,
                              changes[choice % kinds], 0);
                choice /= kinds;
            }
            for (choice = 0; choice < orders; choice++) {
                WORLD world;
                DRIVER driver;
                unsigned rest = sequence;

                world_start(&world, &driver);
                for (step = 0; step < count; step++) {
                    change_client(&world, &driver, changes[rest % kinds],
                                  (int)(choice >> step & 1u));
                    rest /= kinds;
                }
                check(same(&reference, &driver),
                      "every order of the ends of a run of client changes "
                      "gives the outcome the client's end first gives");
                check(driver.counted == 0 && driver.state == DRIVER_RUNNING &&
                          driver.restarts == count,
                      "a run of client changes restarts the driver once "
                      "for each, counting none");
            }
        }
    }
}

/* A driver's own failure is counted, and a client's change after it is
   not. */
static void check_failure(void)
{
    WORLD world;
    DRIVER driver;

    world_start(&world, &driver);
    driver_ends(&world, &driver, 0);
    check(driver.counted == 1 && driver.restarts == 0 &&
              driver.state == DRIVER_RUNNING,
          "a driver that ends without its endpoint closed is started again, "
          "counting a restart");
    change_client(&world, &driver, CLIENT_STARTED_AGAIN, 1);
    check(driver.counted == 1 && driver.restarts == 1 &&
              driver.state == DRIVER_RUNNING,
          "its client's change after that is not counted");
}

/* A driver whose endpoint its client closed while it runs, or whose end
   comes before its client's, waits, and no restart is counted for it. */
static void check_wait(void)
{
    WORLD world;
    DRIVER driver;

    world_start(&world, &driver);
    driver_ends(&world, &driver, 1);
    check(driver.state == DRIVER_WAITING && driver.counted == 0 &&
              driver.restarts == 0,
          "a driver ended for a client as it was waits, counting no restart");
    driver_ends(&world, &driver, 1);
    check(driver.state == DRIVER_WAITING && driver.counted == 0 &&
              driver.restarts == 0,
          "it is decided again from the same facts, and waits on");
    client_ends(&world, &driver, CLIENT_STARTED_AGAIN);
    check(driver.state == DRIVER_RUNNING && driver.counted == 0 &&
              driver.restarts == 1,
          "it is started again when its client's end is read");
}

/* Two changes of the client before the driver's end is read give the one
   restart for the client it has now. */
static void check_two_changes(void)
{
    WORLD world;
    DRIVER driver;

    world_start(&world, &driver);
    client_ends(&world, &driver, CLIENT_STARTED_AGAIN);
    client_ends(&world, &driver, CLIENT_REPLACED);
    driver_ends(&world, &driver, 1);
    check(driver.state == DRIVER_RUNNING && driver.counted == 0 &&
              driver.restarts == 1 && driver.changed == 1 &&
              driver.served_client == world.client &&
              driver.served_start == world.start,
          "a driver ended after two changes of its client is started "
          "again once, for the client it has now");
}

/* Run every check, print the count, and exit nonzero if any failed. */
int main(void)
{
    check_decision();
    check_one_change();
    check_sequences();
    check_failure();
    check_wait();
    check_two_changes();
    printf("%u checks, %u failed\n", checks, failures);
    return failures != 0;
}
