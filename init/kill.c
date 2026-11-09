/* init's check of task_kill (docs/spec/objects.md O7, O10; syscalls.md
   S8) against running code: a refused call, a thread of init's own, and
   processes started from two programs of a few instructions, since no
   program of the system waits forever or kills its own job. The kernel's
   own test (object_test.c) covers the shapes of job trees and the state
   of interrupts; this one, what a program can observe through the calls:
   the status of each, the signals and exit codes of what is killed, and
   that a kill leaves no reference behind.

   Each check ends the processes it started by killing the job it made
   for them, and closes every handle it opened, so that the next check
   finds init's handle table as it was. */

#include "init.h"

/* How long a wait for a killed process or thread may take. */
#define KILL_WAIT_NS 5000000000ULL

/* Where a program's code and stack lie in its process, and where its
   stack pointer starts: 8 more than a multiple of 16, with the home area
   and the fifth argument's slot above it mapped, and zero (S4). */
#define PROGRAM_CODE 0x400000ULL
#define PROGRAM_STACK 0x800000ULL
#define PROGRAM_STACK_START (PROGRAM_STACK + PAGE_SIZE - 0x58)

/* The pages of init's own thread's stack, and its stack pointer's
   distance from their end, which gives it the alignment the x64
   convention gives a called function and its home area above. */
#define THREAD_STACK_PAGES 2
#define THREAD_STACK_OFFSET 0x48

/* The one byte a program writes to say it is running, which its channel
   queues. */
#define ANNOUNCEMENT 1
#define ANNOUNCE_CAPACITY 1

/* channel_write needs a valid array even for no handles. */
static const HANDLE_TRANSFER no_handles[1];

/* A second thread of the waiting program starts SLEEPER_OFFSET bytes into
   its code, and waits on no object, so that it blocks until the process
   ends. object_wait_many takes its items' address, which must lie in user
   memory even for no items, in R10: the code page. */
#define SLEEPER_OFFSET 0x40

/* Announces itself and waits forever. RCX holds the channel endpoint its
   creator gave it and RDX an address of its code page, the byte it
   writes. Both calls follow S4: argument 1 in R10, arguments 2 to 4 in
   RDX, R8 and R9, and argument 5 of channel_write at [RSP+0x28], in the
   stack's zeros. The array of handles, of which there are none, is the
   code page too, since the call checks the array whatever the count
   (S6). R12 keeps the endpoint across the first call, which leaves R10
   zero. */
static const UINT8 wait_program[] = {
    0x49, 0x89, 0xCC,                         /* mov r12, rcx */
    0x49, 0x89, 0xCA,                         /* mov r10, rcx */
    0xB8, CALL_CHANNEL_WRITE, 0, 0, 0,        /* mov eax, CALL_CHANNEL_WRITE */
    0x41, 0xB8, ANNOUNCEMENT, 0, 0, 0,        /* mov r8d, 1 (the count) */
    0x49, 0x89, 0xD1,                         /* mov r9, rdx (the handles) */
    0x0F, 0x05,                               /* syscall */
    0x4D, 0x89, 0xE2,                         /* mov r10, r12 */
    0xB8, CALL_OBJECT_WAIT_ONE, 0, 0, 0,      /* mov eax, ..._WAIT_ONE */
    0xBA, CHANNEL_READABLE, 0, 0, 0,          /* mov edx, CHANNEL_READABLE */
    0x49, 0xC7, 0xC0, 0xFF, 0xFF, 0xFF, 0xFF, /* mov r8, DEADLINE_INFINITE */
    0x0F, 0x05,                               /* syscall */
    0xEB, 0xFE,                               /* jmp . */
    [SLEEPER_OFFSET] =
        0x41, 0xBA, 0x00, 0x00, 0x40, 0x00,       /* mov r10d, PROGRAM_CODE */
        0xB8, CALL_OBJECT_WAIT_MANY, 0, 0, 0,     /* mov eax, ..._WAIT_MANY */
        0x31, 0xD2,                               /* xor edx, edx (no items) */
        0x49, 0xC7, 0xC0, 0xFF, 0xFF, 0xFF, 0xFF, /* mov r8, -1 */
        0x0F, 0x05,                               /* syscall */
        0xEB, 0xFE                                /* jmp . */
};

