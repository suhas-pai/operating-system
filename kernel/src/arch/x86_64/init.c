/*
 * kernel/src/arch/x86_64/init.c
 * © suhas pai
 */

#include "cpu/info.h"
#include "mm/init.h"

#include "sys/gdt.h"
#include "sys/idt.h"

void arch_early_init() {

}

void arch_post_mm_init() {

}

__optimize(3) void arch_init() {
    gdt_load();
    idt_init();
    cpu_init();
    mm_arch_init();
}