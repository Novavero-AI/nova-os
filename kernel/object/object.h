/* Kernel objects and handle tables (docs/spec/objects.md O3 to O5, O10).

   An object counts references and handles apart. Every handle holds a
   reference, and so do kernel uses: a thread holds its process, and a
   call holds the object a handle named for as long as it uses it. A
   handle may sit in a process's handle table, travel in a channel
   message, or be held by the kernel code that created the object until
   it places the handle. When the last handle goes the type's last-handle
   rule runs, even while references remain; when the last reference goes
   the object is destroyed. Both run from a work list, never by
   recursion, so a chain of objects that hold handles to each other ends
   in a loop, not a deep stack (O10).

   On one processor every change runs with interrupts disabled, which
   keeps out the timer and preemption alike. */

#ifndef NOVA_OBJECT_H
#define NOVA_OBJECT_H

#include "../status.h"
#include "../sched/wait.h"
#include "memory_layout.h"

typedef struct OBJECT OBJECT;

/* What the kernel does with an object of a type: its number as user code
   sees it (native_abi.h); what happens when its last handle goes, or 0
   for nothing; how it is destroyed, which frees its memory; and where its
   signals are, or 0 for a type that is not waitable (O9). */
typedef struct {
    UINT32 number;
    void (*last_handle)(OBJECT *object);
    void (*destroy)(OBJECT *object);
    WAITABLE *(*waitable)(OBJECT *object);
} OBJECT_TYPE;

struct OBJECT {
    const OBJECT_TYPE *type;
    UINT64 id; /* unique since boot, never reused (O3) */
    UINT32 references;
    UINT32 handles;
    OBJECT *next_pending; /* in the work list */
};

/* Start an object with one reference and one handle, the creator's,
   which the creator places in a table or a message or closes. */
void object_init(OBJECT *object, const OBJECT_TYPE *type);

void object_retain(OBJECT *object);
void object_release(OBJECT *object);

/* A further handle to object, with its reference, for the caller to
   place. */
void object_add_handle(OBJECT *object);

/* Close a handle the caller holds outside any table: drop it and its
   reference, running the last-handle rule if it was the last. */
void object_close(OBJECT *object);

/* ------------------------------------------------------------------------
   Handle tables. A table is one page of entries, so a process holds at
   most HANDLE_TABLE_CAPACITY handles at once, well below the 2^24 the
   handle encoding allows (O3). A call that moves handles into a table
   reserves every entry it needs first, so it can fail before it changes
   anything (docs/spec/syscalls.md S6). */

typedef struct {
    OBJECT *object;
    UINT32 rights;
    UINT16 generation; /* 1 to 255 */
    UINT16 state;      /* HANDLE_FREE, HANDLE_RESERVED or HANDLE_IN_USE */
} HANDLE_ENTRY;

#define HANDLE_TABLE_CAPACITY (PAGE_SIZE / sizeof(HANDLE_ENTRY))

typedef struct {
    HANDLE_ENTRY entries[HANDLE_TABLE_CAPACITY];
} HANDLE_TABLE;

_Static_assert(sizeof(HANDLE_TABLE) == PAGE_SIZE, "a table is one page");

/* An empty table in a page of its own, or 0 if no page is free. */
HANDLE_TABLE *handle_table_create(void);

/* Close every handle in table; it stays usable, and empty. */
void handle_table_close_all(HANDLE_TABLE *table);

/* Close every handle in table and free it. */
void handle_table_destroy(HANDLE_TABLE *table);

/* Reserve a free entry for a handle about to be placed. Returns OK and
   its index, or ERR_NO_MEMORY if the table is full. */
STATUS handle_reserve(HANDLE_TABLE *table, UINT32 *index);

/* The value the handle placed at a reserved index will have. */
UINT32 handle_value(const HANDLE_TABLE *table, UINT32 index);

/* Place a handle the caller holds, with rights, at a reserved index, and
   return its value. The handle and its reference pass to the table. */
UINT32 handle_place(HANDLE_TABLE *table, UINT32 index, OBJECT *object,
                    UINT32 rights);

/* Give back a reserved entry unused. */
void handle_unreserve(HANDLE_TABLE *table, UINT32 index);

/* The object value names, if it names a handle in table to an object of
   type number type carrying every right in rights, checked in that order
   (O3): ERR_BAD_HANDLE, ERR_WRONG_TYPE, ERR_ACCESS_DENIED. On OK the
   caller holds a reference to the object, and releases it when done. */
STATUS handle_get(HANDLE_TABLE *table, UINT32 value, UINT32 type,
                  UINT32 rights, OBJECT **object);

/* handle_get for a call that takes a waitable object of any type: the
   type check is that the type is waitable, ERR_NOT_SUPPORTED otherwise
   (docs/spec/syscalls.md S7). On OK *waitable is the object's signals. */
STATUS handle_get_waitable(HANDLE_TABLE *table, UINT32 value, UINT32 rights,
                           OBJECT **object, WAITABLE **waitable);

/* Remove the handle value names from table and close it. Returns OK or
   ERR_BAD_HANDLE. */
STATUS handle_close(HANDLE_TABLE *table, UINT32 value);

/* handle_duplicate (O4): a second handle in table to the object value
   names, with rights, RIGHTS_SAME or a subset of the handle's (O5).
   Returns OK and the new handle's value; ERR_BAD_HANDLE;
   ERR_ACCESS_DENIED if the handle lacks DUPLICATE or rights is not
   within its rights; or ERR_NO_MEMORY if the table is full. */
STATUS handle_duplicate(HANDLE_TABLE *table, UINT32 value, UINT32 rights,
                        UINT32 *duplicate);

/* For moving handles out of a table into a message, with interrupts
   disabled across both so that nothing changes between them: the object
   and rights of the handle value names, or ERR_BAD_HANDLE, taking no
   reference; and the removal of that handle without closing it, its
   handle and reference passing to the caller. */
STATUS handle_peek(HANDLE_TABLE *table, UINT32 value, OBJECT **object,
                   UINT32 *rights);
void handle_take(HANDLE_TABLE *table, UINT32 value);

#endif