/* Kills the job whose handle its creator gave it in RCX, which holds
   its own process, then waits where it is: a kill that ended it first
   never gets back. */
static const UINT8 kill_program[] = {
    0x49, 0x89, 0xCA,                         /* mov r10, rcx */
    0xB8, CALL_TASK_KILL, 0, 0, 0,            /* mov eax, CALL_TASK_KILL */
    0x0F, 0x05,                               /* syscall */
    0xEB, 0xFE                                /* jmp . */
};

/* A process started from a program above, its first thread, and, for the
   waiting program, init's end of the channel it announces on. */
typedef struct {
    UINT32 process;
    UINT32 thread;
    UINT32 mine;
} TARGET;

static void close_handle(UINT32 *handle)
{
    if (*handle != HANDLE_INVALID)
        handle_close(*handle);
    *handle = HANDLE_INVALID;
}

/* End a job and every process in it, and close init's handle to it. */
static void end_job(UINT32 *job)
{
    if (*job != HANDLE_INVALID)
        task_kill(*job);
    close_handle(job);
}

static void close_target(TARGET *target)
{
    close_handle(&target->thread);
    close_handle(&target->process);
    close_handle(&target->mine);
}

static int await(UINT32 handle, UINT32 signals)
{
    UINT64 now;
    UINT32 observed = 0;

    if (clock_monotonic(&now) != OK)
        return 0;
    return object_wait_one(handle, signals, now + KILL_WAIT_NS, &observed) ==
               OK &&
           (observed & signals) != 0;
}

static int state_is(UINT32 process, UINT32 state, UINT32 exit_code)
{
    UINT32 reported_state, reported_code;

    return process_info(process, &reported_state, &reported_code) == OK &&
           reported_state == state && reported_code == exit_code;
}

/* A child job of parent with no interrupt vectors. */
static int make_job(UINT32 parent, UINT32 *job)
{
    JOB_LIMITS limits;

    limits.interrupt_vectors = 0;
    return job_create(parent, &limits, job) == OK;
}

/* Put code in a process's address space with a stack. */
static int load_program(UINT32 process, const UINT8 *code, UINT32 size)
{
    UINT32 code_section = HANDLE_INVALID, stack_section = HANDLE_INVALID;
    UINT8 *page;
    UINT32 i;
    int loaded = 0;

    if (section_create(PAGE_SIZE, 0, &code_section) == OK &&
        section_create(PAGE_SIZE, 0, &stack_section) == OK &&
        (page = map_into_init(code_section, PAGE_SIZE,
                              MAP_READ | MAP_WRITE)) != 0) {
        for (i = 0; i < size; i++)
            page[i] = code[i];
        loaded = unmap_from_init(page, PAGE_SIZE) == OK &&
                 process_map(process, code_section, 0, PROGRAM_CODE,
                             PAGE_SIZE, MAP_READ | MAP_EXECUTE) == OK &&
                 process_map(process, stack_section, 0, PROGRAM_STACK,
                             PAGE_SIZE, MAP_READ | MAP_WRITE) == OK;
    }
    close_handle(&code_section);
    close_handle(&stack_section);
    return loaded;
}

/* Start a process of job from code, with bootstrap, which leaves init's
   table, as its argument. Returns 1, or 0 with nothing open and
   bootstrap still init's. */
static int start_process(UINT32 job, const UINT8 *code, UINT32 size,
                         UINT32 bootstrap, TARGET *target)
{
    target->process = HANDLE_INVALID;
    target->thread = HANDLE_INVALID;
    if (process_create(job, 0, &target->process) == OK &&
        thread_create(target->process, &target->thread) == OK &&
        load_program(target->process, code, size) &&
        process_start(target->process, target->thread, PROGRAM_CODE,
                      PROGRAM_STACK_START, bootstrap, PROGRAM_CODE) == OK)
        return 1;
    close_handle(&target->thread);
    close_handle(&target->process);
    return 0;
}

