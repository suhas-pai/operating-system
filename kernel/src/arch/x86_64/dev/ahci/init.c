/*
 * kernel/src/dev/ahci/init.c
 * © suhas pai
 */

#include "dev/driver.h"
#include "dev/printk.h"

#include "lib/util.h"

#include "mm/kmalloc.h"
#include "mm/mmio.h"
#include "mm/zone.h"

#include "sys/mmio.h"
#include "structs.h"

__optimize(3) static void
ahci_hba_port_start_running(volatile struct ahci_spec_hba_port *const port) {
    const uint32_t flags =
        __AHCI_HBA_PORT_CMDSTATUS_FIS_RECEIVE_ENABLE |
        __AHCI_HBA_PORT_CMDSTATUS_START;

    mmio_write(&port->command_and_status,
               mmio_read(&port->command_and_status) | flags);
}

#define MAX_ATTEMPTS 10

__optimize(3) static bool
ahci_hba_port_stop_running(volatile struct ahci_spec_hba_port *const port) {
    const uint32_t flags =
        __AHCI_HBA_PORT_CMDSTATUS_FIS_RECEIVE_ENABLE |
        __AHCI_HBA_PORT_CMDSTATUS_START;

    mmio_write(&port->command_and_status,
               mmio_read(&port->command_and_status) & ~flags);

    for (uint8_t i = 0; i != MAX_ATTEMPTS; i++) {
        if ((mmio_read(&port->command_and_status) & flags) == 0) {
            return true;
        }
    }

    return false;
}

#define AHCI_HBA_CMD_TABLE_PAGE_ORDER 1

_Static_assert(
    ((uint64_t)sizeof(struct ahci_spec_hba_cmd_table) *
     // Each port (upto 32 exist) has a separate command-table
     sizeof_bits_field(struct ahci_spec_hba_registers, port_implemented))
        <= (PAGE_SIZE << AHCI_HBA_CMD_TABLE_PAGE_ORDER),
    "AHCI_HBA_CMD_TABLE_PAGE_ORDER is too low to fit all "
    "struct ahci_port_command_header entries");

struct ahci_device {
    struct pci_device_info *device;
    volatile struct ahci_spec_hba_registers *regs;

    struct page **cmdlist_page_for_ports_list;

    // Each page is allocated with an order of AHCI_HBA_CMD_TABLE_PAGE_ORDER.
    struct page **cmdtable_pages_for_ports_list;
    struct mmio_region **regions_for_ports_list;

    bool supports_64bit_dma : 1;
};

__optimize(3) static void
ahci_spec_hba_pio_readit(volatile struct ahci_spec_hba_port *const port,
                        const uint8_t index,
                        struct ahci_device *const device)
{
    if (!ahci_hba_port_stop_running(port)) {
        printk(LOGLEVEL_WARN,
               "ahci: failed to stop port at index %" PRIu8 " before init\n",
               index);
        return;
    }

    struct page *cmd_list_page = NULL;
    struct page *cmd_table_pages = NULL;

    if (device->supports_64bit_dma) {
        cmd_list_page = alloc_page(PAGE_STATE_USED, __ALLOC_ZERO);
        cmd_table_pages =
            alloc_pages(PAGE_STATE_USED,
                        __ALLOC_ZERO,
                        AHCI_HBA_CMD_TABLE_PAGE_ORDER);
    } else {
        cmd_list_page =
            alloc_pages_from_zone(page_zone_low4g(),
                                  PAGE_STATE_USED,
                                  __ALLOC_ZERO,
                                  /*order=*/0,
                                  /*allow_fallback=*/true);
        cmd_table_pages =
            alloc_pages_from_zone(page_zone_low4g(),
                                  PAGE_STATE_USED,
                                  __ALLOC_ZERO,
                                  AHCI_HBA_CMD_TABLE_PAGE_ORDER,
                                  /*allow_fallback=*/true);
    }

    if (cmd_list_page == NULL) {
        if (cmd_table_pages != NULL) {
            free_pages(cmd_table_pages, AHCI_HBA_CMD_TABLE_PAGE_ORDER);
        }

        printk(LOGLEVEL_WARN, "ahci: failed to allocate page for cmd-table\n");
        return;
    }

    if (cmd_table_pages == NULL) {
        free_page(cmd_list_page);
        printk(LOGLEVEL_WARN, "ahci: failed to allocate pages for hba port\n");

        return;
    }

    const struct range phys_range =
        RANGE_INIT(page_to_phys(cmd_table_pages),
                   PAGE_SIZE << AHCI_HBA_CMD_TABLE_PAGE_ORDER);

