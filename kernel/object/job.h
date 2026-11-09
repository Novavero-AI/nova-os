/* Jobs (docs/spec/objects.md O6, O7): a tree whose root the first
   process receives. A job carries the authority to create processes and
   child jobs, and to let its processes map pages writable and executable
   at once (O5), and a quota of the message-signalled interrupt vectors
   its processes may hold. The root job's quota is every vector the
   kernel gives such interrupts; a child's is reserved from what its
   parent has left when it is created, and given back when it is
   destroyed, so the quotas of all jobs together never exceed the
   vectors there are. A job's processes and child jobs are on lists,
   which task_kill walks to end them (O7). The other quotas O6 lists come
   with the calls that need them. */

#ifndef NOVA_JOB_H
#define NOVA_JOB_H

#include "object.h"

/* The rights of a job's handle: every right a job has (O5, rule 1)
   except WAIT, since a job has no signal to wait for (O9). A child job's
   handle carries WRITE_EXECUTE only if the parent's handle it was
   created through did. */
#define JOB_RIGHTS                                                       \
    (JOB_RIGHT_CREATE_PROCESS | JOB_RIGHT_CREATE_JOB |                   \
     JOB_RIGHT_TERMINATE | JOB_RIGHT_WRITE_EXECUTE | RIGHT_DUPLICATE |   \
     RIGHT_TRANSFER)

typedef struct JOB JOB;
typedef struct PROCESS PROCESS; /* process.h */

/* The lists hold no references: a child holds its parent and a process
   its job, so each is on its list from its creation until it is
   destroyed, and leaves the list as it goes. An entry's link is the
   pointer that points to it, which makes leaving a list one write. A
   list changes with interrupts disabled. */
struct JOB {
    OBJECT object;
    JOB *parent; /* held for the job's life; 0 for the root */
    UINT32 vector_limit;
    UINT32 vectors_held; /* by its processes' interrupts and as its
                            children's limits */
    UINT32 killed;       /* from job_kill on: nothing joins the job */
    JOB *children;       /* the jobs whose parent this is */
    JOB *next_sibling;   /* in parent's children */
    JOB **sibling_link;
    PROCESS *processes;  /* the processes created in this job */
};

/* The root job, with its creator's handle; or 0 if no page is free. */
JOB *job_create_root(void);

/* job_create: a child of parent with limits, and its creator's handle.
   Returns OK; ERR_BAD_STATE if parent has been killed;
   ERR_NO_RESOURCES if parent has fewer vectors left than limits asks
   for; or ERR_NO_MEMORY. */
STATUS job_create(JOB *parent, const JOB_LIMITS *limits, JOB **job);

/* Put process, whose job field is set and which holds nothing yet to
   undo, on its job's list, and take the job's reference for it. Returns
   OK, or ERR_BAD_STATE, taking nothing, if the job has been killed. */
STATUS job_add_process(PROCESS *process);

/* Take process off its job's list and drop the job's reference. */
void job_remove_process(PROCESS *process);

/* End job (O7): mark it and every job under it killed, so that none
   takes a process or a child job again, and stop every process in them
   with PROCESS_EXIT_KILLED (process_stop). Returns once each is
   stopping, not ended: a process with threads ends as its last thread
   exits, and one with none ends here. A killed job can be killed again,
   with no effect. */
void job_kill(JOB *job);

/* Charge one interrupt vector to job. Returns OK, or ERR_NO_RESOURCES if
   job holds its limit already. */
STATUS job_charge_vector(JOB *job);

/* Give back a vector job_charge_vector charged. */
void job_uncharge_vector(JOB *job);

#endif
