/*
 * kernel/src/dev/pci/init.c
 * © suhas pai
 */

#if defined(__x86_64__)
    #include "acpi/api.h"
    #include "dev/pci/legacy.h"
#endif /* defined(__x86_64__) */

#include "dev/driver.h"
#include "dev/printk.h"

#include "lib/util.h"

#include "mm/kmalloc.h"
#include "mm/mmio.h"

#include "structs.h"

static struct list g_entity_list = LIST_INIT(g_entity_list);

enum parse_bar_result {
    E_PARSE_BAR_OK,
    E_PARSE_BAR_IGNORE,

    E_PARSE_BAR_UNKNOWN_MEM_KIND,
    E_PARSE_BAR_NO_REG_FOR_UPPER32,

    E_PARSE_BAR_MMIO_MAP_FAIL
};

#define read_bar(dev, index) \
    pci_domain_read_32((dev)->bus->domain, \
                       &(dev)->loc, \
                       offsetof(struct pci_spec_entity_info, bar_list) + \
                         ((index) * sizeof(uint32_t)))

#define write_bar(dev, index, value) \
    pci_domain_write_32((dev)->bus->domain, \
                        &(dev)->loc, \
                        offsetof(struct pci_spec_entity_info, bar_list) + \
                         ((index) * sizeof(uint32_t)), \
                        (value))

enum pci_bar_masks {
    PCI_BAR_32B_ADDR_SIZE_MASK = 0xfffffff0,
    PCI_BAR_PORT_ADDR_SIZE_MASK = 0xfffffffc
};

__optimize(3) static enum parse_bar_result
pci_bar_parse_size(struct pci_entity_info *const dev,
                   struct pci_entity_bar_info *const info,
                   uint64_t base_addr,
                   const uint32_t bar_0_index,
                   const uint32_t bar_0_orig)
{
    /*
     * To get the size, we have to
     *   (1) read the BAR register and save the value.
     *   (2) write ~0 to the BAR register, and read the new value.
     *   (3) Restore the original value
     *
     * Since we operate on two registers for 64-bit, we have to perform this
     * procedure on both registers for 64-bit.
     */

    write_bar(dev, bar_0_index, UINT32_MAX);
    const uint32_t bar_0_size = read_bar(dev, bar_0_index);
    write_bar(dev, bar_0_index, bar_0_orig);

    uint64_t size = 0;
    if (info->is_64_bit) {
        const uint32_t bar_1_index = bar_0_index + 1;
        const uint32_t bar_1_orig = read_bar(dev, bar_1_index);

        write_bar(dev, bar_1_index, UINT32_MAX);
        const uint64_t size_high = read_bar(dev, bar_1_index);
        write_bar(dev, bar_1_index, bar_1_orig);

        size = size_high << 32 | (bar_0_size & PCI_BAR_32B_ADDR_SIZE_MASK);
        size = ~size + 1;
    } else {
        uint32_t size_low =
            info->is_mmio ?
                bar_0_size & PCI_BAR_32B_ADDR_SIZE_MASK :
                bar_0_size & PCI_BAR_PORT_ADDR_SIZE_MASK;

        size_low = ~size_low + 1;
        size = size_low;
    }

    if (size == 0) {
        return E_PARSE_BAR_IGNORE;
    }

    info->port_or_phys_range = RANGE_INIT(base_addr, size);
    info->is_present = true;

    return E_PARSE_BAR_OK;
}

// index_in is incremented assuming index += 1 will be called by the caller
static enum parse_bar_result
pci_parse_bar(struct pci_entity_info *const dev,
              uint8_t *const index_in,
              const bool is_bridge,
              struct pci_entity_bar_info *const bar)
{
    uint64_t base_addr = 0;

    const uint8_t bar_0_index = *index_in;
    const uint32_t bar_0 = read_bar(dev, bar_0_index);

    if (bar_0 & __PCI_DEVBAR_IO) {
        base_addr = bar_0 & PCI_BAR_PORT_ADDR_SIZE_MASK;
        return pci_bar_parse_size(dev, bar, base_addr, bar_0_index, bar_0);
    }

