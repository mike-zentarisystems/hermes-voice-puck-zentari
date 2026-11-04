/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 */
#include "zv_tca9554.h"

#include "esp_check.h"
#include "esp_log.h"

#define REG_INPUT 0x00
#define REG_OUTPUT 0x01
#define REG_CONFIG 0x03
#define I2C_TIMEOUT_MS 100
#define DEVICE_SPEED_HZ 400000

static const char *TAG = "zv_tca9554";

static esp_err_t read_reg(zv_tca9554_t *expander, uint8_t reg, uint8_t *value)
{
    return i2c_master_transmit_receive(expander->dev, &reg, 1, value, 1, I2C_TIMEOUT_MS);
}

static esp_err_t write_reg(zv_tca9554_t *expander, uint8_t reg, uint8_t value)
{
    const uint8_t frame[2] = {reg, value};
    return i2c_master_transmit(expander->dev, frame, sizeof(frame), I2C_TIMEOUT_MS);
}

esp_err_t zv_tca9554_init(zv_tca9554_t *expander, i2c_master_bus_handle_t bus, uint8_t address_7bit,
                          uint8_t config_reg)
{
    ESP_RETURN_ON_FALSE(expander != NULL && bus != NULL, ESP_ERR_INVALID_ARG, TAG, "null argument");

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address_7bit,
        .scl_speed_hz = DEVICE_SPEED_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &dev_cfg, &expander->dev), TAG, "add device 0x%02x failed",
                        address_7bit);

    ESP_RETURN_ON_ERROR(read_reg(expander, REG_OUTPUT, &expander->output_cache), TAG, "read output register failed");
    ESP_RETURN_ON_ERROR(write_reg(expander, REG_CONFIG, config_reg), TAG, "write config register failed");

    uint8_t readback = 0;
    ESP_RETURN_ON_ERROR(read_reg(expander, REG_CONFIG, &readback), TAG, "read config register failed");
    ESP_RETURN_ON_FALSE(readback == config_reg, ESP_ERR_INVALID_RESPONSE, TAG,
                        "config readback mismatch: wrote 0x%02x read 0x%02x", config_reg, readback);
    expander->config_cache = readback;

    ESP_LOGI(TAG, "ready at 0x%02x: config=0x%02x output=0x%02x", address_7bit, readback, expander->output_cache);
    return ESP_OK;
}

esp_err_t zv_tca9554_set_level(zv_tca9554_t *expander, uint8_t exio, bool level)
{
    ESP_RETURN_ON_FALSE(expander != NULL && expander->dev != NULL, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    ESP_RETURN_ON_FALSE(exio >= 1 && exio <= 8, ESP_ERR_INVALID_ARG, TAG, "EXIO%u out of range", exio);

    uint8_t mask = (uint8_t)(1u << (exio - 1u));
    ESP_RETURN_ON_FALSE((expander->config_cache & mask) == 0, ESP_ERR_INVALID_STATE, TAG,
                        "EXIO%u is configured as input", exio);

    uint8_t next = level ? (uint8_t)(expander->output_cache | mask) : (uint8_t)(expander->output_cache & ~mask);
    ESP_RETURN_ON_ERROR(write_reg(expander, REG_OUTPUT, next), TAG, "write EXIO%u failed", exio);
    expander->output_cache = next;
    return ESP_OK;
}

esp_err_t zv_tca9554_read_inputs(zv_tca9554_t *expander, uint8_t *value)
{
    ESP_RETURN_ON_FALSE(expander != NULL && expander->dev != NULL && value != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "invalid argument");
    return read_reg(expander, REG_INPUT, value);
}
