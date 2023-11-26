/*
 * kernel/src/dev/dtb/parse.c
 * © suhas pai
 */

#include "dev/dtb/node.h"
#include "lib/adt/string.h"

#include "dev/printk.h"
#include "fdt/libfdt.h"
#include "mm/kmalloc.h"

#include "parse.h"

bool
parse_array_prop(const struct fdt_property *const fdt_prop,
                 const int prop_length,
                 const fdt32_t **const data_out,
                 uint32_t *const length_out)
{
    *data_out = (const fdt32_t *)(uint64_t)fdt_prop->data;
    *length_out = (uint32_t)prop_length / sizeof(fdt32_t);

    return true;
}

static bool
parse_reg_pairs(const void *const dtb,
                const struct fdt_property *const fdt_prop,
                const int prop_length,
                const int parent_off,
                struct array *const array)
{
    const int addr_cells = fdt_address_cells(dtb, parent_off);
    const int size_cells = fdt_size_cells(dtb, parent_off);

    if (addr_cells < 0 || size_cells < 0) {
        return false;
    }

    const fdt32_t *reg = NULL;
    uint32_t reg_length = 0;

    if (!parse_array_prop(fdt_prop, prop_length, &reg, &reg_length)) {
        return false;
    }

    if (reg_length == 0) {
        return true;
    }

    const uint32_t entry_size = (uint32_t)(addr_cells + size_cells);
    if (entry_size == 0) {
        return false;
    }

    if ((reg_length % entry_size) != 0) {
        return false;
    }

    const uint32_t entry_count = reg_length / entry_size;
    array_reserve(array, entry_count);

    const uint32_t addr_shift = sizeof_bits(uint64_t) / (uint32_t)addr_cells;
    const uint32_t size_shift = sizeof_bits(uint64_t) / (uint32_t)size_cells;

    for (uint32_t i = 0; i != entry_count; i++) {
        struct devicetree_prop_reg_info info;
        if (addr_shift != sizeof_bits(uint64_t)) {
            for (int j = 0; j != addr_cells; j++) {
                info.address = info.address << addr_shift | fdt32_to_cpu(*reg);
                reg++;
            }
        } else {
            info.address = fdt32_to_cpu(*reg);
            reg++;
        }

        if (size_shift != sizeof_bits(uint64_t)) {
            for (int j = 0; j != size_cells; j++) {
                info.size = info.size << size_shift | fdt32_to_cpu(*reg);
                reg++;
            }
        } else {
            info.size = fdt32_to_cpu(*reg);
            reg++;
        }

        if (!array_append(array, &info)) {
            return false;
        }
    }

    return true;
}

static bool
parse_ranges_prop(const void *const dtb,
                  const struct fdt_property *const fdt_prop,
                  const int prop_length,
                  const int nodeoff,
                  const int parent_off,
                  struct array *const array)
{
    const int parent_addr_cells = fdt_address_cells(dtb, parent_off);
    const int child_addr_cells = fdt_address_cells(dtb, nodeoff);
    const int size_cells = fdt_size_cells(dtb, nodeoff);

    if (parent_addr_cells < 0 || child_addr_cells < 0 || size_cells < 0) {
        return false;
    }

    const fdt32_t *reg = NULL;
    uint32_t reg_length = 0;

    if (!parse_array_prop(fdt_prop, prop_length, &reg, &reg_length)) {
        return false;
    }

    if (reg_length == 0) {
        return true;
    }

    const uint32_t entry_size =
        (uint32_t)(parent_addr_cells + child_addr_cells + size_cells);

    if (entry_size == 0) {
        return false;
    }

    if ((reg_length % entry_size) != 0) {
        return false;
    }

    const uint32_t entry_count = reg_length / entry_size;
    array_reserve(array, entry_count);

    const uint32_t size_shift = sizeof_bits(uint64_t) / (uint32_t)size_cells;
    const uint32_t child_addr_shift =
        sizeof_bits(uint64_t) / (uint32_t)child_addr_cells;
    const uint32_t parent_addr_shift =
        sizeof_bits(uint64_t) / (uint32_t)parent_addr_cells;

