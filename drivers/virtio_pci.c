/* The virtio PCI transport's capabilities and device reset. */

#include "virtio_pci.h"
#include "mmio.h"
#include "pci.h"

void virtio_pci_capabilities(const volatile UINT8 *config,
                             UINT32 *structures, UINT32 *msix)
{
    UINT32 at, steps, type;

    for (type = 0; type < VIRTIO_CAP_TYPES; type++)
        structures[type] = 0;
    *msix = 0;
    if (!(mmio_read16(config + PCI_STATUS) & PCI_STATUS_CAPABILITIES))
        return;
    at = mmio_read8(config + PCI_CAPABILITIES) & PCI_CAP_POINTER_MASK;
    for (steps = 0; at && steps < PCI_CAPS_MAX; steps++) {
        UINT32 id = mmio_read8(config + at + PCI_CAP_ID);

        if (id == PCI_CAP_MSIX && !*msix)
            *msix = at;
        if (id == PCI_CAP_VENDOR) {
            type = mmio_read8(config + at + VIRTIO_CAP_CFG_TYPE);
            if (type < VIRTIO_CAP_TYPES && !structures[type])
                structures[type] = at;
        }
        at = mmio_read8(config + at + PCI_CAP_NEXT) & PCI_CAP_POINTER_MASK;
    }
}

int virtio_pci_reset(volatile UINT8 *common)
{
    UINT32 polls;

    mmio_write8(common + VIRTIO_COMMON_DEVICE_STATUS, 0);
    for (polls = 0; polls < VIRTIO_RESET_POLLS; polls++)
        if (mmio_read8(common + VIRTIO_COMMON_DEVICE_STATUS) == 0)
            return 1;
    return 0;
}
