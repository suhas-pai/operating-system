/*
 * kernel/src/dev/virtio/device.c
 * © suhas pai
 */

#include "dev/printk.h"
#include "device.h"

void
virtio_device_queue_select_and_notify(struct virtio_device *const device,
                                      const uint16_t queue_index)
{
    (void)device;
    (void)queue_index;
}

bool
virtio_device_shmem_region_map(struct virtio_device_shmem_region *const region)
{
    if (region->mmio != NULL) {
        return true;
    }

    struct mmio_region *const mmio =
        vmap_mmio(region->phys_range, PROT_READ | PROT_WRITE, /*flags=*/0);

    if (mmio == NULL) {
        printk(LOGLEVEL_WARN,
               "virtio-device: failed to map shared-memory region\n");
        return false;
    }

    region->mmio = mmio;
    return true;
}

void
virtio_device_shmem_region_unmap(
    struct virtio_device_shmem_region *const region)
{
    if (region->mmio == NULL) {
        return;
    }

    vunmap_mmio(region->mmio);
    region->mmio = NULL;

    return;
}