    for (uint32_t i = 0; i != entry_count; i++) {
        struct devicetree_prop_range_info info;
        if (child_addr_shift != sizeof_bits(uint64_t)) {
            for (int j = 0; j != child_addr_cells; j++) {
                info.child_bus_address =
                    info.child_bus_address << child_addr_shift |
                    fdt32_to_cpu(*reg);

                reg++;
            }
        } else {
            info.child_bus_address = fdt32_to_cpu(*reg);
            reg++;
        }

        if (parent_addr_shift != sizeof_bits(uint64_t)) {
            for (int j = 0; j != parent_addr_cells; j++) {
                info.parent_bus_address =
                    info.parent_bus_address << parent_addr_shift |
                    fdt32_to_cpu(*reg);

                reg++;
            }
        } else {
            info.parent_bus_address = fdt32_to_cpu(*reg);
            reg++;
        }

        if (size_shift != sizeof_bits(uint64_t)) {
            for (int j = 0; j != size_cells; j++) {
                info.size = info.size << size_shift | fdt32_to_cpu(*reg);
                reg++;
            }
        } else {
            info.size = fdt32_to_cpu(*reg);
            reg++;
        }

        if (!array_append(array, &info)) {
            return false;
        }
    }

    return true;
}

static bool
parse_model_prop(const char *const model,
                 struct string_view *const manufacturer_out,
                 struct string_view *const model_out)
{
    const char *const comma = strchr(model, ',');
    if (comma == NULL) {
        return false;
    }

    const char *const end = model + strlen(model);

    *manufacturer_out = sv_create_length(model, distance(model, comma));
    *model_out = sv_create_length(comma + 1, distance(comma + 1, end));

    return true;
}

static bool
parse_status_prop(const char *const string,
                  enum devicetree_prop_status_kind *const kind_out)
{
    const struct string_view sv = sv_create_length(string, strlen(string));
    if (sv_equals(sv, SV_STATIC("okay"))) {
        *kind_out = DEVICETREE_PROP_STATUS_OKAY;
        return true;
    }

    if (sv_equals(sv, SV_STATIC("disabled"))) {
        *kind_out = DEVICETREE_PROP_STATUS_DISABLED;
        return true;
    }

    if (sv_equals(sv, SV_STATIC("reserved"))) {
        *kind_out = DEVICETREE_PROP_STATUS_RESERVED;
        return true;
    }

    if (sv_equals(sv, SV_STATIC("fail"))) {
        *kind_out = DEVICETREE_PROP_STATUS_FAIL;
        return true;
    }

    if (sv_equals(sv, SV_STATIC("fail-sss"))) {
        *kind_out = DEVICETREE_PROP_STATUS_FAIL_SSS;
        return true;
    }

    return false;
}

static bool
parse_integer_prop(const struct fdt_property *const fdt_prop,
                   const int prop_length,
                   uint32_t *const int_out)
{
    if (prop_length != sizeof(fdt32_t)) {
        return false;
    }

    *int_out = fdt32_to_cpu(*(uint32_t *)(uint64_t)fdt_prop->data);
    return true;
}

static bool
parse_integer_list_prop(const struct fdt_property *const fdt_prop,
                        const int prop_length,
                        struct array *const array)
{
    const fdt32_t *reg = NULL;
    uint32_t reg_length = 0;

    if (!parse_array_prop(fdt_prop, prop_length, &reg, &reg_length)) {
        return false;
    }

    if (reg_length == 0) {
        return true;
    }

    const fdt32_t *const num_list = (fdt32_t *)(uint64_t)fdt_prop->data;

    *array = ARRAY_INIT(sizeof(uint32_t));
    array_reserve(array, reg_length);

    for (uint32_t i = 0; i != reg_length; i++) {
        const uint32_t num = fdt32_to_cpu(num_list[i]);
        if (!array_append(array, &num)) {
            return false;
        }
    }

    return true;
}

static int
fdt_cells(const void *const fdt, const int nodeoffset, const char *const name) {
    const fdt32_t *c;
    uint32_t val;
    int len;

    c = fdt_getprop(fdt, nodeoffset, name, &len);
    if (!c)
        return len;

    if (len != sizeof(*c))
        return -FDT_ERR_BADNCELLS;

    val = fdt32_to_cpu(*c);
    if (val > FDT_MAX_NCELLS)
        return -FDT_ERR_BADNCELLS;

    return (int)val;
}