/* Start the waiting program in job and wait until it says it is running.
   It holds the only other endpoint of its channel, so it is the one
   thing keeping that channel's peer open. On one processor the program,
   which wakes init by writing its announcement, runs on to its wait
   before init runs again; if the timer ended its slice first, a kill
   meets it before the wait instead, which stops it as well (O10). */
static const char *start_waiter(UINT32 job, TARGET *target)
{
    UINT32 theirs;

    target->process = HANDLE_INVALID;
    target->thread = HANDLE_INVALID;
    target->mine = HANDLE_INVALID;
    if (channel_create(ANNOUNCE_CAPACITY, &target->mine, &theirs) != OK)
        return "channel_create failed";
    if (!start_process(job, wait_program, sizeof wait_program, theirs,
                       target)) {
        handle_close(theirs);
        close_handle(&target->mine);
        return "a process did not start";
    }
    if (!await(target->mine, CHANNEL_READABLE))
        return "a process did not run";
    return 0;
}

/* The waiting program has ended with exit_code, and its handles have
   closed: its endpoint's peer, init's, reads PEER_CLOSED. */
static const char *check_ended(const TARGET *target, UINT32 exit_code)
{
    if (!await(target->process, PROCESS_TERMINATED))
        return "a killed process did not end";
    if (!state_is(target->process, PROCESS_STATE_TERMINATED, exit_code))
        return "a killed process ended with another exit code";
    if (!await(target->mine, CHANNEL_PEER_CLOSED))
        return "a killed process kept its handles";
    return 0;
}

/* The status of every call that refuses its handle, in the order of
   objects.md O3: the value, then the type, then the right. A thread
   killed before it started never starts. */
static const char *check_refusals(UINT32 self, UINT32 job)
{
    UINT32 channel = HANDLE_INVALID, other = HANDLE_INVALID;
    UINT32 child = HANDLE_INVALID, restricted = HANDLE_INVALID;
    UINT32 thread = HANDLE_INVALID, watcher = HANDLE_INVALID;
    const char *failure = 0;

    if (task_kill(HANDLE_INVALID) != ERR_BAD_HANDLE)
        return "task_kill took HANDLE_INVALID";
    if (task_kill(self) != ERR_ACCESS_DENIED)
        return "task_kill ended a process without TERMINATE";
    if (channel_create(ANNOUNCE_CAPACITY, &channel, &other) != OK ||
        !make_job(job, &child) ||
        handle_duplicate(child, JOB_RIGHT_CREATE_PROCESS, &restricted) != OK ||
        thread_create(self, &thread) != OK ||
        handle_duplicate(thread, RIGHT_WAIT, &watcher) != OK)
        failure = "no handle for the refusals";
    else if (task_kill(channel) != ERR_WRONG_TYPE)
        failure = "task_kill took a channel";
    else if (task_kill(restricted) != ERR_ACCESS_DENIED)
        failure = "task_kill ended a job without TERMINATE";
    else if (task_kill(watcher) != ERR_ACCESS_DENIED)
        failure = "task_kill ended a thread without TERMINATE";
    else if (task_kill(thread) != OK || task_kill(thread) != OK)
        failure = "task_kill refused a thread never started";
    else if (!await(watcher, THREAD_TERMINATED))
        failure = "a thread killed before it started did not end";
    else if (thread_start(thread, 0, 0, 0, 0) != ERR_BAD_STATE)
        failure = "a thread killed before it started was started";
    close_handle(&watcher);
    close_handle(&thread);
    close_handle(&restricted);
    end_job(&child);
    close_handle(&other);
    close_handle(&channel);
    return failure;
}

/* process_start refuses a thread killed before it started, and the
   bootstrap handle the refused start was given stays init's. */
