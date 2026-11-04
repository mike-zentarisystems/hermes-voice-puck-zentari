/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Board bring-up: shared I2C bus, TCA9554 expander, amplifier held off.
 * Safe to call once; subsequent calls return ESP_OK.
 */
esp_err_t zv_board_init(void);

/** Shared I2C master bus. NULL before zv_board_init(). */
i2c_master_bus_handle_t zv_board_i2c_bus(void);

/** Read the PCF85063 calendar when it contains a valid date and time. */
esp_err_t zv_board_rtc_read(struct tm *out_time);

/** Drives a TCA9554 reset line low for `low_ms`, then high, then waits `settle_ms`. */
esp_err_t zv_board_exio_pulse_reset(uint8_t exio, uint32_t low_ms, uint32_t settle_ms);

/** NS4150B enable on GPIO15 (high = enabled per upstream Audio_Driver.c). */
esp_err_t zv_board_pa_enable(bool enable);

esp_err_t zv_board_backlight_init(void);
/** 0..100 percent using the upstream duty curve. */
esp_err_t zv_board_backlight_set(uint8_t percent);

esp_err_t zv_board_battery_init(void);
/** Battery voltage in mV. Returns ESP_ERR_INVALID_STATE if ADC calibration is unavailable. */
esp_err_t zv_board_battery_read_mv(uint32_t *battery_mv);

typedef struct {
    uint8_t address;
    const char *name;
    bool present;
} zv_i2c_probe_entry_t;

#define ZV_BOARD_I2C_EXPECTED_COUNT 5u

typedef struct {
    zv_i2c_probe_entry_t entries[ZV_BOARD_I2C_EXPECTED_COUNT];
    size_t missing;
} zv_i2c_probe_report_t;

/** Probes every expected I2C address. Missing devices are counted, not fatal. */
esp_err_t zv_board_probe_i2c(zv_i2c_probe_report_t *report);

typedef struct {
    size_t psram_bytes;
    uint32_t flash_bytes;
    size_t internal_free;
    size_t internal_min_free;
    size_t psram_free;
    size_t dma_largest_block;
    bool psram_matches_rev2;  /**< >= 8 MB detected */
    bool flash_matches_rev2;  /**< 16 MB detected */
} zv_board_memory_report_t;

esp_err_t zv_board_memory_report(zv_board_memory_report_t *report);

/**
 * Bounded M0 lab aid: initializes only verified Rev2 display/backlight/touch,
 * renders visible display feedback, and logs touch coordinates. Audio, mics,
 * amplifier, network, gateway, Hermes, credentials, OTA, eFuse, erase, and
 * destructive actions remain outside this function.
 */
esp_err_t zv_board_m0_acceptance_demo(void);

/** Normal voice boot: initialize display/touch without an operator challenge.
 * Does not mark display rendering as physically confirmed. */
esp_err_t zv_board_voice_ui_init(void);

/**
 * True only when the operator answered the M0 positional display challenge,
 * which is the only evidence this firmware can produce that pixels were
 * actually lit and legible. Always false when the challenge is disabled.
 */
bool zv_board_m0_display_confirmed(void);

/** Initializes the ST77916 panel through the vendored driver (idempotent). */
esp_err_t zv_board_lcd_init(void);

/** DMA-safe rectangle fill. x1/y1 exclusive. Clamped to 360x360. */
esp_err_t zv_board_lcd_fill_rect(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint16_t color);

/** Synchronous, DMA-safe RGB565 row copy; source may be stack or flash. */
esp_err_t zv_board_lcd_draw_row(uint16_t x, uint16_t y, uint16_t width, const uint16_t *pixels);
/** Submit completed RGB565 pixels (at most 3360), without intermediate clears. */
esp_err_t zv_board_lcd_draw_patch(uint16_t x, uint16_t y, uint16_t width, uint16_t height,
                                const uint16_t *pixels);

/** Initializes CST816 touch (idempotent). */
esp_err_t zv_board_touch_init(void);

/** One touch poll. *touched is false when no finger is down. */
esp_err_t zv_board_touch_poll(bool *touched, uint16_t *x, uint16_t *y);

/** Waits until no finger is down, or until timeout_ms elapses. */
esp_err_t zv_board_touch_wait_release(uint32_t timeout_ms);
/** Wait for touch IRQ or timeout; falls back to bounded sleep if IRQ unavailable. */
void zv_board_touch_wait(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
