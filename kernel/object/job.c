/* Jobs. */

#include "job.h"
#include "../mm/page.h"

static void destroy(OBJECT *object)
{
    page_free_mapped(object);
}

static const OBJECT_TYPE job_type = {TYPE_JOB, 0, destroy, 0};

JOB *job_create_root(void)
{
    JOB *job = page_alloc_mapped();

    if (job)
        object_init(&job->object, &job_type);
    return job;
}