    base_addr = bar_0 & PCI_BAR_32B_ADDR_SIZE_MASK;
    const enum pci_spec_devbar_memspace_kind memory_kind =
        (bar_0 & __PCI_DEVBAR_MEMKIND_MASK) >> __PCI_DEVBAR_MEMKIND_SHIFT;

    enum parse_bar_result result = E_PARSE_BAR_OK;
    if (memory_kind == PCI_DEVBAR_MEMSPACE_32B) {
        result = pci_bar_parse_size(dev, bar, base_addr, bar_0_index, bar_0);
        goto mmio_map;
    }

    if (memory_kind != PCI_DEVBAR_MEMSPACE_64B) {
        printk(LOGLEVEL_WARN,
               "pci: bar has reserved memory kind %d\n", memory_kind);
        return E_PARSE_BAR_UNKNOWN_MEM_KIND;
    }

    // Check if we even have another register left
    const uint8_t last_index =
        is_bridge ?
            (PCI_BAR_COUNT_FOR_BRIDGE - 1) : (PCI_BAR_COUNT_FOR_GENERAL - 1);

    if (bar_0_index == last_index) {
        printk(LOGLEVEL_INFO, "pci: 64-bit bar has no upper32 register\n");
        return E_PARSE_BAR_NO_REG_FOR_UPPER32;
    }

    base_addr |= (uint64_t)read_bar(dev, bar_0_index + 1) << 32;
    bar->is_64_bit = true;

    // Increment once more since we read another register
    *index_in += 1;
    result = pci_bar_parse_size(dev, bar, base_addr, bar_0_index, bar_0);

mmio_map:
    if (result != E_PARSE_BAR_OK) {
        bar->is_64_bit = false;
        return result;
    }

    bar->is_mmio = true;
    bar->is_prefetchable = bar_0 & __PCI_DEVBAR_PREFETCHABLE;

    return E_PARSE_BAR_OK;
}

#undef write_bar
#undef read_bar

static bool
validate_cap_offset(struct array *const prev_cap_offsets,
                    const uint8_t cap_offset)
{
    uint32_t struct_size = offsetof(struct pci_spec_entity_info, data);
    if (index_in_bounds(cap_offset, struct_size)) {
        printk(LOGLEVEL_INFO,
               "\t\tinvalid entity. pci capability offset points to "
               "within structure: 0x%" PRIx8 "\n",
               cap_offset);
        return false;
    }

    const struct range cap_range =
        RANGE_INIT(cap_offset, sizeof(struct pci_spec_capability));

    if (!range_has_index_range(rangeof_field(struct pci_spec_entity_info, data),
                               cap_range))
    {
        printk(LOGLEVEL_INFO,
               "\t\tinvalid entity. pci capability struct is outside entity's "
               "data range: " RANGE_FMT "\n",
               RANGE_FMT_ARGS(cap_range));
        return false;
    }

    array_foreach(prev_cap_offsets, const uint8_t, iter) {
        if (*iter == cap_offset) {
            printk(LOGLEVEL_WARN,
                   "\t\tcapability'e offset_to_next points to previously "
                   "visited capability\n");
            return false;
        }

        const struct range range =
            RANGE_INIT(*iter, sizeof(struct pci_spec_capability));

        if (range_has_loc(range, cap_offset)) {
            printk(LOGLEVEL_WARN,
                   "\t\tcapability'e offset_to_next points to within "
                   "previously visited capability\n");
            return false;
        }
    }

    return true;
}

