/* Jobs (docs/spec/objects.md O6, O7): a tree whose root the first
   process receives. A job carries the authority to create processes and
   child jobs, and to let its processes map pages writable and executable
   at once (O5), and a quota of the message-signalled interrupt vectors
   its processes may hold. The root job's quota is every vector the
   kernel gives such interrupts; a child's is reserved from what its
   parent has left when it is created, and given back when it is
   destroyed, so the quotas of all jobs together never exceed the
   vectors there are. The other quotas O6 lists, and termination of a
   job's processes, come with the calls that need them. */

#ifndef NOVA_JOB_H
#define NOVA_JOB_H

#include "object.h"

/* The rights of a job's handle: every right a job has (O5, rule 1)
   except WAIT, since this kernel does not yet assert a job's
   TERMINATED. A child job's handle carries WRITE_EXECUTE only if the
   parent's handle it was created through did. */
#define JOB_RIGHTS                                                       \
    (JOB_RIGHT_CREATE_PROCESS | JOB_RIGHT_CREATE_JOB |                   \
     JOB_RIGHT_TERMINATE | JOB_RIGHT_WRITE_EXECUTE | RIGHT_DUPLICATE |   \
     RIGHT_TRANSFER)

typedef struct JOB JOB;

struct JOB {
    OBJECT object;
    JOB *parent; /* held for the job's life; 0 for the root */
    UINT32 vector_limit;
    UINT32 vectors_held; /* by its processes' interrupts and as its
                            children's limits */
};

/* The root job, with its creator's handle; or 0 if no page is free. */
JOB *job_create_root(void);

/* job_create: a child of parent with limits, and its creator's handle.
   Returns OK; ERR_NO_RESOURCES if parent has fewer vectors left than
   limits asks for; or ERR_NO_MEMORY. */
STATUS job_create(JOB *parent, const JOB_LIMITS *limits, JOB **job);

/* Charge one interrupt vector to job. Returns OK, or ERR_NO_RESOURCES if
   job holds its limit already. */
STATUS job_charge_vector(JOB *job);

/* Give back a vector job_charge_vector charged. */
void job_uncharge_vector(JOB *job);

#endif