    device->cmdlist_page_for_ports_list[index] = cmd_list_page;
    device->cmdtable_pages_for_ports_list[index] = cmd_table_pages;
    device->regions_for_ports_list[index] =
        vmap_mmio(phys_range, PROT_READ | PROT_WRITE, /*flags=*/0);

    if (device->regions_for_ports_list[index] == NULL) {
        free_page(cmd_list_page);
        free_pages(cmd_table_pages, AHCI_HBA_CMD_TABLE_PAGE_ORDER);

        return;
    }

    const uint64_t cmd_list_phys = page_to_phys(cmd_list_page);

    mmio_write(&port->cmd_list_base_phys_lower32, cmd_list_phys);
    mmio_write(&port->cmd_list_base_phys_upper32, cmd_list_phys >> 32);

    printk(LOGLEVEL_INFO,
           "ahci: port at index %" PRIu8 " has a cmd-list base at %p\n",
           index,
           (void *)phys_range.front);

    volatile struct ahci_spec_port_cmd_header *entry =
        phys_to_virt(phys_range.front);
    const volatile struct ahci_spec_port_cmd_header *const end =
        entry +
        sizeof_bits_field(struct ahci_spec_hba_registers, port_implemented);

    for (uint64_t phys = phys_range.front; entry != end; entry++) {
        mmio_write(&entry->prdt_length, AHCI_HBA_MAX_PRDT_ENTRIES);
        mmio_write(&entry->prd_byte_count, 0);
        mmio_write(&entry->cmd_table_base_lower32, phys);
        mmio_write(&entry->cmd_table_base_upper32, phys >> 32);

        phys += sizeof(struct ahci_spec_hba_cmd_table);
    }

    mmio_write(&port->interrupt_enable,
               __AHCI_HBA_IE_DEV_TO_HOST_FIS_INT_ENABLE |
               __AHCI_HBA_IE_PIO_SETUP_FIS_INT_ENABLE |
               __AHCI_HBA_IE_DMA_SETUP_FIS_INT_ENABLE |
               __AHCI_HBA_IE_SET_DEV_BITS_FIS_INT_ENABLE |
               __AHCI_HBA_IE_UNKNOWN_FIS_INT_ENABLE |
               __AHCI_HBA_IE_DESC_PROCESSED_INT_ENABLE |
               __AHCI_HBA_IE_PORT_CHANGE_INT_ENABLE |
               __AHCI_HBA_IE_DEV_MECH_PRESENCE_INT_ENABLE |
               __AHCI_HBA_IE_PHYRDY_CHANGE_STATUS |
               __AHCI_HBA_IE_INCORRECT_PORT_MULT_STATUS |
               __AHCI_HBA_IE_OVERFLOW_STATUS |
               __AHCI_HBA_IE_INTERFACE_NOT_FATAL_ERR_STATUS |
               __AHCI_HBA_IE_INTERFACE_FATAL_ERR_STATUS |
               __AHCI_HBA_IE_HOST_BUS_DATA_ERR_STATUS |
               __AHCI_HBA_IE_HOST_BUS_FATAL_ERR_STATUS |
               __AHCI_HBA_IE_TASK_FILE_ERR_STATUS |
               __AHCI_HBA_IE_COLD_PORT_DETECT_STATUS);

    ahci_hba_port_start_running(port);
}

__optimize(3) static
bool ahci_hba_probe_port(volatile struct ahci_spec_hba_port *const port) {
    const uint32_t sata_status = mmio_read(&port->sata_status);

    const enum ahci_hba_port_ipm ipm = (sata_status >> 8) & 0x0F;
    const enum ahci_hba_port_det det = sata_status & 0x0F;

    return (det == AHCI_HBA_PORT_DET_PRESENT &&
            ipm == AHCI_HBA_PORT_IPM_ACTIVE);
}