static void pci_parse_capabilities(struct pci_entity_info *const dev) {
    if ((dev->status & __PCI_DEVSTATUS_CAPABILITIES) == 0) {
        printk(LOGLEVEL_INFO, "\t\thas no capabilities\n");
        return;
    }

    uint8_t cap_offset =
        pci_read(dev, struct pci_spec_entity_info, capabilities_offset);

    if (cap_offset == 0 || cap_offset == (uint8_t)PCI_READ_FAIL) {
        printk(LOGLEVEL_INFO,
               "\t\thas no capabilities, but pci-entity is marked as having "
               "some\n");
        return;
    }

    if (!range_has_index(rangeof_field(struct pci_spec_entity_info, data),
                         cap_offset))
    {
        printk(LOGLEVEL_INFO,
               "\t\tcapabilities point to within entity-info structure\n");
        return;
    }

    // On x86_64, the fadt may provide a flag indicating the MSI is disabled.
#if defined(__x86_64__)
    bool supports_msi = true;
    const struct acpi_fadt *const fadt = get_acpi_info()->fadt;

    if (fadt != NULL) {
        if (fadt->iapc_boot_arch_flags &
                __ACPI_FADT_IAPC_BOOT_MSI_NOT_SUPPORTED)
        {
            supports_msi = false;
        }
    }
#endif

#define pci_read_cap_field(type, field) \
    pci_read_from_base(dev, cap_offset, type, field)

    for (uint64_t i = 0; cap_offset != 0 && cap_offset != 0xff; i++) {
        if (i == 128) {
            printk(LOGLEVEL_INFO,
                   "\t\ttoo many capabilities for "
                   "entity " PCI_ENTITY_INFO_FMT "\n",
                   PCI_ENTITY_INFO_FMT_ARGS(dev));
            return;
        }

        if (!validate_cap_offset(&dev->vendor_cap_list, cap_offset)) {
            continue;
        }

        const uint8_t id = pci_read_cap_field(struct pci_spec_capability, id);
        const char *kind = "unknown";

        switch ((enum pci_spec_cap_id)id) {
            case PCI_SPEC_CAP_ID_NULL:
                kind = "null";
                break;
            case PCI_SPEC_CAP_ID_AGP:
                kind = "advanced-graphics-port";
                break;
            case PCI_SPEC_CAP_ID_VPD:
                kind = "vital-product-data";
                break;
            case PCI_SPEC_CAP_ID_SLOT_ID:
                kind = "slot-identification";
                break;
            case PCI_SPEC_CAP_ID_MSI: {
                kind = "msi";
            #if defined(__x86_64__)
                if (!supports_msi) {
                    break;
                }
            #endif /* defined(__x86_64) */

                if (dev->msi_support == PCI_ENTITY_MSI_SUPPORT_MSIX) {
                    break;
                }

                const struct range msi_range =
                    RANGE_INIT(cap_offset, sizeof(struct pci_spec_cap_msi));

                if (!index_range_in_bounds(msi_range, PCI_SPACE_MAX_OFFSET)) {
                    printk(LOGLEVEL_WARN,
                           "\t\tmsi-cap goes beyond end of pci-domain\n");
                    break;
                }

                dev->msi_support = PCI_ENTITY_MSI_SUPPORT_MSI;
                dev->pcie_msi_offset = cap_offset;

                break;
            }
            case PCI_SPEC_CAP_ID_MSI_X: {
                kind = "msix";
            #if defined(__x86_64__)
                if (!supports_msi) {
                    break;
                }
            #endif /* defined(__x86_64) */

                if (dev->msi_support == PCI_ENTITY_MSI_SUPPORT_MSIX) {
                    printk(LOGLEVEL_WARN,
                           "\t\tfound multiple msix capabilities. ignoring\n");
                    break;
                }

                const struct range msix_range =
                    RANGE_INIT(cap_offset, sizeof(struct pci_spec_cap_msix));

                if (!index_range_in_bounds(msix_range, PCI_SPACE_MAX_OFFSET)) {
                    printk(LOGLEVEL_WARN,
                           "\t\tmsix-cap goes beyond end of pci-domain\n");
                    break;
                }

                dev->pcie_msix_offset = cap_offset;

                const uint16_t msg_control =
                    pci_read_cap_field(struct pci_spec_cap_msix, msg_control);
                const uint16_t bitmap_size =
                    (msg_control & __PCI_CAP_MSIX_TABLE_SIZE_MASK) + 1;

                const struct bitmap bitmap = bitmap_alloc(bitmap_size);
                if (bitmap.gbuffer.begin == NULL) {
                    printk(LOGLEVEL_WARN,
                           "\t\tfailed to allocate msix table "
                           "(size: %" PRIu16 " bytes). disabling msix\n",
                           bitmap_size);
                    break;
                }

                dev->msi_support = PCI_ENTITY_MSI_SUPPORT_MSIX;
                dev->msix_table = bitmap;

                break;
            }
            case PCI_SPEC_CAP_ID_POWER_MANAGEMENT:
                kind = "power-management";
                break;
            case PCI_SPEC_CAP_ID_VENDOR_SPECIFIC:
                if (!array_append(&dev->vendor_cap_list, &cap_offset)) {
                    printk(LOGLEVEL_WARN,
                           "\t\tfailed to append to internal array\n");
                    return;
                }

                kind = "vendor-specific";
                break;
            case PCI_SPEC_CAP_ID_PCI_X:
                kind = "pci-x";
                break;
            case PCI_SPEC_CAP_ID_DEBUG_PORT:
                kind = "debug-port";
                break;
            case PCI_SPEC_CAP_ID_SATA:
                kind = "sata";
                break;
            case PCI_SPEC_CAP_ID_COMPACT_PCI_HOT_SWAP:
                kind = "compact-pci-hot-swap";
                break;
            case PCI_SPEC_CAP_ID_HYPER_TRANSPORT:
                kind = "hyper-transport";
                break;
            case PCI_SPEC_CAP_ID_COMPACT_PCI_CENTRAL_RSRC_CNTRL:
                kind = "compact-pci-central-resource-control";
                break;
            case PCI_SPEC_CAP_ID_PCI_HOTPLUG:
                kind = "pci-hotplug";
                break;
            case PCI_SPEC_CAP_ID_PCI_BRIDGE_SYS_VENDOR_ID:
                kind = "pci-bridge-system-vendor-id";
                break;
            case PCI_SPEC_CAP_ID_AGP_8X:
                kind = "advanced-graphics-port-8x";
                break;
            case PCI_SPEC_CAP_ID_SECURE_DEVICE:
                kind = "secure-device";
                break;
            case PCI_SPEC_CAP_ID_PCI_EXPRESS: {
                kind = "pci-express";
                const struct range pcie_range =
                    RANGE_INIT(cap_offset, sizeof(struct pci_spec_cap_pcie));

                if (!index_range_in_bounds(pcie_range, PCI_SPACE_MAX_OFFSET)) {
                    printk(LOGLEVEL_WARN,
                           "\t\tpcie-cap goes beyond end of pci-domain\n");
                    break;
                }

                dev->supports_pcie = true;
                break;
            }
            case PCI_SPEC_CAP_ID_ADV_FEATURES:
                kind = "advanced-features";
                break;
            case PCI_SPEC_CAP_ID_ENHANCED_ALLOCS:
                kind = "enhanced-allocations";
                break;
            case PCI_SPEC_CAP_ID_FLAT_PORTAL_BRIDGE:
                kind = "flattening-portal-bridge";
                break;
        }

        printk(LOGLEVEL_INFO,
               "\t\tfound capability: %s at offset 0x%" PRIx8 "\n",
               kind,
               cap_offset);

        cap_offset =
            pci_read_cap_field(struct pci_spec_capability, offset_to_next);
    }

#undef pci_read_cap_field
}