static bool
parse_specifier_map_prop(const void *const dtb,
                         const struct devicetree_node *const node,
                         const struct fdt_property *const fdt_prop,
                         const int prop_length,
                         const int parent_off,
                         struct array *const array)
{
    struct string cells_key = STRING_NULL();

    string_reserve(&cells_key, LEN_OF("#-cells") + node->name.length);
    string_append_char(&cells_key, '#', /*amt=*/1);

    string_append_sv(&cells_key, node->name);
    string_append_sv(&cells_key, SV_STATIC("-cells"));

    const int cells = fdt_cells(dtb, parent_off, string_to_cstr(cells_key));
    if (cells < 0) {
        // If we don't have a corresponding cells prop, then we assume this
        // isn't a specifier-map prop.

        return true;
    }

    const fdt32_t *reg = NULL;
    uint32_t reg_length = 0;

    if (!parse_array_prop(fdt_prop, prop_length, &reg, &reg_length)) {
        return false;
    }

    if (reg_length == 0) {
        return true;
    }

    const uint32_t entry_size = (uint32_t)cells;
    if (entry_size == 0) {
        return false;
    }

    if ((reg_length % entry_size) != 0) {
        return false;
    }

    const uint32_t entry_count = reg_length / entry_size;
    array_reserve(array, entry_count);

    const uint32_t shift = sizeof_bits(uint64_t) / entry_size;
    for (uint32_t i = 0; i != entry_count; i++) {
        struct devicetree_prop_specifier_map_entry info;
        if (shift != sizeof_bits(uint64_t)) {
            for (int j = 0; j != cells; j++) {
                info.child_specifier =
                    info.child_specifier << shift | fdt32_to_cpu(*reg);

                reg++;
            }
        } else {
            info.child_specifier = fdt32_to_cpu(*reg);
            reg++;
        }

        info.specifier_parent =
            info.specifier_parent << shift | fdt32_to_cpu(*reg);

        reg++;
        if (shift != sizeof_bits(uint64_t)) {
            for (int j = 0; j != cells; j++) {
                info.parent_specifier =
                    info.parent_specifier << shift | fdt32_to_cpu(*reg);

                reg++;
            }
        } else {
            info.parent_specifier = fdt32_to_cpu(*reg);
            reg++;
        }

        if (!array_append(array, &info)) {
            return false;
        }
    }

    return true;
}

static bool
parse_node_prop(const void *const dtb,
                const int nodeoff,
                const int prop_off,
                const int parent_nodeoff,
                struct devicetree *const tree,
                struct devicetree_node *const node,
                struct array *const int_map_list,
                struct devicetree_prop_addr_size_cells *const addr_size_cells)
{
    int prop_len = 0;
    int name_len = 0;

    const struct fdt_property *const fdt_prop =
        fdt_get_property_by_offset(dtb, prop_off, &prop_len);
    const char *const prop_string =
        fdt_get_string(dtb,
                       (int)fdt32_to_cpu((fdt32_t)fdt_prop->nameoff),
                       &name_len);

    const struct string_view name =
        sv_create_length(prop_string, (uint64_t)name_len);

