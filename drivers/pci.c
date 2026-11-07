/* What the platform driver and PCI device drivers share. */

#include "pci.h"
#include "log.h"

#define SEGMENT_DIGITS 4
#define BUS_DIGITS 2
#define DEVICE_DIGITS 2
#define FUNCTION_DIGITS 1
#define BUS_MASK 0xff

void pci_say_function(UINT32 requester)
{
    say_hex_width(requester >> PCI_SEGMENT_SHIFT, SEGMENT_DIGITS);
    say(":");
    say_hex_width(requester >> PCI_REQUESTER_BUS_SHIFT & BUS_MASK,
                  BUS_DIGITS);
    say(":");
    say_hex_width(requester >> PCI_REQUESTER_DEVICE_SHIFT &
                      (PCI_DEVICES - 1),
                  DEVICE_DIGITS);
    say(".");
    say_hex_width(requester & (PCI_FUNCTIONS - 1), FUNCTION_DIGITS);
}
