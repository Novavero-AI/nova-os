/* The virtio PCI transport's modern interface (VIRTIO 1.4 4.1) as the
   platform driver and virtio drivers both reach it: the virtio structure
   capabilities in a function's configuration space (4.1.4), the common
   configuration structure (4.1.4.3), the device status it holds (2.1),
   and the device reset (2.4). */

#ifndef NOVA_VIRTIO_PCI_H
#define NOVA_VIRTIO_PCI_H

#include "native_abi.h"

/* The virtio structure capability (4.1.4): cfg_type, bar, offset and
   length; the notification capability adds notify_off_multiplier
   (4.1.4.4). */
#define VIRTIO_CAP_CFG_TYPE 3
#define VIRTIO_CAP_BAR 4
#define VIRTIO_CAP_OFFSET 8
#define VIRTIO_CAP_LENGTH 12
#define VIRTIO_CAP_NOTIFY_MULTIPLIER 16
#define VIRTIO_CAP_COMMON 1
#define VIRTIO_CAP_NOTIFY 2
#define VIRTIO_CAP_DEVICE 4
#define VIRTIO_CAP_TYPES 5

/* struct virtio_pci_common_cfg (4.1.4.3). */
#define VIRTIO_COMMON_DEVICE_FEATURE_SELECT 0
#define VIRTIO_COMMON_DEVICE_FEATURE 4
#define VIRTIO_COMMON_DRIVER_FEATURE_SELECT 8
#define VIRTIO_COMMON_DRIVER_FEATURE 12
#define VIRTIO_COMMON_CONFIG_MSIX_VECTOR 16
#define VIRTIO_COMMON_DEVICE_STATUS 20
#define VIRTIO_COMMON_CONFIG_GENERATION 21
#define VIRTIO_COMMON_QUEUE_SELECT 22
#define VIRTIO_COMMON_QUEUE_SIZE 24
#define VIRTIO_COMMON_QUEUE_MSIX_VECTOR 26
#define VIRTIO_COMMON_QUEUE_ENABLE 28
#define VIRTIO_COMMON_QUEUE_NOTIFY_OFF 30
#define VIRTIO_COMMON_QUEUE_DESC 32
#define VIRTIO_COMMON_QUEUE_DRIVER 40
#define VIRTIO_COMMON_QUEUE_DEVICE 48
#define VIRTIO_COMMON_SIZE 56

/* Device status bits (2.1). */
#define VIRTIO_STATUS_ACKNOWLEDGE 1
#define VIRTIO_STATUS_DRIVER 2
#define VIRTIO_STATUS_DRIVER_OK 4
#define VIRTIO_STATUS_FEATURES_OK 8
#define VIRTIO_STATUS_FAILED 128

/* The offset in the configuration space at config of the first virtio
   structure capability of each type, in structures[type], and of the
   MSI-X capability, in msix; 0 where there is none, and everywhere for a
   function without a capability list. The list is walked at most
   PCI_CAPS_MAX steps, so a looping list ends. */
void virtio_pci_capabilities(const volatile UINT8 *config,
                             UINT32 *structures, UINT32 *msix);

/* Reset the device whose common configuration structure is at common:
   write 0 to device_status, then read it until it gives 0, which the
   device presents once the reset is done (4.1.4.3.1, 2.4.2); from then
   it sends no notifications and leaves its queues alone (2.4.1). Returns
   1 once done, or 0 if VIRTIO_RESET_POLLS reads never gave 0. */
int virtio_pci_reset(volatile UINT8 *common);

/* The most reads virtio_pci_reset makes of device_status. */
#define VIRTIO_RESET_POLLS 1000000

#endif
