/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 */
#include "zv_memory_budget.h"

#include <stddef.h>

static const zv_mem_budget_entry_t k_entries[] = {
    {"lvgl_draw_buffers", ZV_MEM_REGION_INTERNAL, ZV_MEM_INT_LVGL_DRAW,
     "2 x (360 x 36) px RGB565, DMA-capable internal RAM (vendor LVGL_Driver.c keeps draw buffers internal)"},
    {"wifi_lwip_internal", ZV_MEM_REGION_INTERNAL, ZV_MEM_INT_WIFI_LWIP,
     "Wi-Fi static RX/TX buffers that must stay internal; remainder moved to PSRAM via SPIRAM_TRY_ALLOCATE_WIFI_LWIP"},
    {"tls_internal", ZV_MEM_REGION_INTERNAL, ZV_MEM_INT_TLS, "mbedTLS contexts not relocatable to PSRAM"},
    {"i2s_dma", ZV_MEM_REGION_INTERNAL, ZV_MEM_INT_I2S_DMA, "I2S DMA descriptors and buffers for duplex codec path"},
    {"task_stacks_internal", ZV_MEM_REGION_INTERNAL, ZV_MEM_INT_TASK_STACKS,
     "Stacks of tasks that touch flash or run with cache disabled"},
    {"internal_reserve", ZV_MEM_REGION_INTERNAL, ZV_MEM_INT_RESERVE, "IDF drivers, fragmentation headroom"},
    {"lvgl_heap", ZV_MEM_REGION_PSRAM, ZV_MEM_PS_LVGL_HEAP, "LVGL object heap via custom allocator"},
    {"ui_assets", ZV_MEM_REGION_PSRAM, ZV_MEM_PS_UI_ASSETS, "Fonts and decoded avatar assets"},
    {"afe", ZV_MEM_REGION_PSRAM, ZV_MEM_PS_AFE, "esp-sr AFE (AEC/NS/VAD) working memory; unmeasured"},
    {"capture_ring", ZV_MEM_REGION_PSRAM, ZV_MEM_PS_CAPTURE_RING, "~2 s of 16 kHz x 4 ch x 16-bit capture"},
    {"playback_jitter", ZV_MEM_REGION_PSRAM, ZV_MEM_PS_PLAYBACK_JITTER, "~4 s of 24 kHz mono PCM16 downlink"},
    {"uplink", ZV_MEM_REGION_PSRAM, ZV_MEM_PS_UPLINK, "Outgoing encoded audio frames"},
    {"wifi_lwip_psram", ZV_MEM_REGION_PSRAM, ZV_MEM_PS_WIFI_LWIP, "Dynamic Wi-Fi/lwIP buffers"},
    {"tls_psram", ZV_MEM_REGION_PSRAM, ZV_MEM_PS_TLS, "mbedTLS record buffers (MBEDTLS_EXTERNAL_MEM_ALLOC)"},
    {"websocket", ZV_MEM_REGION_PSRAM, ZV_MEM_PS_WEBSOCKET, "WebSocket RX/TX buffers"},
    {"ota", ZV_MEM_REGION_PSRAM, ZV_MEM_PS_OTA, "HTTPS OTA download buffer"},
    {"task_stacks_psram", ZV_MEM_REGION_PSRAM, ZV_MEM_PS_TASK_STACKS, "Stacks of tasks allowed in PSRAM"},
    {"psram_reserve", ZV_MEM_REGION_PSRAM, ZV_MEM_PS_RESERVE, "Fragmentation and future features"},
};

const zv_mem_budget_entry_t *zv_mem_budget_entries(size_t *count)
{
    if (count != NULL) {
        *count = sizeof(k_entries) / sizeof(k_entries[0]);
    }
    return k_entries;
}

uint32_t zv_mem_budget_total(zv_mem_region_t region)
{
    uint32_t total = 0u;
    for (size_t i = 0u; i < sizeof(k_entries) / sizeof(k_entries[0]); i++) {
        if (k_entries[i].region == region) {
            total += k_entries[i].bytes;
        }
    }
    return total;
}

uint32_t zv_mem_budget_cap(zv_mem_region_t region)
{
    return region == ZV_MEM_REGION_INTERNAL ? ZV_MEM_INTERNAL_CAP : ZV_MEM_PSRAM_CAP;
}
