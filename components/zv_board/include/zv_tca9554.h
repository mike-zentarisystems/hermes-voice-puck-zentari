/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** TCA9554 8-bit I/O expander. Every register access returns an explicit error. */
typedef struct {
    i2c_master_dev_handle_t dev;
    uint8_t output_cache;
    uint8_t config_cache; /**< 1 = input, 0 = output (datasheet register 0x03) */
} zv_tca9554_t;

/**
 * Attaches the device and writes the configuration register.
 * @param config_reg bit n = 1 configures EXIO(n+1) as input.
 */
esp_err_t zv_tca9554_init(zv_tca9554_t *expander, i2c_master_bus_handle_t bus, uint8_t address_7bit,
                          uint8_t config_reg);

/** Sets EXIO pin (1..8) level. The pin must be configured as output. */
esp_err_t zv_tca9554_set_level(zv_tca9554_t *expander, uint8_t exio, bool level);
esp_err_t zv_tca9554_read_inputs(zv_tca9554_t *expander, uint8_t *value);

#ifdef __cplusplus
}
#endif