__optimize(3)
static void free_inside_entity_info(struct pci_entity_info *const entity) {
    struct pci_entity_bar_info *const bar_list = entity->bar_list;
    const uint8_t bar_count =
        entity->header_kind == PCI_SPEC_ENTITY_HDR_KIND_PCI_BRIDGE  ?
            PCI_BAR_COUNT_FOR_BRIDGE : PCI_BAR_COUNT_FOR_GENERAL;

    for (uint8_t i = 0; i != bar_count; i++) {
        struct pci_entity_bar_info *const bar = &bar_list[i];
        if (bar->mmio != NULL) {
            vunmap_mmio(bar->mmio);
        }
    }

    if (entity->msi_support == PCI_ENTITY_MSI_SUPPORT_MSIX) {
        bitmap_destroy(&entity->msix_table);
    }

    array_destroy(&entity->vendor_cap_list);
}

__optimize(3) static inline
const char *pci_entity_get_vendor_name(struct pci_entity_info *const entity) {
    carr_foreach(pci_vendor_info_list, iter) {
        if (entity->vendor_id == iter->id) {
            return iter->name;
        }
    }

    return "unknown";
}

void
pci_parse_bus(const struct pci_bus *bus,
              struct pci_location loc,
              uint8_t bus_id);

static void
parse_function(const struct pci_bus *const bus,
               const struct pci_location *const loc,
               const uint16_t vendor_id)
{
    struct pci_entity_info info = {
        .bus = bus,
        .loc = *loc
    };

    const uint8_t header_kind =
        pci_read(&info, struct pci_spec_entity_info_base, header_kind);

    const uint8_t hdrkind = header_kind & (uint8_t)~__PCI_ENTITY_HDR_MULTFUNC;
    const uint8_t irq_pin =
        hdrkind == PCI_SPEC_ENTITY_HDR_KIND_GENERAL ?
            pci_read(&info, struct pci_spec_entity_info, interrupt_pin) : 0;

    info.id = pci_read(&info, struct pci_spec_entity_info_base, device_id);
    info.vendor_id = vendor_id;
    info.command = pci_read(&info, struct pci_spec_entity_info_base, command);
    info.status = pci_read(&info, struct pci_spec_entity_info_base, status);
    info.revision_id =
        pci_read(&info, struct pci_spec_entity_info_base, revision_id);

    info.prog_if = pci_read(&info, struct pci_spec_entity_info_base, prog_if);
    info.header_kind = hdrkind;

    info.class = pci_read(&info, struct pci_spec_entity_info_base, class_code);
    info.subclass = pci_read(&info, struct pci_spec_entity_info_base, subclass);
    info.irq_pin = irq_pin;
    info.vendor_cap_list = ARRAY_INIT(sizeof(uint8_t));

    printk(LOGLEVEL_INFO,
           "\tentity: " PCI_ENTITY_INFO_FMT " from %s\n",
           PCI_ENTITY_INFO_FMT_ARGS(&info),
           pci_entity_get_vendor_name(&info));

    const bool class_is_pci_bridge =
        info.class == PCI_ENTITY_CLASS_BRIDGE_DEVICE &&
        (info.subclass == PCI_ENTITY_SUBCLASS_PCI_BRIDGE ||
         info.subclass == PCI_ENTITY_SUBCLASS_PCI_BRIDGE_2);

    const bool hdrkind_is_pci_bridge =
        hdrkind == PCI_SPEC_ENTITY_HDR_KIND_PCI_BRIDGE;

    if (hdrkind_is_pci_bridge != class_is_pci_bridge) {
        free_inside_entity_info(&info);
        printk(LOGLEVEL_WARN,
               "pci: invalid entity, header-type and class/subclass "
               "mismatch\n");

        return;
    }

    // Disable I/O Space and Memory domain flags to parse bars. Re-enable after.
    const uint16_t new_command =
        (info.command &
            (uint16_t)~(__PCI_DEVCMDREG_IOSPACE | __PCI_DEVCMDREG_MEMSPACE)) |
        __PCI_DEVCMDREG_INT_DISABLE;

    pci_write(&info, struct pci_spec_entity_info_base, command, new_command);
    switch (hdrkind) {
        case PCI_SPEC_ENTITY_HDR_KIND_GENERAL:
            info.max_bar_count = PCI_BAR_COUNT_FOR_GENERAL;
            info.bar_list =
                kmalloc(sizeof(struct pci_entity_bar_info) *
                        info.max_bar_count);

            if (info.bar_list == NULL) {
                free_inside_entity_info(&info);
                printk(LOGLEVEL_WARN,
                       "pci: failed to allocate memory for bar list\n");

                return;
            }

            pci_parse_capabilities(&info);
            for (uint8_t index = 0; index != info.max_bar_count; index++) {
                struct pci_entity_bar_info *const bar = &info.bar_list[index];
                const uint8_t bar_index = index;

                const enum parse_bar_result result =
                    pci_parse_bar(&info, &index, /*is_bridge=*/false, bar);

                if (result == E_PARSE_BAR_IGNORE) {
                    printk(LOGLEVEL_INFO,
                           "\t\tgeneral bar %" PRIu8 ": ignoring\n",
                           index);

                    bar->is_present = false;
                    continue;
                }

                if (result != E_PARSE_BAR_OK) {
                    printk(LOGLEVEL_WARN,
                           "pci: failed to parse bar %" PRIu8 " for entity\n",
                           index);

                    bar->is_present = false;
                    break;
                }

                printk(LOGLEVEL_INFO,
                       "\t\tgeneral bar %" PRIu8 " %s: " RANGE_FMT ", %s, %s"
                       "size: %" PRIu64 "\n",
                       bar_index,
                       bar->is_mmio ? "mmio" : "ports",
                       RANGE_FMT_ARGS(bar->port_or_phys_range),
                       bar->is_prefetchable ?
                        "prefetchable" : "not-prefetchable",
                       bar->is_mmio ?
                        bar->is_64_bit ? "64-bit, " : "32-bit, " : "",
                       bar->port_or_phys_range.size);
            }

            break;
        case PCI_SPEC_ENTITY_HDR_KIND_PCI_BRIDGE:
            info.max_bar_count = PCI_BAR_COUNT_FOR_BRIDGE;
            info.bar_list =
                kmalloc(sizeof(struct pci_entity_bar_info) *
                        info.max_bar_count);

            if (info.bar_list == NULL) {
                free_inside_entity_info(&info);
                printk(LOGLEVEL_WARN,
                       "pci: failed to allocate memory for bar list\n");

                return;
            }

            pci_parse_capabilities(&info);
            for (uint8_t index = 0; index != info.max_bar_count; index++) {
                struct pci_entity_bar_info *const bar = &info.bar_list[index];

                const uint8_t bar_index = index;
                const enum parse_bar_result result =
                    pci_parse_bar(&info, &index, /*is_bridge=*/true, bar);

                if (result == E_PARSE_BAR_IGNORE) {
                    printk(LOGLEVEL_INFO,
                           "\t\tbridge bar %" PRIu8 ": ignoring\n",
                           index);
                    continue;
                }

                if (result != E_PARSE_BAR_OK) {
                    printk(LOGLEVEL_INFO,
                           "pci: failed to parse bar %" PRIu8 " for "
                           "entity, " PCI_ENTITY_INFO_FMT "\n",
                           index,
                           PCI_ENTITY_INFO_FMT_ARGS(&info));
                    break;
                }

                printk(LOGLEVEL_INFO,
                       "\t\tbridge bar %" PRIu8 " %s: " RANGE_FMT ", %s, %s"
                       "size: %" PRIu64 "\n",
                       bar_index,
                       bar->is_mmio ? "mmio" : "ports",
                       RANGE_FMT_ARGS(bar->port_or_phys_range),
                       bar->is_prefetchable ?
                        "prefetchable" : "not-prefetchable",
                       bar->is_mmio ?
                        bar->is_64_bit ? "64-bit, " : "32-bit, " : "",
                       bar->port_or_phys_range.size);
            }

            const uint8_t secondary_bus_number =
                pci_read(&info,
                         struct pci_spec_pci_to_pci_bridge_entity_info,
                         secondary_bus_number);

            pci_parse_bus(bus, info.loc, secondary_bus_number);
            break;
        case PCI_SPEC_ENTITY_HDR_KIND_CARDBUS_BRIDGE:
            printk(LOGLEVEL_INFO,
                   "pcie: cardbus bridge not supported. ignoring");
            break;
    }

    pci_write(&info, struct pci_spec_entity_info_base, command, info.command);

    struct pci_entity_info *const info_out = kmalloc(sizeof(*info_out));
    if (info_out == NULL) {
        free_inside_entity_info(&info);
        printk(LOGLEVEL_WARN,
               "pci: failed to allocate pci-entity-info struct\n");

        return;
    }

    *info_out = info;
    list_add(&g_entity_list, &info_out->list_in_entities);
}

