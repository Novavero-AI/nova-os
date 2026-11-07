/* Kernel objects, their references and handles, and handle tables. */

#include "object.h"
#include "cpu.h"
#include "../mm/page.h"

enum { HANDLE_FREE, HANDLE_RESERVED, HANDLE_IN_USE };

/* A handle's generation runs from 1 to 255 and skips 0 when it wraps, so
   no handle value is 0 (O3). */
#define GENERATION_FIRST 1
#define GENERATION_LAST HANDLE_GENERATION_MASK

static UINT64 next_id = 1;

/* The work list: objects whose last handle has gone and whose
   last-handle rule has yet to run, and objects whose last reference has
   gone, yet to be destroyed. */
static OBJECT *closing;
static OBJECT *dying;
static int working;

static void push(OBJECT **list, OBJECT *object)
{
    object->next_pending = *list;
    *list = object;
}

static OBJECT *pop(OBJECT **list)
{
    OBJECT *object = *list;

    if (object)
        *list = object->next_pending;
    return object;
}

static void drop_reference(OBJECT *object)
{
    if (--object->references == 0)
        push(&dying, object);
}

/* Run the work list until it is empty. A rule or destructor that closes
   handles or releases references adds to the list, and this loop, not a
   nested call, gets to them; a call made while the loop runs only adds.
   A closed handle's reference goes after its rule has run. */
static void work(void)
{
    OBJECT *object;

    if (working)
        return;
    working = 1;
    for (;;) {
        if ((object = pop(&closing)) != 0) {
            if (object->type->last_handle)
                object->type->last_handle(object);
            drop_reference(object);
        } else if ((object = pop(&dying)) != 0) {
            object->type->destroy(object);
        } else {
            break;
        }
    }
    working = 0;
}

void object_init(OBJECT *object, const OBJECT_TYPE *type)
{
    UINT64 flags = save_and_disable_interrupts();

    object->type = type;
    object->id = next_id++;
    object->references = 1;
    object->handles = 1;
    object->next_pending = 0;
    restore_interrupts(flags);
}

void object_retain(OBJECT *object)
{
    UINT64 flags = save_and_disable_interrupts();

    object->references++;
    restore_interrupts(flags);
}

void object_release(OBJECT *object)
{
    UINT64 flags = save_and_disable_interrupts();

    drop_reference(object);
    work();
    restore_interrupts(flags);
}

void object_add_handle(OBJECT *object)
{
    UINT64 flags = save_and_disable_interrupts();

    object->references++;
    object->handles++;
    restore_interrupts(flags);
}

void object_close(OBJECT *object)
{
    UINT64 flags = save_and_disable_interrupts();

    if (--object->handles == 0)
        push(&closing, object);
    else
        drop_reference(object);
    work();
    restore_interrupts(flags);
}

/* ------------------------------------------------------------------------
   Handle tables. */

HANDLE_TABLE *handle_table_create(void)
{
    HANDLE_TABLE *table = page_alloc_mapped();
    UINT32 i;

    if (!table)
        return 0;
    for (i = 0; i < HANDLE_TABLE_CAPACITY; i++)
        table->entries[i].generation = GENERATION_FIRST;
    return table;
}

/* The entry value names, or 0. A value of generation 0, HANDLE_INVALID
   among them, never matches. The index field is a caller's index into the
   table, masked after its range check (docs/spec/speculation.md M4). */
static HANDLE_ENTRY *entry_of(HANDLE_TABLE *table, UINT32 value)
{
    UINT32 index = value >> HANDLE_GENERATION_BITS;
    HANDLE_ENTRY *entry;

    if (index >= HANDLE_TABLE_CAPACITY)
        return 0;
    entry = &table->entries[index_masked(index, HANDLE_TABLE_CAPACITY)];
    if (entry->state != HANDLE_IN_USE ||
        entry->generation != (value & HANDLE_GENERATION_MASK))
        return 0;
    return entry;
}

/* Free entry, of table, and return the handle it held, ending every wait
   made through it first (O9). The next handle placed there gets the next
   generation, so value names nothing from now on. With interrupts
   disabled. */
