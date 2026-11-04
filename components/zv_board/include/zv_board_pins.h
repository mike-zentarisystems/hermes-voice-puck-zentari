/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 *
 * Board: Waveshare ESP32-S3-Touch-LCD-1.85C-BOX, Rev2.0 (V2 schematic) ONLY.
 *
 * Every value below is copied from Waveshare upstream revision
 * 8ead4a96bf3a278fc4ebd8ef4768657e17fa2880 and cited per constant in
 * hardware/pinmap.json. scripts/check_pinmap.py fails CI if this header and
 * the JSON diverge. Nothing here has been confirmed on hardware yet; see the
 * gate named in the JSON for each value. Do not add a pin without a source.
 */
#pragma once

/* ---- Shared I2C bus: touch, ES8311, ES7210, TCA9554, PCF85063 ---- */
#define ZV_I2C_PORT 0
#define ZV_PIN_I2C_SCL 10
#define ZV_PIN_I2C_SDA 11

/* 7-bit I2C addresses */
#define ZV_I2C_ADDR_CST816 0x15   /* CST816.h */
#define ZV_I2C_ADDR_ES8311 0x18   /* V2 schematic note "ES8311_I2C_ADD: 0x18" */
#define ZV_I2C_ADDR_TCA9554 0x20  /* TCA9554PWR.h */
#define ZV_I2C_ADDR_ES7210 0x40   /* esp_codec_dev ES7210 default used by bsp_board.c; strap unverified */
#define ZV_I2C_ADDR_PCF85063 0x51 /* PCF85063.h */

/* ---- ST77916 360x360 QSPI display ---- */
#define ZV_PIN_LCD_QSPI_SCK 40
#define ZV_PIN_LCD_QSPI_D0 46
#define ZV_PIN_LCD_QSPI_D1 45
#define ZV_PIN_LCD_QSPI_D2 42
#define ZV_PIN_LCD_QSPI_D3 41
#define ZV_PIN_LCD_QSPI_CS 21
#define ZV_PIN_LCD_TE 18 /* Present upstream but unused by the vendor driver */
#define ZV_PIN_LCD_BACKLIGHT 5
#define ZV_LCD_PCLK_HZ 80000000
#define ZV_LCD_ID_READ_PCLK_HZ 3000000
#define ZV_LCD_BACKLIGHT_PWM_HZ 5000

/* ---- TCA9554 expander pins (1-based, matching upstream TCA9554_EXIOn) ---- */
#define ZV_EXIO_TOUCH_RESET 1
#define ZV_EXIO_LCD_RESET 2

/* ---- CST816-family capacitive touch ---- */
#define ZV_PIN_TOUCH_INT 4

/* ---- Duplex I2S to ES8311 (playback) and ES7210 (capture) ---- */
#define ZV_PIN_I2S_MCLK 2
#define ZV_PIN_I2S_BCLK 48
#define ZV_PIN_I2S_WS 38
#define ZV_PIN_I2S_DOUT 47 /* ESP32-S3 -> ES8311 */
#define ZV_PIN_I2S_DIN 39  /* ES7210 -> ESP32-S3 */

/* ---- NS4150B speaker amplifier enable (upstream drives high to enable) ---- */
#define ZV_PIN_PA_CTRL 15

/* ---- Battery sense: ADC1 channel 7, external divider ---- */
#define ZV_PIN_BAT_ADC 8
#define ZV_BAT_ADC_CHANNEL 7
#define ZV_BAT_DIVIDER_RATIO 3
