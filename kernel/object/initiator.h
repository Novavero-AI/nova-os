/* Initiators and pins (docs/spec/ipc.md I8). An initiator is one bus
   requester ID's authority to do DMA; a pin holds pages of a memory
   section for that device. A pin ends with pin_release; if its last
   handle goes any other way it is quarantined: its pages stay allocated,
   held by the initiator, until a holder of RELEASE calls
   initiator_release_quarantine, or for the life of the system once the
   initiator's own last handle has gone. This kernel drives no IOMMU, so
   a device address is the page's physical address and an initiator
   confines nothing. */

#ifndef NOVA_INITIATOR_H
#define NOVA_INITIATOR_H

#include "section.h"

/* The rights of new initiator and pin handles: every right each has. */
#define INITIATOR_RIGHTS                                                 \
    (INITIATOR_RIGHT_PIN | INITIATOR_RIGHT_RELEASE | RIGHT_DUPLICATE |   \
     RIGHT_TRANSFER)
#define PIN_RIGHTS (RIGHT_DUPLICATE | RIGHT_TRANSFER)

typedef struct PIN PIN;

typedef struct {
    OBJECT object;
    UINT64 requester;
    PIN *pins; /* its pins, live and quarantined */
} INITIATOR;

struct PIN {
    OBJECT object;
    INITIATOR *initiator; /* 0 once off the initiator's list */
    PIN *next;
    SECTION *section; /* holds a reference, which keeps the pages */
    int released;
    int quarantined;
};

/* initiator_create: an initiator for requester, which resource, a DMA
   resource, must cover. Returns OK, ERR_OUT_OF_RANGE or ERR_NO_MEMORY. */
STATUS initiator_create(const RESOURCE *resource, UINT64 requester,
                        INITIATOR **initiator);

/* The section rights section_pin needs for options: PIN, and READ or
   WRITE as the device reads or writes the pages (ipc.md I8). */
UINT32 initiator_pin_rights(UINT32 options);

/* section_pin: pin size bytes of section from offset for initiator's
   device, which options says reads them, writes them or both, and write
   one device address per page, a UINT64 each, to the user array at
   addresses. The caller holds the section by a handle with
   initiator_pin_rights(options). Returns OK; ERR_INVALID_ARGS for
   unknown or no options, a size of 0, an unaligned offset or size, or a
   device section; ERR_OUT_OF_RANGE past the section's end;
   ERR_BAD_ADDRESS, with no pin made; ERR_BAD_STATE if the initiator's
   last handle has gone; or ERR_NO_MEMORY. */
STATUS initiator_pin(INITIATOR *initiator, SECTION *section, UINT64 offset,
                     UINT64 size, UINT32 options, UINT64 addresses,
                     PIN **pin);

/* pin_release's part before the handle closes: the pin ends, and closing
   its handles no longer quarantines it. */
void pin_release(PIN *pin);

/* initiator_release_quarantine: free initiator's quarantined pins. */
void initiator_release_quarantine(INITIATOR *initiator);

#endif