static OBJECT *free_entry(HANDLE_TABLE *table, HANDLE_ENTRY *entry)
{
    OBJECT *object = entry->object;
    UINT32 index = (UINT32)(entry - table->entries);

    if (object->type->handle_waits)
        waitable_cancel(object->type->handle_waits(object), table,
                        handle_value(table, index));
    entry->object = 0;
    entry->rights = 0;
    entry->state = HANDLE_FREE;
    entry->generation = entry->generation == GENERATION_LAST
                            ? GENERATION_FIRST
                            : entry->generation + 1;
    return object;
}

static void close_entry(HANDLE_TABLE *table, HANDLE_ENTRY *entry)
{
    object_close(free_entry(table, entry));
}

void handle_table_close_all(HANDLE_TABLE *table)
{
    UINT64 flags = save_and_disable_interrupts();
    UINT32 i;

    for (i = 0; i < HANDLE_TABLE_CAPACITY; i++)
        if (table->entries[i].state == HANDLE_IN_USE)
            close_entry(table, &table->entries[i]);
    restore_interrupts(flags);
}

void handle_table_destroy(HANDLE_TABLE *table)
{
    handle_table_close_all(table);
    page_free_mapped(table);
}

STATUS handle_reserve(HANDLE_TABLE *table, UINT32 *index)
{
    UINT64 flags = save_and_disable_interrupts();
    STATUS status = ERR_NO_MEMORY;
    UINT32 i;

    for (i = 0; i < HANDLE_TABLE_CAPACITY; i++)
        if (table->entries[i].state == HANDLE_FREE) {
            table->entries[i].state = HANDLE_RESERVED;
            *index = i;
            status = OK;
            break;
        }
    restore_interrupts(flags);
    return status;
}

UINT32 handle_value(const HANDLE_TABLE *table, UINT32 index)
{
    return index << HANDLE_GENERATION_BITS | table->entries[index].generation;
}

UINT32 handle_place(HANDLE_TABLE *table, UINT32 index, OBJECT *object,
                    UINT32 rights)
{
    UINT64 flags = save_and_disable_interrupts();
    HANDLE_ENTRY *entry = &table->entries[index];

    entry->object = object;
    entry->rights = rights;
    entry->state = HANDLE_IN_USE;
    restore_interrupts(flags);
    return handle_value(table, index);
}

void handle_unreserve(HANDLE_TABLE *table, UINT32 index)
{
    UINT64 flags = save_and_disable_interrupts();

    table->entries[index].state = HANDLE_FREE;
    restore_interrupts(flags);
}

STATUS handle_get(HANDLE_TABLE *table, UINT32 value, UINT32 type,
                  UINT32 rights, OBJECT **object)
{
    UINT64 flags = save_and_disable_interrupts();
    HANDLE_ENTRY *entry = entry_of(table, value);
    STATUS status = OK;

    if (!entry)
        status = ERR_BAD_HANDLE;
    else if (entry->object->type->number != type)
        status = ERR_WRONG_TYPE;
    else if ((entry->rights & rights) != rights)
        status = ERR_ACCESS_DENIED;
    if (status == OK) {
        *object = entry->object;
        entry->object->references++;
    }
    restore_interrupts(flags);
    return status;
}

STATUS handle_get_waitable(HANDLE_TABLE *table, UINT32 value, UINT32 rights,
                           OBJECT **object, WAITABLE **waitable)
{
    UINT64 flags = save_and_disable_interrupts();
    HANDLE_ENTRY *entry = entry_of(table, value);
    STATUS status = OK;

    if (!entry)
        status = ERR_BAD_HANDLE;
    else if (!entry->object->type->waitable)
        status = ERR_NOT_SUPPORTED;
    else if ((entry->rights & rights) != rights)
        status = ERR_ACCESS_DENIED;
    if (status == OK) {
        *object = entry->object;
        *waitable = entry->object->type->waitable(entry->object);
        entry->object->references++;
    }
    restore_interrupts(flags);
    return status;
}

