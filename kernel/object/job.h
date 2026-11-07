/* Jobs (docs/spec/objects.md O7). This kernel has the root job alone,
   which the first process receives; it carries the authority to create
   processes, and to let them map pages writable and executable at once
   (O5). Quotas, child jobs and termination of a job's processes come
   with the calls that need them. */

#ifndef NOVA_JOB_H
#define NOVA_JOB_H

#include "object.h"

/* The rights of the root job's handle: every right a job has (O5, rule
   1) except WAIT, since this kernel does not yet assert a job's
   TERMINATED. */
#define ROOT_JOB_RIGHTS                                                  \
    (JOB_RIGHT_CREATE_PROCESS | JOB_RIGHT_CREATE_JOB |                   \
     JOB_RIGHT_TERMINATE | JOB_RIGHT_WRITE_EXECUTE | RIGHT_DUPLICATE |   \
     RIGHT_TRANSFER)

typedef struct {
    OBJECT object;
} JOB;

/* The root job, with its creator's handle; or 0 if no page is free. */
JOB *job_create_root(void);

#endif
