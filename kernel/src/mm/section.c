/*
 * kernel/src/mm/sections.c
 * © suhas pai
 */

#include "lib/overflow.h"
#include "mm/page.h"

#include "sys/boot.h"

__optimize(3) void
page_section_init(struct page_section *const section,
                  struct page_zone *const zone,
                  const struct range range,
                  const uint64_t pfn)
{
    section->zone = zone;
    section->lock = SPINLOCK_INIT();
    section->pfn = pfn;
    section->range = range;
    section->min_order = 0;
    section->max_order = 0;
    section->total_free = 0;

    for (uint8_t i = 0; i != MAX_ORDER; i++) {
        list_init(&section->freelist_list[i].page_list);
        section->freelist_list[i].count = 0;
    }

    list_init(&section->zone_list);
}

__optimize(3) struct page_section *phys_to_section(const uint64_t phys) {
    struct page_section *const begin = mm_get_page_section_list();
    struct page_section *const end = begin + mm_get_section_count();

    for (struct page_section *iter = begin; iter != end; iter++) {
        if (range_has_loc(iter->range, phys)) {
            return iter;
        }
    }

    verify_not_reached();
}

__optimize(3) uint64_t phys_to_pfn(const uint64_t phys) {
    const struct page_section *const begin = mm_get_page_section_list();
    const struct page_section *const end = begin + mm_get_section_count();

    for (const struct page_section *iter = begin; iter != end; iter++) {
        if (range_has_loc(iter->range, phys)) {
            return iter->pfn + ((phys - iter->range.front) >> PAGE_SHIFT);
        }
    }

    verify_not_reached();
}

__optimize(3) uint64_t page_to_phys(const struct page *const page) {
    assert((uint64_t)page >= PAGE_OFFSET && (uint64_t)page < PAGE_END);
    const struct page_section *const section = page_to_section(page);

    const uint64_t page_pfn = page_to_pfn(page);
    const uint64_t relative_pfn = check_sub_assert(page_pfn, section->pfn);

    return section->range.front + (relative_pfn << PAGE_SHIFT);
}