static void init_from_pci(struct pci_device_info *const pci_device) {
    if (!index_in_bounds(AHCI_HBA_REGS_BAR_INDEX, pci_device->max_bar_count)) {
        printk(LOGLEVEL_WARN,
               "ahci: pci-device has fewer than %" PRIu32 " bars\n",
               AHCI_HBA_REGS_BAR_INDEX);
        return;
    }

    struct pci_device_bar_info *const bar =
        &pci_device->bar_list[AHCI_HBA_REGS_BAR_INDEX];

    if (!bar->is_present) {
        printk(LOGLEVEL_WARN,
               "ahci: pci-device doesn't have the required bar at "
               "index %" PRIu32 "\n",
               AHCI_HBA_REGS_BAR_INDEX);
        return;
    }

    if (!bar->is_mmio) {
        printk(LOGLEVEL_WARN,
               "ahci: pci-device's bar at index %" PRIu32 " isn't an mmio "
               "bar\n",
               AHCI_HBA_REGS_BAR_INDEX);
        return;
    }

    if (!pci_map_bar(bar)) {
        printk(LOGLEVEL_WARN,
               "ahci: failed to map pci bar at index %" PRIu32 "\n",
               AHCI_HBA_REGS_BAR_INDEX);
        return;
    }

    volatile struct ahci_spec_hba_registers *const regs =
        (volatile struct ahci_spec_hba_registers *)bar->mmio->base;

    const uint32_t version = mmio_read(&regs->version);
    printk(LOGLEVEL_INFO,
           "ahci: version is %" PRIu32 ".%" PRIu32 "\n",
           version >> 16,
           (uint16_t)version);

    const uint32_t ports_impled = mmio_read(&regs->port_implemented);
    const uint8_t ports_impled_count =
        count_all_one_bits(ports_impled,
                           /*start_index=*/0,
                           /*end_index=*/sizeof_bits(ports_impled));

    if (ports_impled_count == 0) {
        printk(LOGLEVEL_WARN, "ahci: no ports are implemented\n");
        return;
    }

    printk(LOGLEVEL_INFO,
           "ahci: has %" PRIu32 " ports implemented\n",
           ports_impled_count);

    const uint64_t host_cap = mmio_read(&regs->host_capabilities);
    struct ahci_device device = {
        .device = pci_device,
        .regs = regs,
        .cmdlist_page_for_ports_list =
            kmalloc(sizeof(struct page *) * ports_impled),
        .cmdtable_pages_for_ports_list =
            kmalloc(sizeof(struct page *) * ports_impled),
        .regions_for_ports_list =
            kmalloc(sizeof(struct mmio_region *) * ports_impled),
        .supports_64bit_dma = host_cap & __AHCI_HBA_HOST_CAP_64BIT_DMA,
    };

    if (device.cmdlist_page_for_ports_list == NULL) {
        printk(LOGLEVEL_WARN,
               "ahci: failed to allocate memory for list of the cmd-list page "
               "of each port\n");
        return;
    }

    if (device.cmdtable_pages_for_ports_list == NULL) {
        printk(LOGLEVEL_WARN,
               "ahci: failed to allocate memory for list of cmd-table pages of "
               "each port\n");
        return;
    }

    if (device.regions_for_ports_list == NULL) {
        printk(LOGLEVEL_WARN,
               "ahci: failed to allocate memory for list of regions of each "
               "port\n");
        return;
    }

    if (!device.supports_64bit_dma) {
        printk(LOGLEVEL_WARN, "ahci: hba doesn't support 64-bit dma\n");
    }

    uint8_t usable_port_count = 0;
    for (uint8_t index = 0; index != sizeof_bits(ports_impled); index++) {
        if ((ports_impled & (1ull << index)) == 0) {
            continue;
        }

        volatile struct ahci_spec_hba_port *const port =
            (volatile struct ahci_spec_hba_port *)&regs->ports[index];

        if (!ahci_hba_probe_port(port)) {
            printk(LOGLEVEL_WARN,
                   "ahci: implemented port at index %" PRIu8 " is either not "
                   "present or inactive, or both\n",
                   index);
            continue;
        }

        ahci_spec_hba_pio_readit(port, index, &device);
        usable_port_count++;
    }

    if (usable_port_count == 0) {
        kfree(device.cmdtable_pages_for_ports_list);
        printk(LOGLEVEL_WARN,
               "ahci: no implemented ports are both present and active\n");

        return;
    }

    const uint32_t global_host_ctrl =
        mmio_read(&regs->global_host_control) |
        __AHCI_HBA_GLOBAL_HOST_CTRL_INT_ENABLE;

    mmio_write(&regs->global_host_control, global_host_ctrl);
    printk(LOGLEVEL_INFO, "ahci: fully initialized\n");
}

static struct pci_driver pci_driver = {
    .vendor = 0x1af4,
    .class = 0x1,
    .subclass = 0x6,
    .prog_if = 0x1,
    .match =
        __PCI_DRIVER_MATCH_CLASS |
        __PCI_DRIVER_MATCH_SUBCLASS |
        __PCI_DRIVER_MATCH_PROGIF,
    .init = init_from_pci
};

__driver struct driver driver = {
    .dtb = NULL,
    .pci = &pci_driver
};