/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 */
#include "zv_memory_budget.h"
#include "zv_test.h"

void test_suite_memory_budget(void)
{
    size_t count = 0;
    const zv_mem_budget_entry_t *entries = zv_mem_budget_entries(&count);
    ZV_CHECK(entries != NULL);
    ZV_CHECK(count >= 10u);

    for (size_t i = 0; i < count; i++) {
        ZV_CHECK(entries[i].name != NULL && entries[i].name[0] != '\0');
        ZV_CHECK(entries[i].basis != NULL && entries[i].basis[0] != '\0');
        ZV_CHECK(entries[i].bytes > 0u);
        for (size_t j = i + 1u; j < count; j++) {
            ZV_CHECK(strcmp(entries[i].name, entries[j].name) != 0);
        }
    }

    uint32_t internal = zv_mem_budget_total(ZV_MEM_REGION_INTERNAL);
    uint32_t psram = zv_mem_budget_total(ZV_MEM_REGION_PSRAM);
    ZV_CHECK_EQ(internal, ZV_MEM_INT_TOTAL);
    ZV_CHECK_EQ(psram, ZV_MEM_PS_TOTAL);
    ZV_CHECK(internal <= zv_mem_budget_cap(ZV_MEM_REGION_INTERNAL));
    ZV_CHECK(psram <= zv_mem_budget_cap(ZV_MEM_REGION_PSRAM));
    ZV_CHECK_EQ(ZV_MEM_INT_LVGL_DRAW, 51840);

    printf("  budget internal %u / %u bytes, psram %u / %u bytes (targets; measure on hardware: HW-MEM-01)\n",
           (unsigned)internal, (unsigned)zv_mem_budget_cap(ZV_MEM_REGION_INTERNAL), (unsigned)psram,
           (unsigned)zv_mem_budget_cap(ZV_MEM_REGION_PSRAM));
}
