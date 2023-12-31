/*
 * kernel/src/arch/aarch64/sys/gic.h
 * © suhas pai
 */

#pragma once

#include "dev/dtb/node.h"
#include "dev/dtb/tree.h"

#include "lib/adt/array.h"
#include "mm/mmio.h"

#define GIC_SGI_INTERRUPT_START 0
#define GIC_SGI_INTERRUPT_LAST 15

#define GIC_SGI_IRQ_RANGE \
    RANGE_INIT(GIC_SGI_INTERRUPT_START, GIC_SGI_INTERRUPT_LAST)

#define GIC_PPI_INTERRUPT_START 16
#define GIC_PPI_INTERRUPT_LAST 31

#define GIC_PPI_IRQ_RANGE \
    RANGE_INIT(GIC_PPI_INTERRUPT_START, GIC_PPI_INTERRUPT_LAST)

#define GIC_SPI_INTERRUPT_START 32
#define GIC_SPI_INTERRUPT_LAST 1020

enum gic_version {
    GICv1 = 1,
    GICv2,
    GICv3,
    GICv4,
    GIC_VERSION_BACK = GICv4
};

struct gic_msi_frame {
    struct mmio_region *mmio;
    uint32_t id;
};

struct gic_distributor {
    struct array msi_frame_list;
    enum gic_version version;

    uint8_t impl_cpu_count;
    uint8_t max_impl_lockable_spis;

    uint16_t interrupt_lines_count;
    bool supports_security_extensions : 1;
};

struct gic_cpu_interface;
struct cpu_info;

struct gic_cpu_info {
    volatile struct gic_cpu_interface *interface;
    struct mmio_region *mmio;
};

bool
gic_init_from_dtb(const struct devicetree *tree,
                  const struct devicetree_node *node);

void gic_cpu_init(const struct cpu_info *cpu);
void gicd_init(uint64_t phys_base_address, uint8_t gic_version);

struct gic_msi_frame *gicd_add_msi(uint64_t phys_base_address);
const struct gic_distributor *gic_get_dist();

void gicd_mask_irq(uint16_t irq);
void gicd_unmask_irq(uint16_t irq);

void gicd_set_irq_affinity(uint16_t irq, uint8_t iface);
void gicd_set_irq_trigger_mode(uint16_t irq, enum irq_trigger_mpde mode);
void gicd_set_irq_priority(uint16_t irq, uint8_t priority);

typedef uint16_t irq_number_t;

irq_number_t gic_cpu_get_irq_number(const struct cpu_info *cpu);
uint32_t gic_cpu_get_irq_priority(const struct cpu_info *cpu);

void gic_cpu_eoi(const struct cpu_info *cpu, irq_number_t number);