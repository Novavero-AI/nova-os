/* Jobs. A job's counts change with interrupts disabled. */

#include "job.h"
#include "cpu.h"
#include "../mm/page.h"
#include "../x86_64/x86_64.h"

/* A child's limit goes back to its parent, which it held until now. */
static void destroy(OBJECT *object)
{
    JOB *job = (JOB *)object;
    UINT64 flags;

    if (job->parent) {
        flags = save_and_disable_interrupts();
        job->parent->vectors_held -= job->vector_limit;
        restore_interrupts(flags);
        object_release(&job->parent->object);
    }
    page_free_mapped(job);
}

static const OBJECT_TYPE job_type = {TYPE_JOB, 0, destroy, 0};

static JOB *create(JOB *parent, UINT32 vector_limit)
{
    JOB *job = page_alloc_mapped();

    if (!job)
        return 0;
    object_init(&job->object, &job_type);
    job->parent = parent;
    job->vector_limit = vector_limit;
    job->vectors_held = 0;
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
    if (vectors > parent->vector_limit - parent->vectors_held)
        status = ERR_NO_RESOURCES;
    else
        parent->vectors_held += vectors;
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