    if (sv_equals(name, SV_STATIC("reg"))) {
        struct array pair_arr =
            ARRAY_INIT(sizeof(struct devicetree_prop_reg_info));

        if (!parse_reg_pairs(dtb,
                             fdt_prop,
                             prop_len,
                             parent_nodeoff,
                             &pair_arr))
        {
            return false;
        }

        struct devicetree_prop_reg *const reg_prop = kmalloc(sizeof(*reg_prop));
        if (reg_prop == NULL) {
            array_destroy(&pair_arr);
            return false;
        }

        reg_prop->kind = DEVICETREE_PROP_REG;
        reg_prop->pair_list = pair_arr;

        if (!array_append(&node->known_props, &reg_prop)) {
            kfree(reg_prop);
            array_destroy(&pair_arr);

            return false;
        }
    } else if (sv_equals(name, SV_STATIC("ranges"))) {
        struct array range_arr = ARRAY_INIT(sizeof(struct range));
        if (!parse_ranges_prop(dtb,
                               fdt_prop,
                               prop_len,
                               nodeoff,
                               parent_nodeoff,
                               &range_arr))
        {
            return false;
        }

        struct devicetree_prop_ranges *const ranges_prop =
            kmalloc(sizeof(*ranges_prop));

        if (ranges_prop == NULL) {
            array_destroy(&range_arr);
            return false;
        }

        ranges_prop->kind = DEVICETREE_PROP_RANGES;
        ranges_prop->range_list = range_arr;

        if (!array_append(&node->known_props, &ranges_prop)) {
            kfree(ranges_prop);
            return false;
        }
    } else if (sv_equals(name, SV_STATIC("model"))) {
        struct string_view manufacturer_sv = SV_EMPTY();
        struct string_view model_sv = SV_EMPTY();

        if (!parse_model_prop(fdt_prop->data, &manufacturer_sv, &model_sv)) {
            return false;
        }

        struct devicetree_prop_model *const model_prop =
            kmalloc(sizeof(*model_prop));

        if (model_prop == NULL) {
            return false;
        }

        model_prop->kind = DEVICETREE_PROP_MODEL;
        model_prop->manufacturer = manufacturer_sv;
        model_prop->model = model_sv;

        if (!array_append(&node->known_props, &model_prop)) {
            kfree(model_prop);
            return false;
        }
    } else if (sv_equals(name, SV_STATIC("status"))) {
        enum devicetree_prop_status_kind status = DEVICETREE_PROP_STATUS_OKAY;
        if (!parse_status_prop(fdt_prop->data, &status)) {
            return false;
        }

        struct devicetree_prop_status *const status_prop =
            kmalloc(sizeof(*status_prop));

        if (status_prop == NULL) {
            return false;
        }

        status_prop->kind = DEVICETREE_PROP_STATUS;
        status_prop->status = status;

        if (!array_append(&node->known_props, &status_prop)) {
            kfree(status_prop);
            return false;
        }
    } else if (sv_equals(name, SV_STATIC("phandle"))) {
        uint32_t phandle = 0;
        if (!parse_integer_prop(fdt_prop, prop_len, &phandle)) {
            return false;
        }

        struct devicetree_prop_phandle *const phandle_prop =
            kmalloc(sizeof(*phandle_prop));

        if (phandle_prop == NULL) {
            return false;
        }

        phandle_prop->kind = DEVICETREE_PROP_PHANDLE;
        phandle_prop->phandle = phandle;

        if (!array_append(&node->known_props, &phandle_prop)) {
            kfree(phandle_prop);
            return false;
        }

        if (!array_append(&tree->phandle_list, &node)) {
            kfree(phandle_prop);
            return false;
        }
    } else if (sv_equals(name, SV_STATIC("#address-cells"))) {
        uint32_t addr_cells = 0;
        if (!parse_integer_prop(fdt_prop, prop_len, &addr_cells)) {
            return false;
        }

        if (addr_cells == 0) {
            return false;
        }

        addr_size_cells->addr_cells = addr_cells;
    } else if (sv_equals(name, SV_STATIC("#size-cells"))) {
        uint32_t size_cells = 0;
        if (!parse_integer_prop(fdt_prop, prop_len, &size_cells)) {
            return false;
        }

        addr_size_cells->size_cells = size_cells;
    } else if (sv_equals(name, SV_STATIC("virtual-reg"))) {
        uint32_t address = 0;
        if (!parse_integer_prop(fdt_prop, prop_len, &address)) {
            return false;
        }

        struct devicetree_prop_virtual_reg *const virt_reg_prop =
            kmalloc(sizeof(*virt_reg_prop));

        if (virt_reg_prop == NULL) {
            return false;
        }

        virt_reg_prop->kind = DEVICETREE_PROP_VIRTUAL_REG;
        virt_reg_prop->address = address;

        if (!array_append(&node->known_props, &virt_reg_prop)) {
            kfree(virt_reg_prop);
            return false;
        }
    } else if (sv_equals(name, SV_STATIC("dma-ranges"))) {
        struct array range_arr = ARRAY_INIT(sizeof(struct range));
        if (!parse_ranges_prop(dtb,
                               fdt_prop,
                               prop_len,
                               nodeoff,
                               parent_nodeoff,
                               &range_arr))
        {
            return false;
        }

        struct devicetree_prop_ranges *const ranges_prop =
            kmalloc(sizeof(*ranges_prop));

        if (ranges_prop == NULL) {
            array_destroy(&range_arr);
            return false;
        }

        ranges_prop->kind = DEVICETREE_PROP_DMA_RANGES;
        ranges_prop->range_list = range_arr;

        if (!array_append(&node->known_props, &ranges_prop)) {
            kfree(ranges_prop);
            return false;
        }
    } else if (sv_equals(name, SV_STATIC("dma-coherent"))) {
        struct devicetree_prop_no_value *const prop = kmalloc(sizeof(*prop));
        if (prop == NULL) {
            return false;
        }

        prop->kind = DEVICETREE_PROP_DMA_COHERENT;
        if (!array_append(&node->known_props, &prop)) {
            kfree(prop);
            return false;
        }
    } else if (sv_equals(name, SV_STATIC("interrupts"))) {
        struct array list = ARRAY_INIT(sizeof(uint32_t));
        if (!parse_integer_list_prop(fdt_prop, prop_len, &list)) {
            return false;
        }

        struct devicetree_prop_interrupts *const prop = kmalloc(sizeof(*prop));
        if (prop == NULL) {
            return false;
        }

        prop->kind = DEVICETREE_PROP_INTERRUPTS;
        prop->list = list;

        if (!array_append(&node->known_props, &prop)) {
            kfree(prop);
            return false;
        }
    } else if (sv_equals(name, SV_STATIC("interrupt-parent"))) {
        uint32_t phandle = 0;
        if (!parse_integer_prop(fdt_prop, prop_len, &phandle)) {
            return false;
        }

        struct devicetree_prop_interrupt_parent *const prop =
            kmalloc(sizeof(*prop));

        if (prop == NULL) {
            return false;
        }

        prop->kind = DEVICETREE_PROP_INTERRUPT_PARENT;
        prop->phandle = phandle;

        if (!array_append(&node->known_props, &prop)) {
            kfree(prop);
            return false;
        }
    } else if (sv_equals(name, SV_STATIC("#interrupt-cells"))) {
        uint32_t count = 0;
        if (!parse_integer_prop(fdt_prop, prop_len, &count)) {
            return false;
        }

        struct devicetree_prop_interrupt_cells *const prop =
            kmalloc(sizeof(*prop));

        if (prop == NULL) {
            return false;
        }

        prop->kind = DEVICETREE_PROP_INTERRUPT_CELLS;
        prop->count = count;

        if (!array_append(&node->known_props, &prop)) {
            kfree(prop);
            return false;
        }
    } else if (sv_equals(name, SV_STATIC("interrupt-map"))) {
        if (!array_append(int_map_list, &fdt_prop)) {
            return false;
        }
    } else if (sv_has_suffix(name, SV_STATIC("-map"))) {
        struct array list =
            ARRAY_INIT(sizeof(struct devicetree_prop_specifier_map_entry));

        if (!parse_specifier_map_prop(dtb,
                                      node,
                                      fdt_prop,
                                      prop_len,
                                      parent_nodeoff,
                                      &list))
        {
            return false;
        }

        if (array_empty(list)) {
            return true;
        }

        struct devicetree_prop_specifier_map *const prop =
            kmalloc(sizeof(*prop));

        if (prop == NULL) {
            return false;
        }

        prop->kind = DEVICETREE_PROP_SPECIFIER_MAP;
        prop->name = name;
        prop->list = list;

        if (!array_append(&node->known_props, &prop)) {
            kfree(prop);
            return false;
        }
    } else if (sv_has_suffix(name, SV_STATIC("-cells"))) {
        uint32_t cells = 0;
        if (!parse_integer_prop(fdt_prop, prop_len, &cells)) {
            return false;
        }

        struct devicetree_prop_specifier_cells *const prop =
            kmalloc(sizeof(*prop));

        if (prop == NULL) {
            return false;
        }

        prop->kind = DEVICETREE_PROP_SPECIFIER_MAP;
        prop->name = name;
        prop->cells = cells;

        if (!array_append(&node->known_props, &prop)) {
            kfree(prop);
            return false;
        }
    } else {
        struct devicetree_prop_other *const other_prop =
            kmalloc(sizeof(*other_prop));

        if (other_prop == NULL) {
            return false;
        }

        other_prop->name = name;
        other_prop->data = &fdt_prop->data;
        other_prop->data_length = (uint32_t)prop_len;

        if (!array_append(&node->known_props, &other_prop)) {
            kfree(other_prop);
            return false;
        }
    }

