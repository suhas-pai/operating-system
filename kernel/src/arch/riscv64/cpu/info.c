/*
 * kernel/src/arch/riscv64/cpu/info.c
 * © suhas pai
 */

#include "info.h"

static struct cpu_info g_base_cpu_info = {
    .pagemap = &kernel_pagemap,
    .pagemap_node = LIST_INIT(g_base_cpu_info.pagemap_node),
    .spur_int_count = 0
};

__optimize(3) const struct cpu_info *get_base_cpu_info() {
    return &g_base_cpu_info;
}

__optimize(3) const struct cpu_info *this_cpu() {
    return &g_base_cpu_info;
}

__optimize(3) struct cpu_info *this_cpu_mut() {
    return &g_base_cpu_info;
}