void
pci_parse_bus(const struct pci_bus *const bus,
              struct pci_location loc,
              const uint8_t bus_id)
{
    loc.bus += bus_id;
    for (uint8_t slot = 0; slot != PCI_MAX_SLOT_COUNT; slot++) {
        loc.slot = slot;
        for (uint8_t func = 0; func != PCI_MAX_FUNCTION_COUNT; func++) {
            loc.function = func;
            const uint16_t vendor_id =
                pci_domain_read_16(
                    bus->domain,
                    &loc,
                    offsetof(struct pci_spec_entity_info_base, vendor_id));

            if (vendor_id == (uint16_t)PCI_READ_FAIL) {
                continue;
            }

            parse_function(bus, &loc, vendor_id);
        }
    }
}

void pci_find_entities(struct pci_bus *const bus) {
    struct pci_location loc = {
        .segment = bus->segment,
        .bus = bus->bus_id,
        .slot = 0,
        .function = 0,
    };

    const uint8_t header_kind =
        pci_domain_read_8(bus->domain,
                          &loc,
                          offsetof(struct pci_spec_entity_info_base,
                                   header_kind));

    if (header_kind == (uint8_t)PCI_READ_FAIL) {
        return;
    }

    const uint8_t host_count =
        header_kind & __PCI_ENTITY_HDR_MULTFUNC ? PCI_MAX_FUNCTION_COUNT : 1;

    for (uint16_t i = 0; i != host_count; i++) {
        pci_parse_bus(bus, loc, /*bus=*/i);
    }
}