static const char *check_unstarted(UINT32 job)
{
    UINT32 scenario = HANDLE_INVALID, process = HANDLE_INVALID;
    UINT32 thread = HANDLE_INVALID, channel = HANDLE_INVALID;
    UINT32 other = HANDLE_INVALID;
    const char *failure = 0;

    if (!make_job(job, &scenario) ||
        process_create(scenario, 0, &process) != OK ||
        thread_create(process, &thread) != OK ||
        channel_create(ANNOUNCE_CAPACITY, &channel, &other) != OK)
        failure = "no handle for the unstarted thread";
    else if (task_kill(thread) != OK)
        failure = "task_kill refused a thread never started";
    else if (process_start(process, thread, PROGRAM_CODE, PROGRAM_STACK_START,
                           other, 0) != ERR_BAD_STATE)
        failure = "a process was started with a thread that had been killed";
    else if (handle_close(other) != OK)
        failure = "a refused process_start took its bootstrap handle";
    else
        other = HANDLE_INVALID;
    close_handle(&other);
    close_handle(&channel);
    close_handle(&thread);
    close_handle(&process);
    end_job(&scenario);
    return failure;
}

/* A thread of init's own: it announces itself and waits on a channel
   nothing writes. */
static void parked(UINT64 park, UINT64 announce)
{
    static const UINT8 byte = ANNOUNCEMENT;
    UINT32 observed;

    channel_write((UINT32)announce, &byte, sizeof byte, no_handles, 0);
    object_wait_one((UINT32)park, CHANNEL_READABLE, DEADLINE_INFINITE,
                    &observed);
    thread_exit();
}

/* One thread of a process that has others ends alone: the process goes
   on, and the handle the thread waited through stays open. */
static const char *check_thread(UINT32 self)
{
    UINT32 park = HANDLE_INVALID, parked_end = HANDLE_INVALID;
    UINT32 announce = HANDLE_INVALID, heard = HANDLE_INVALID;
    UINT32 thread = HANDLE_INVALID, section = HANDLE_INVALID;
    UINT8 *stack = 0;
    UINT32 observed;
    const char *failure = 0;

    if (channel_create(ANNOUNCE_CAPACITY, &park, &parked_end) != OK ||
        channel_create(ANNOUNCE_CAPACITY, &announce, &heard) != OK ||
        section_create(THREAD_STACK_PAGES * PAGE_SIZE, 0, &section) != OK ||
        !(stack = map_into_init(section, THREAD_STACK_PAGES * PAGE_SIZE,
                                MAP_READ | MAP_WRITE)) ||
        thread_create(self, &thread) != OK)
        failure = "no handle for init's thread";
    else if (thread_start(thread, (UINT64)parked,
                          (UINT64)stack + THREAD_STACK_PAGES * PAGE_SIZE -
                              THREAD_STACK_OFFSET,
                          parked_end, announce) != OK)
        failure = "thread_start failed";
    else if (!await(heard, CHANNEL_READABLE))
        failure = "a thread of init's did not run";
    else if (task_kill(thread) != OK)
        failure = "task_kill refused a thread";
    else if (!await(thread, THREAD_TERMINATED))
        failure = "a killed thread did not end";
    else if (object_wait_one(park, CHANNEL_PEER_CLOSED, 0, &observed) !=
             ERR_TIMED_OUT)
        failure = "killing a thread closed a handle of its process";
    close_handle(&thread);
    close_handle(&section);
    if (stack && unmap_from_init(stack, THREAD_STACK_PAGES * PAGE_SIZE) != OK &&
        !failure)
        failure = "a killed thread's stack stayed mapped";
    close_handle(&parked_end);
    close_handle(&park);
    close_handle(&heard);
    close_handle(&announce);
    return failure;
}

/* A process blocked in a wait ends with PROCESS_EXIT_KILLED, and its
   handles close; killing it again, once it has ended, is not an error. */
static const char *check_process(UINT32 job)
{
    UINT32 scenario = HANDLE_INVALID;
    TARGET target;
    const char *failure;

    if (!make_job(job, &scenario))
        return "job_create failed";
    failure = start_waiter(scenario, &target);
    if (!failure && task_kill(target.process) != OK)
        failure = "task_kill refused a process";
    if (!failure)
        failure = check_ended(&target, PROCESS_EXIT_KILLED);
    if (!failure && task_kill(target.process) != OK)
        failure = "task_kill refused a process that had ended";
    close_target(&target);
    end_job(&scenario);
    return failure;
}