STATUS handle_peek(HANDLE_TABLE *table, UINT32 value, OBJECT **object,
                   UINT32 *rights)
{
    HANDLE_ENTRY *entry = entry_of(table, value);

    if (!entry)
        return ERR_BAD_HANDLE;
    *object = entry->object;
    *rights = entry->rights;
    return OK;
}

void handle_take(HANDLE_TABLE *table, UINT32 value)
{
    free_entry(table, entry_of(table, value));
}

STATUS handle_duplicate(HANDLE_TABLE *table, UINT32 value, UINT32 rights,
                        UINT32 *duplicate)
{
    UINT64 flags = save_and_disable_interrupts();
    HANDLE_ENTRY *entry = entry_of(table, value);
    STATUS status = OK;
    UINT32 index;

    if (!entry)
        status = ERR_BAD_HANDLE;
    else if (!(entry->rights & RIGHT_DUPLICATE) ||
             (rights != RIGHTS_SAME && (rights & entry->rights) != rights))
        status = ERR_ACCESS_DENIED;
    else
        status = handle_reserve(table, &index);
    if (status == OK) {
        object_add_handle(entry->object);
        *duplicate = handle_place(table, index, entry->object,
                                  rights == RIGHTS_SAME ? entry->rights
                                                        : rights);
    }
    restore_interrupts(flags);
    return status;
}

/* TinyCC calls __chkstk, which no runtime here provides, from a function
   whose frame reaches this many bytes (random.c). */
#define CHKSTK_FRAME 4096

_Static_assert(sizeof(WAIT_ITEM) * WAIT_MANY_MAX < CHKSTK_FRAME,
               "a wait's items fit one frame below __chkstk's");

/* Check each entry's handle, then wait through them all; objects
   receives the object of each handle checked, *taken their count, for
   the caller to release. The WAIT_ITEMs live in this frame and the
   objects in the caller's, so that neither frame reaches CHKSTK_FRAME. */
static STATUS check_and_wait(HANDLE_TABLE *table, WAIT_ENTRY *entries,
                             UINT32 count, UINT64 deadline, OBJECT **objects,
                             UINT32 *taken)
{
    WAIT_ITEM items[WAIT_MANY_MAX];
    STATUS status = OK;
    UINT32 i;

    for (*taken = 0; status == OK && *taken < count;) {
        i = *taken;
        status = handle_get_waitable(table, entries[i].handle, RIGHT_WAIT,
                                     &objects[i], &items[i].object);
        if (status == OK) {
            items[i].signals = entries[i].signals;
            items[i].handle_table = table;
            items[i].handle = entries[i].handle;
            (*taken)++;
        }
    }
    if (status != OK)
        return status;
    status = wait_many(items, count, deadline);
    for (i = 0; i < count; i++)
        entries[i].observed = items[i].observed;
    return status;
}

/* Interrupts stay disabled from the first handle's check until the wait
   blocks, and again from its waking to the end, so a handle cannot leave
   the table between its check and the wait's start, where its close
   would find no wait to end. */
STATUS handle_wait(HANDLE_TABLE *table, WAIT_ENTRY *entries, UINT32 count,
                   UINT64 deadline)
{
    OBJECT *objects[WAIT_MANY_MAX];
    UINT32 taken, i;
    UINT64 flags;
    STATUS status;

    if (count > WAIT_MANY_MAX)
        return ERR_OUT_OF_RANGE;
    flags = save_and_disable_interrupts();
    status = check_and_wait(table, entries, count, deadline, objects, &taken);
    for (i = 0; i < taken; i++)
        object_release(objects[i]);
    restore_interrupts(flags);
    return status;
}

STATUS handle_close(HANDLE_TABLE *table, UINT32 value)
{
    UINT64 flags = save_and_disable_interrupts();
    HANDLE_ENTRY *entry = entry_of(table, value);

    if (entry)
        close_entry(table, entry);
    restore_interrupts(flags);
    return entry ? OK : ERR_BAD_HANDLE;
}