    return true;
}

static bool
parse_node_children(const void *const dtb,
                    struct devicetree *const tree,
                    struct devicetree_node *const parent)
{
    struct array int_map_list = ARRAY_INIT(sizeof(struct fdt_property *));
    int nodeoff = 0;

    fdt_for_each_subnode(nodeoff, dtb, parent->nodeoff) {
        struct devicetree_node *const node = kmalloc(sizeof(*node));
        if (node == NULL) {
            return false;
        }

        list_init(&node->child_list);
        list_init(&node->sibling_list);

        int lenp = 0;

        node->parent = parent;
        node->nodeoff = nodeoff;
        node->known_props = ARRAY_INIT(sizeof(void *));
        node->other_props = ARRAY_INIT(sizeof(struct devicetree_prop_other *));
        node->name =
            sv_create_length(fdt_get_name(dtb, nodeoff, &lenp), (uint64_t)lenp);

        struct devicetree_prop_addr_size_cells addr_size_cells_prop = {
            .addr_cells = UINT32_MAX,
            .size_cells = UINT32_MAX
        };

        int prop_off = 0;
        fdt_for_each_property_offset(prop_off, dtb, nodeoff) {
            const int parent_off = parent->nodeoff;
            if (!parse_node_prop(dtb,
                                 nodeoff,
                                 prop_off,
                                 parent_off,
                                 tree,
                                 node,
                                 &int_map_list,
                                 &addr_size_cells_prop))
            {
                return false;
            }
        }

        if (addr_size_cells_prop.addr_cells != UINT32_MAX) {
            if (addr_size_cells_prop.size_cells == UINT32_MAX) {
                printk(LOGLEVEL_WARN,
                       "devicetree: node " SV_FMT " has #address-cells prop "
                       "but no #size-cells prop\n",
                       SV_FMT_ARGS(node->name));
                return false;
            }

            struct devicetree_prop_addr_size_cells *const addr_size_cells =
                kmalloc(sizeof(*addr_size_cells));

            *addr_size_cells = addr_size_cells_prop;
            if (!array_append(&node->known_props, addr_size_cells)) {
                kfree(addr_size_cells);
                return false;
            }
        } else if (addr_size_cells_prop.size_cells != UINT32_MAX) {
            printk(LOGLEVEL_WARN,
                   "devicetree: node " SV_FMT " has #address-cells prop but no "
                   "#size-cells prop\n",
                   SV_FMT_ARGS(node->name));
            return false;
        }

        if (!parse_node_children(dtb, tree, node)) {
            return false;
        }

        list_add(&parent->child_list, &node->sibling_list);
    }

    array_foreach(&int_map_list, struct fdt_property *, iter) {

    }

    return true;
}

bool devicetree_parse(struct devicetree *const tree, const void *const dtb) {
    int prop_off = 0;

    struct array int_map_list = ARRAY_INIT(sizeof(struct fdt_property *));
    struct devicetree_prop_addr_size_cells addr_size_cells_prop;
    struct devicetree_node *const root = tree->root;

    fdt_for_each_property_offset(prop_off, dtb, /*nodeoffset=*/0) {
        if (!parse_node_prop(dtb,
                             /*nodeoff=*/0,
                             prop_off,
                             /*parent_nodeoff=*/-1,
                             tree,
                             root,
                             &int_map_list,
                             &addr_size_cells_prop))
        {
            return false;
        }
    }

    return parse_node_children(dtb, tree, tree->root);
}