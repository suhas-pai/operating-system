/*
 * kernel/src/dev/pci/entity.h
 * © suhas pai
 */

#pragma once

#include "dev/pci/bus.h"
#include "lib/adt/bitmap.h"

#if defined(__x86_64__)
    #include "cpu/info.h"
    #include "cpu/isr.h"
#endif /* defined(__x86_64__) */

#include "lib/list.h"
#include "bar.h"

#define PCI_ENTITY_MAX_BAR_COUNT 6

enum pci_entity_msi_support {
    PCI_ENTITY_MSI_SUPPORT_NONE,
    PCI_ENTITY_MSI_SUPPORT_MSI,
    PCI_ENTITY_MSI_SUPPORT_MSIX,
};

struct pci_entity_info {
    struct list list_in_entities;
    struct list list_in_domain;

    const struct pci_bus *bus;
    struct pci_location loc;

    uint16_t id;
    uint16_t vendor_id;
    uint16_t command;
    uint16_t status;
    uint8_t revision_id;

    // prog_if = "Programming Interface"
    uint8_t prog_if;
    uint8_t header_kind;

    uint8_t subclass;
    uint8_t class;
    uint8_t irq_pin;

    bool supports_pcie : 1;
    uint8_t max_bar_count : 3;

    enum pci_entity_msi_support msi_support : 2;

    union {
        uint64_t pcie_msi_offset;
        struct {
            uint64_t pcie_msix_offset;
            struct bitmap msix_table;
        };
    };

    // Array of uint8_t
    struct array vendor_cap_list;
    struct pci_entity_bar_info *bar_list;
};

#define PCI_ENTITY_INFO_FMT                                                    \
    "%" PRIx8 ":%" PRIx8 ":%" PRIx8 ":%" PRIx16 ":%" PRIx16 " (%" PRIx8 ":"    \
    "%" PRIx8 ")"

#define PCI_ENTITY_INFO_FMT_ARGS(device)                                       \
    (device)->loc.bus,                                                         \
    (device)->loc.slot,                                                        \
    (device)->loc.function,                                                    \
    (device)->vendor_id,                                                       \
    (device)->id,                                                              \
    (device)->class,                                                           \
    (device)->subclass

#if defined(__x86_64__)
    bool
    pci_entity_bind_msi_to_vector(struct pci_entity_info *entity,
                                  const struct cpu_info *cpu,
                                  isr_vector_t vector,
                                  bool masked);
#endif

enum pci_entity_privilege {
    __PCI_ENTITY_PRIVL_PIO_ACCESS = 1ull << 0,
    __PCI_ENTITY_PRIVL_MEM_ACCESS = 1ull << 1,
    __PCI_ENTITY_PRIVL_BUS_MASTER = 1ull << 2,
    __PCI_ENTITY_PRIVL_INTERRUPTS = 1ull << 10
};

void pci_entity_enable_privl(struct pci_entity_info *device, uint8_t privl);