/* A process whose last thread is killed ends as it does when its last
   thread exits, with exit code 0 (O10). */
static const char *check_last_thread(UINT32 job)
{
    UINT32 scenario = HANDLE_INVALID;
    TARGET target;
    const char *failure;

    if (!make_job(job, &scenario))
        return "job_create failed";
    failure = start_waiter(scenario, &target);
    if (!failure && task_kill(target.thread) != OK)
        failure = "task_kill refused a thread";
    if (!failure && !await(target.thread, THREAD_TERMINATED))
        failure = "a killed thread did not end";
    if (!failure)
        failure = check_ended(&target, 0);
    close_target(&target);
    end_job(&scenario);
    return failure;
}

/* Killing a job ends the processes in it and in the two jobs under it, and
   none takes a process or a child job afterwards; a job beside it, with
   its process, is untouched (O7). */
static const char *check_job(UINT32 job)
{
    UINT32 scenario = HANDLE_INVALID, nested = HANDLE_INVALID;
    UINT32 twin = HANDLE_INVALID, beside = HANDLE_INVALID;
    UINT32 late = HANDLE_INVALID;
    TARGET outer, inner, sibling, bystander;
    const char *failure = 0;

    outer.process = inner.process = sibling.process = HANDLE_INVALID;
    outer.thread = inner.thread = sibling.thread = HANDLE_INVALID;
    outer.mine = inner.mine = sibling.mine = HANDLE_INVALID;
    bystander.process = bystander.thread = bystander.mine = HANDLE_INVALID;
    if (!make_job(job, &scenario) || !make_job(scenario, &nested) ||
        !make_job(scenario, &twin) || !make_job(job, &beside))
        failure = "job_create failed";
    if (!failure)
        failure = start_waiter(scenario, &outer);
    if (!failure)
        failure = start_waiter(nested, &inner);
    if (!failure)
        failure = start_waiter(twin, &sibling);
    if (!failure)
        failure = start_waiter(beside, &bystander);
    if (!failure && task_kill(scenario) != OK)
        failure = "task_kill refused a job";
    if (!failure)
        failure = check_ended(&outer, PROCESS_EXIT_KILLED);
    if (!failure)
        failure = check_ended(&inner, PROCESS_EXIT_KILLED);
    if (!failure)
        failure = check_ended(&sibling, PROCESS_EXIT_KILLED);
    if (!failure && !state_is(bystander.process, PROCESS_STATE_RUNNING, 0))
        failure = "killing a job ended a process outside it";
    if (!failure &&
        (process_create(scenario, 0, &late) != ERR_BAD_STATE ||
         process_create(nested, 0, &late) != ERR_BAD_STATE))
        failure = "a killed job took a process";
    if (!failure && (make_job(scenario, &late) || make_job(nested, &late)))
        failure = "a killed job took a child job";
    if (!failure && process_create(beside, 0, &late) != OK)
        failure = "a job outside the kill refused a process";
    close_handle(&late);
    close_target(&bystander);
    close_target(&sibling);
    close_target(&inner);
    close_target(&outer);
    end_job(&beside);
    end_job(&twin);
    end_job(&nested);
    end_job(&scenario);
    return failure;
}

/* A process that kills its own job is ended by the kill, with the code a
   kill gives. */
static const char *check_own_job(UINT32 job)
{
    UINT32 scenario = HANDLE_INVALID, lent = HANDLE_INVALID;
    TARGET target;
    const char *failure = 0;

    target.process = target.thread = target.mine = HANDLE_INVALID;
    if (!make_job(job, &scenario) ||
        handle_duplicate(scenario, RIGHTS_SAME, &lent) != OK)
        failure = "no job for the process to kill";
    else if (!start_process(scenario, kill_program, sizeof kill_program, lent,
                            &target)) {
        failure = "a process did not start";
        close_handle(&lent);
    } else if (!await(target.process, PROCESS_TERMINATED))
        failure = "a process that killed its own job did not end";
    else if (!state_is(target.process, PROCESS_STATE_TERMINATED,
                       PROCESS_EXIT_KILLED))
        failure = "a process that killed its own job ended with another code";
    close_target(&target);
    end_job(&scenario);
    return failure;
}

