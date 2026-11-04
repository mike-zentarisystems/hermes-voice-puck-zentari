/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Memory budgets are DESIGN TARGETS, not measurements. Capacities come from
 * the V2 schematic (ESP32-S3R8 = 8 MB in-package PSRAM, W25Q128JVSI = 16 MB
 * flash) and the vendor sdkconfig. Usable internal heap after boot must be
 * measured on hardware (gate HW-MEM-01) and the caps below revised.
 */

#define ZV_KIB(x) ((uint32_t)(x) * 1024u)
#define ZV_MIB(x) ((uint32_t)(x) * 1024u * 1024u)

#define ZV_LCD_WIDTH 360u
#define ZV_LCD_HEIGHT 360u
#define ZV_LCD_BYTES_PER_PIXEL 2u /* RGB565, vendor EXAMPLE_LCD_COLOR_BITS = 16 */

/* ---- Capacities ---- */
#define ZV_MEM_PSRAM_TOTAL ZV_MIB(8)
#define ZV_MEM_FLASH_TOTAL ZV_MIB(16)
/** Assumed usable internal heap at app_main; unmeasured (HW-MEM-01). */
#define ZV_MEM_INTERNAL_CAP ZV_KIB(280)
/** PSRAM usable by the application after IDF reservations; unmeasured (HW-MEM-01). */
#define ZV_MEM_PSRAM_CAP ZV_KIB(7680)

/* ---- Internal (DMA-capable / latency-critical) budget ---- */
/* Two partial draw buffers of 1/10 screen each. The vendor comments that SPI
 * DMA cannot transmit directly from PSRAM in this configuration. */
#define ZV_MEM_INT_LVGL_DRAW (2u * (ZV_LCD_WIDTH * (ZV_LCD_HEIGHT / 10u)) * ZV_LCD_BYTES_PER_PIXEL)
#define ZV_MEM_INT_WIFI_LWIP ZV_KIB(72)
#define ZV_MEM_INT_TLS ZV_KIB(24)
#define ZV_MEM_INT_I2S_DMA ZV_KIB(16)
#define ZV_MEM_INT_TASK_STACKS ZV_KIB(48)
#define ZV_MEM_INT_RESERVE ZV_KIB(64)

#define ZV_MEM_INT_TOTAL (ZV_MEM_INT_LVGL_DRAW + ZV_MEM_INT_WIFI_LWIP + ZV_MEM_INT_TLS + ZV_MEM_INT_I2S_DMA + \
                          ZV_MEM_INT_TASK_STACKS + ZV_MEM_INT_RESERVE)

/* ---- PSRAM budget ---- */
#define ZV_MEM_PS_LVGL_HEAP ZV_KIB(768)
#define ZV_MEM_PS_UI_ASSETS ZV_KIB(512)
#define ZV_MEM_PS_AFE ZV_KIB(1536)
#define ZV_MEM_PS_CAPTURE_RING ZV_KIB(256)
#define ZV_MEM_PS_PLAYBACK_JITTER ZV_KIB(192)
#define ZV_MEM_PS_UPLINK ZV_KIB(64)
#define ZV_MEM_PS_WIFI_LWIP ZV_KIB(512)
#define ZV_MEM_PS_TLS ZV_KIB(256)
#define ZV_MEM_PS_WEBSOCKET ZV_KIB(64)
#define ZV_MEM_PS_OTA ZV_KIB(128)
#define ZV_MEM_PS_TASK_STACKS ZV_KIB(160)
#define ZV_MEM_PS_RESERVE ZV_KIB(1024)

#define ZV_MEM_PS_TOTAL (ZV_MEM_PS_LVGL_HEAP + ZV_MEM_PS_UI_ASSETS + ZV_MEM_PS_AFE + ZV_MEM_PS_CAPTURE_RING + \
                         ZV_MEM_PS_PLAYBACK_JITTER + ZV_MEM_PS_UPLINK + ZV_MEM_PS_WIFI_LWIP + ZV_MEM_PS_TLS + \
                         ZV_MEM_PS_WEBSOCKET + ZV_MEM_PS_OTA + ZV_MEM_PS_TASK_STACKS + ZV_MEM_PS_RESERVE)

_Static_assert(ZV_MEM_INT_TOTAL <= ZV_MEM_INTERNAL_CAP, "internal memory budget exceeds cap");
_Static_assert(ZV_MEM_PS_TOTAL <= ZV_MEM_PSRAM_CAP, "PSRAM budget exceeds cap");
_Static_assert(ZV_MEM_PSRAM_CAP <= ZV_MEM_PSRAM_TOTAL, "PSRAM cap exceeds physical PSRAM");

typedef enum {
    ZV_MEM_REGION_INTERNAL = 0,
    ZV_MEM_REGION_PSRAM,
} zv_mem_region_t;

typedef struct {
    const char *name;
    zv_mem_region_t region;
    uint32_t bytes;
    const char *basis;
} zv_mem_budget_entry_t;

const zv_mem_budget_entry_t *zv_mem_budget_entries(size_t *count);
uint32_t zv_mem_budget_total(zv_mem_region_t region);
uint32_t zv_mem_budget_cap(zv_mem_region_t region);

#ifdef __cplusplus
}
#endif
