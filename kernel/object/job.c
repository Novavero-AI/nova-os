/* Jobs. A job's counts and lists change with interrupts disabled. */

#include "job.h"
#include "cpu.h"
#include "process.h"
#include "../mm/page.h"
#include "../x86_64/x86_64.h"

/* A child's limit goes back to its parent, which it held until now, and
   the child leaves the parent's list. */
static void destroy(OBJECT *object)
{
    JOB *job = (JOB *)object;
    UINT64 flags;

    if (job->parent) {
        flags = save_and_disable_interrupts();
        job->parent->vectors_held -= job->vector_limit;
        *job->sibling_link = job->next_sibling;
        if (job->next_sibling)
            job->next_sibling->sibling_link = job->sibling_link;
        restore_interrupts(flags);
        object_release(&job->parent->object);
    }
    page_free_mapped(job);
}

static const OBJECT_TYPE job_type = {TYPE_JOB, 0, destroy, 0, 0};

static JOB *create(JOB *parent, UINT32 vector_limit)
{
    JOB *job = page_alloc_mapped();

    if (!job)
        return 0;
    object_init(&job->object, &job_type);
    job->parent = parent;
    job->vector_limit = vector_limit;
    job->vectors_held = 0;
    job->killed = 0;
    job->children = 0;
    job->next_sibling = 0;
    job->sibling_link = 0;
    job->processes = 0;
    return job;
}

JOB *job_create_root(void)
{
    return create(0, MSI_VECTOR_COUNT);
}

STATUS job_create(JOB *parent, const JOB_LIMITS *limits, JOB **out)
{
    UINT32 vectors = limits->interrupt_vectors;
    JOB *job = create(parent, vectors);
    STATUS status = OK;
    UINT64 flags;

    if (!job)
        return ERR_NO_MEMORY;
    flags = save_and_disable_interrupts();
    if (parent->killed)
        status = ERR_BAD_STATE;
    else if (vectors > parent->vector_limit - parent->vectors_held)
        status = ERR_NO_RESOURCES;
    else {
        parent->vectors_held += vectors;
        job->next_sibling = parent->children;
        if (job->next_sibling)
            job->next_sibling->sibling_link = &job->next_sibling;
        parent->children = job;
        job->sibling_link = &parent->children;
    }
    restore_interrupts(flags);
    if (status != OK) {
        page_free_mapped(job);
        return status;
    }
    object_retain(&parent->object);
    *out = job;
    return OK;
}

STATUS job_charge_vector(JOB *job)
{
    UINT64 flags = save_and_disable_interrupts();
    STATUS status = ERR_NO_RESOURCES;

    if (job->vectors_held < job->vector_limit) {
        job->vectors_held++;
        status = OK;
    }
    restore_interrupts(flags);
    return status;
}

void job_uncharge_vector(JOB *job)
{
    UINT64 flags = save_and_disable_interrupts();

    job->vectors_held--;
    restore_interrupts(flags);
}

/* ------------------------------------------------------------------------
   Processes and termination. */

STATUS job_add_process(PROCESS *process)
{
    JOB *job = process->job;
    UINT64 flags = save_and_disable_interrupts();
    STATUS status = OK;

    if (job->killed) {
        status = ERR_BAD_STATE;
    } else {
        process->next_in_job = job->processes;
        if (process->next_in_job)
            process->next_in_job->link_in_job = &process->next_in_job;
        job->processes = process;
        process->link_in_job = &job->processes;
        object_retain(&job->object);
    }
    restore_interrupts(flags);
    return status;
}

void job_remove_process(PROCESS *process)
{
    UINT64 flags = save_and_disable_interrupts();

    *process->link_in_job = process->next_in_job;
    if (process->next_in_job)
        process->next_in_job->link_in_job = process->link_in_job;
    restore_interrupts(flags);
    object_release(&process->job->object);
}

/* The tree is walked in place, in one stretch with interrupts disabled,
   since a process created or destroyed meanwhile would change the lists
   under the walk: it visits each job, then its children, and climbs by
   the parent links. A process with no thread left to finish it is
   finished after the walk, so that the stretch holds list work alone
   and not the unmapping of sections. It is held until then: interrupts
   are enabled between the two, its creator may close the last handle to
   it in that time, and without the hold that would free it while it is
   on the list of those to finish.

   The stretch visits each job, each process and each started thread of
   the tree once, in a fixed number of steps apiece: a stopped thread
   leaves the timeout list in constant time (thread.c), so no step
   searches a list. Each is memory from the page pool: a page for a job,
   three for a process (its own, its handle table and the root of its
   space) and seven for a started thread (its own, its FPU state, a guard
   page and four of stack). The visits are therefore at most the pool's
   pages, 65,536 for the 256 MiB the boots run in, and a chain of jobs,
   one page each, is the longest walk. That is accepted rather than
   bounded: it is linear in what dies, and only a tree that fills the
   memory makes it long. Splitting it would leave the lists between
   steps for a creation or a destruction to change under the walk. */
void job_kill(JOB *job)
{
    PROCESS *unfinished = 0;
    JOB *node = job;
    UINT64 flags = save_and_disable_interrupts();

    for (;;) {
        PROCESS *process;

        node->killed = 1;
        for (process = node->processes; process;
             process = process->next_in_job)
            if (process_stop(process, PROCESS_EXIT_KILLED)) {
                object_retain(&process->object);
                process->next_unfinished = unfinished;
                unfinished = process;
            }
        if (node->children) {
            node = node->children;
            continue;
        }
        while (node != job && !node->next_sibling)
            node = node->parent;
        if (node == job)
            break;
        node = node->next_sibling;
    }
    restore_interrupts(flags);
    while (unfinished) {
        PROCESS *process = unfinished;

        unfinished = process->next_unfinished;
        process_finish(process);
        object_release(&process->object);
    }
}