/* A process with two threads blocked, one on a channel and one on no
   object, ends whole when it is killed: every thread of it stops, not the
   first alone (O10). */
static const char *check_threads(UINT32 job)
{
    UINT32 scenario = HANDLE_INVALID, second = HANDLE_INVALID;
    TARGET target;
    const char *failure;

    if (!make_job(job, &scenario))
        return "job_create failed";
    failure = start_waiter(scenario, &target);
    if (!failure &&
        (thread_create(target.process, &second) != OK ||
         thread_start(second, PROGRAM_CODE + SLEEPER_OFFSET,
                      PROGRAM_STACK_START, 0, 0) != OK))
        failure = "a second thread did not start";
    if (!failure && task_kill(target.process) != OK)
        failure = "task_kill refused a process";
    if (!failure)
        failure = check_ended(&target, PROCESS_EXIT_KILLED);
    if (!failure && !await(second, THREAD_TERMINATED))
        failure = "a killed process kept one of its threads";
    close_handle(&second);
    close_target(&target);
    end_job(&scenario);
    return failure;
}

/* The most vectors a new child job of parent can be granted: each grant
   is a job made and closed at once, which gives them back. */
static UINT32 spare_vectors(UINT32 parent)
{
    JOB_LIMITS limits;
    UINT32 child, spare = 0;

    for (limits.interrupt_vectors = 1;; limits.interrupt_vectors++) {
        if (job_create(parent, &limits, &child) != OK)
            return spare;
        handle_close(child);
        spare = limits.interrupt_vectors;
    }
}

typedef enum {
    KILL_A_JOB,
    KILL_A_PROCESS,
    KILL_A_THREAD,
    KILL_KINDS
} KILL_KIND;

/* A kill holds no reference past its return. A job granted every vector
   its parent has left is destroyed, and gives them back, only when
   nothing refers to it any more, to a process in it, or to that
   process's thread, so after a kill of the job, the process or the
   thread, and the closing of init's handles, the same grant is made
   again. */
static const char *check_leak(UINT32 job, UINT32 spare, KILL_KIND kind)
{
    JOB_LIMITS limits;
    UINT32 held = HANDLE_INVALID, again = HANDLE_INVALID;
    TARGET target;
    const char *failure;

    limits.interrupt_vectors = spare;
    if (job_create(job, &limits, &held) != OK)
        return "job_create refused the vectors its parent had left";
    failure = start_waiter(held, &target);
    if (!failure &&
        task_kill(kind == KILL_A_JOB       ? held
                  : kind == KILL_A_PROCESS ? target.process
                                           : target.thread) != OK)
        failure = "task_kill refused what it was given";
    if (!failure && !await(target.process, PROCESS_TERMINATED))
        failure = "a killed task's process did not end";
    if (!failure && !await(target.thread, THREAD_TERMINATED))
        failure = "a killed task's thread did not end";
    close_target(&target);
    close_handle(&held);
    if (!failure && job_create(job, &limits, &again) != OK)
        failure = "a kill left a reference to the job, its process or "
                  "its thread";
    close_handle(&again);
    return failure;
}

static const char *check_leaks(UINT32 job)
{
    UINT32 spare = spare_vectors(job);
    const char *failure = spare ? 0 : "init's job has no vector to lend";
    UINT32 kind;

    for (kind = 0; !failure && kind < KILL_KINDS; kind++)
        failure = check_leak(job, spare, (KILL_KIND)kind);
    return failure;
}

const char *kill_check(UINT32 self, UINT32 job)
{
    const char *failure = check_refusals(self, job);

    if (!failure)
        failure = check_unstarted(job);
    if (!failure)
        failure = check_thread(self);
    if (!failure)
        failure = check_process(job);
    if (!failure)
        failure = check_last_thread(job);
    if (!failure)
        failure = check_threads(job);
    if (!failure)
        failure = check_job(job);
    if (!failure)
        failure = check_own_job(job);
    if (!failure)
        failure = check_leaks(job);
    return failure;
}