void pci_init_drivers() {
    driver_foreach(driver) {
        if (driver->pci == NULL) {
            continue;
        }

        const struct pci_driver *const pci_driver = driver->pci;
        struct pci_entity_info *entity = NULL;

        list_foreach(entity, &g_entity_list, list_in_entities) {
            if (pci_driver->match == PCI_DRIVER_MATCH_VENDOR) {
                if (entity->vendor_id == pci_driver->vendor) {
                    pci_driver->init(entity);
                }

                continue;
            }

            if (pci_driver->match == PCI_DRIVER_MATCH_VENDOR_DEVICE) {
                if (entity->vendor_id != pci_driver->vendor) {
                    continue;
                }

                bool found = false;
                for (uint8_t i = 0; i != pci_driver->device_count; i++) {
                    if (pci_driver->devices[i] == entity->id) {
                        found = true;
                        break;
                    }
                }

                if (found) {
                    pci_driver->init(entity);
                }

                continue;
            }

            if (pci_driver->match & __PCI_DRIVER_MATCH_CLASS) {
                if (entity->class != pci_driver->class) {
                    continue;
                }
            }

            if (pci_driver->match & __PCI_DRIVER_MATCH_SUBCLASS) {
                if (entity->subclass != pci_driver->subclass) {
                    continue;
                }
            }

            if (pci_driver->match & __PCI_DRIVER_MATCH_PROGIF) {
                if (entity->prog_if != pci_driver->prog_if) {
                    continue;
                }
            }

            pci_driver->init(entity);
        }
    }
}

