/* Initiators and pins. A pin is on its initiator's list from its creation
   until it is released, or, once quarantined, until the quarantine is
   released; the list holds no reference, and each side unlinks the other
   before it goes. A quarantined pin holds a reference to itself, which
   keeps it, its section and the section's pages alive; one whose
   initiator has gone moves to the leaked list, where it stays. Every
   change to the lists runs with interrupts disabled. */

#include "initiator.h"
#include "cpu.h"
#include "../mm/page.h"
#include "../x86_64/serial.h"
#include "../x86_64/user.h"

#define PIN_OPTIONS (PIN_DEVICE_READ | PIN_DEVICE_WRITE)

/* Quarantined pins whose initiator's last handle has gone: the device
   may still write their pages and nobody is left to reset it (ipc.md
   I8). */
static PIN *leaked;

static void unlink_pin(PIN *pin)
{
    PIN **link;

    if (!pin->initiator)
        return;
    for (link = &pin->initiator->pins; *link; link = &(*link)->next)
        if (*link == pin) {
            *link = pin->next;
            break;
        }
    pin->initiator = 0;
    pin->next = 0;
}

/* Quarantine a pin whose last handle went without pin_release. */
static void pin_last_handle(OBJECT *object)
{
    PIN *pin = (PIN *)object;
    UINT64 flags = save_and_disable_interrupts();

    if (!pin->released) {
        pin->quarantined = 1;
        object_retain(&pin->object);
        if (!pin->initiator) {
            pin->next = leaked;
            leaked = pin;
        }
    }
    restore_interrupts(flags);
}

static void pin_destroy(OBJECT *object)
{
    PIN *pin = (PIN *)object;
    UINT64 flags = save_and_disable_interrupts();

    unlink_pin(pin);
    restore_interrupts(flags);
    object_release(&pin->section->object);
    page_free_mapped(pin);
}

static const OBJECT_TYPE pin_type = {TYPE_PIN, pin_last_handle, pin_destroy,
                                     0, 0};

/* Its quarantined pins move to the leaked list; its live pins leave its
   list, to go there too if they are quarantined later. */
static void initiator_last_handle(OBJECT *object)
{
    INITIATOR *initiator = (INITIATOR *)object;
    UINT64 flags = save_and_disable_interrupts();
    PIN *pin;

    while ((pin = initiator->pins) != 0) {
        unlink_pin(pin);
        if (pin->quarantined) {
            pin->next = leaked;
            leaked = pin;
        }
    }
    restore_interrupts(flags);
}

static void initiator_destroy(OBJECT *object)
{
    page_free_mapped(object);
}

static const OBJECT_TYPE initiator_type = {
    TYPE_INITIATOR, initiator_last_handle, initiator_destroy, 0, 0};

STATUS initiator_create(const RESOURCE *resource, UINT64 requester,
                        INITIATOR **out)
{
    INITIATOR *initiator;
    UINT64 flags;

    if (!resource_covers(resource, RESOURCE_DMA, requester, 1))
        return ERR_OUT_OF_RANGE;
    initiator = page_alloc_mapped();
    if (!initiator)
        return ERR_NO_MEMORY;
    object_init(&initiator->object, &initiator_type);
    initiator->requester = requester;
    flags = serial_lock();
    serial_write("nova: dma unconfined pci=");
    resource_log_requester(requester);
    serial_write("\r\n");
    serial_unlock(flags);
    *out = initiator;
    return OK;
}

UINT32 initiator_pin_rights(UINT32 options)
{
    return SECTION_RIGHT_PIN |
           ((options & PIN_DEVICE_READ) ? SECTION_RIGHT_READ : 0) |
           ((options & PIN_DEVICE_WRITE) ? SECTION_RIGHT_WRITE : 0);
}

static STATUS check_pin(const SECTION *section, UINT64 offset, UINT64 size,
                        UINT32 options)
{
    if (options == 0 || (options & ~PIN_OPTIONS) || size == 0 ||
        offset % PAGE_SIZE != 0 || size % PAGE_SIZE != 0 ||
        section->kind != SECTION_MEMORY)
        return ERR_INVALID_ARGS;
    if (offset / PAGE_SIZE > section->pages ||
        size / PAGE_SIZE > section->pages - offset / PAGE_SIZE)
        return ERR_OUT_OF_RANGE;
    return OK;
}

/* The addresses are written before the pin joins the list, so a fault
   leaves nothing behind: the pin, never handed out, is freed whole. The
   caller's reference keeps the initiator, but another thread of its
   process may have closed the initiator's last handle meanwhile; its
   last-handle rule has then emptied the list, which no pin may join
   after it. */
STATUS initiator_pin(INITIATOR *initiator, SECTION *section, UINT64 offset,
                     UINT64 size, UINT32 options, UINT64 addresses,
                     PIN **out)
{
    STATUS status = check_pin(section, offset, size, options);
    UINT64 first = offset / PAGE_SIZE, pages = size / PAGE_SIZE, i;
    UINT64 flags, address;
    PIN *pin;

    if (status == OK && !user_range_valid(addresses, pages * sizeof(UINT64)))
        status = ERR_BAD_ADDRESS;
    for (i = 0; status == OK && i < pages; i++) {
        address = section_page(section, first + i);
        status = copy_to_user(addresses + i * sizeof(UINT64), &address,
                              sizeof(UINT64));
    }
    if (status != OK)
        return status;
    pin = page_alloc_mapped();
    if (!pin)
        return ERR_NO_MEMORY;
    flags = save_and_disable_interrupts();
    if (initiator->object.handles == 0) {
        restore_interrupts(flags);
        page_free_mapped(pin);
        return ERR_BAD_STATE;
    }
    object_init(&pin->object, &pin_type);
    object_retain(&section->object);
    pin->section = section;
    pin->initiator = initiator;
    pin->next = initiator->pins;
    initiator->pins = pin;
    restore_interrupts(flags);
    *out = pin;
    return OK;
}

void pin_release(PIN *pin)
{
    UINT64 flags = save_and_disable_interrupts();

    pin->released = 1;
    unlink_pin(pin);
    restore_interrupts(flags);
}

void initiator_release_quarantine(INITIATOR *initiator)
{
    UINT64 flags = save_and_disable_interrupts();
    PIN **link = &initiator->pins;
    PIN *pin;

    while ((pin = *link) != 0) {
        if (!pin->quarantined) {
            link = &pin->next;
            continue;
        }
        *link = pin->next;
        pin->initiator = 0;
        pin->next = 0;
        pin->quarantined = 0;
        pin->released = 1;
        object_release(&pin->object);
    }
    restore_interrupts(flags);
}