__optimize(3) void pci_init() {
    int flag = 0;
    const struct array *const bus_list = pci_get_root_bus_list_locked(&flag);

#if defined(__x86_64__)
    if (array_empty(*bus_list)) {
        pci_release_root_bus_list_lock(flag);
        printk(LOGLEVEL_INFO,
               "pci: searching for entities in root bus (legacy domain)\n");

        const struct pci_location loc = {
            .segment = 0,
            .bus = 0,
            .slot = 0,
            .function = 0
        };

        const uint32_t first_dword =
            pci_legacy_domain_read(
                &loc,
                offsetof(struct pci_spec_entity_info_base, vendor_id),
                sizeof(uint16_t));

        if (first_dword == PCI_READ_FAIL) {
            printk(LOGLEVEL_WARN,
                   "pci: failed to find pci bus in legacy domain. aborting "
                   "init\n");
            return;
        }

        struct pci_domain *const legacy_domain =
            kmalloc(sizeof(*legacy_domain));

        assert_msg(legacy_domain != NULL,
                   "pci: failed to allocate pci-legacy root domain");

        legacy_domain->kind = PCI_DOMAIN_LEGACY;
        struct pci_bus *const root_bus =
            pci_bus_create(legacy_domain, /*bus_id=*/0, /*segment=*/0);

        assert_msg(root_bus != NULL,
                   "pci: failed to allocate pci-legacy root bus");

        pci_find_entities(root_bus);
        pci_add_root_bus(root_bus);
    } else {
        printk(LOGLEVEL_INFO, "pci: no root-bus found. Aborting init\n");
    }
#else
    if (!array_empty(*bus_list)) {
        printk(LOGLEVEL_INFO,
                "pci: searching for entities in every pci-bus\n");

        array_foreach(bus_list, struct pci_bus *const, iter) {
            pci_find_entities(*iter);
        }
    } else {
        printk(LOGLEVEL_INFO, "pci: no root-bus found. Aborting init\n");
    }
#endif /* defined(__x86_64__) */

    pci_release_root_bus_list_lock(flag);
    pci_init_drivers();
}
