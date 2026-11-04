/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 */
#include "zv_board.h"

#include <inttypes.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_st77916.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "zv_board_pins.h"
#include "zv_tca9554.h"

static const char *TAG = "zv_board";

#define BACKLIGHT_TIMER LEDC_TIMER_0
#define BACKLIGHT_CHANNEL LEDC_CHANNEL_0
#define BACKLIGHT_RESOLUTION LEDC_TIMER_13_BIT
#define BACKLIGHT_MAX_DUTY ((1u << 13) - 1u)
#define PROBE_TIMEOUT_MS 50
#define LCD_HOST SPI2_HOST
#define LCD_WIDTH 360
#define LCD_HEIGHT 360
#define LCD_OPCODE_READ_CMD 0x0Bu
#define LCD_ID_REG 0x04u
#define LCD_FILL_LINES 16u
#define LCD_BITS_PER_PIXEL 16
#define M0_GRID 3u
#define M0_CELL (LCD_WIDTH / (int)M0_GRID)
#define M0_MARK_INSET 20
#define M0_POLL_MS 100u
#define M0_DEBOUNCE_MS 250u
#define TOUCH_DATA_START_REG 0x02u
#define TOUCH_CHIP_ID_REG 0xA7u
#define TOUCH_AUTOSLEEP_REG 0xFEu

static i2c_master_bus_handle_t s_i2c_bus;
static zv_tca9554_t s_exio;
static i2c_master_dev_handle_t s_touch_dev;
static i2c_master_dev_handle_t s_rtc_dev;
static esp_lcd_panel_io_handle_t s_lcd_io;
static esp_lcd_panel_handle_t s_lcd_panel;
static SemaphoreHandle_t s_lcd_trans_done;
static uint16_t *s_sprite_dma_row;
static bool s_sprite_dma_fault;
static bool s_initialized;
static bool s_backlight_ready;
static bool s_lcd_ready;
static bool s_display_confirmed;
static SemaphoreHandle_t s_touch_wake;
static bool s_touch_irq_ready;
static void touch_wake_isr(void *arg)
{
    (void)arg;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_touch_wake, &woken);
    if (woken) portYIELD_FROM_ISR();
}

/* Interrupt is a latency optimization; bounded polling remains the fallback. */
static void touch_wake_init(void)
{
    if (s_touch_irq_ready) return;
    if (s_touch_wake == NULL) s_touch_wake = xSemaphoreCreateBinary();
    if (s_touch_wake == NULL) return;
    const gpio_config_t cfg = {.pin_bit_mask=1ULL << ZV_PIN_TOUCH_INT,
        .mode=GPIO_MODE_INPUT, .pull_up_en=GPIO_PULLUP_ENABLE,
        .pull_down_en=GPIO_PULLDOWN_DISABLE, .intr_type=GPIO_INTR_NEGEDGE};
    esp_err_t err = gpio_config(&cfg);
    if (err == ESP_OK) {
        err = gpio_install_isr_service(0);
        if (err == ESP_ERR_INVALID_STATE) err = ESP_OK;
    }
    if (err == ESP_OK) err = gpio_isr_handler_add(ZV_PIN_TOUCH_INT, touch_wake_isr, NULL);
    s_touch_irq_ready = err == ESP_OK;
    if (!s_touch_irq_ready) ESP_LOGW(TAG, "touch IRQ unavailable; bounded polling fallback");
}
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_adc_cali;
static bool s_adc_calibrated;

static esp_err_t lcd_init(void);
static esp_err_t touch_init(void);
static esp_err_t touch_poll_once(bool *touched, uint16_t *out_x, uint16_t *out_y);
#if CONFIG_ZV_M0_DISPLAY_CONFIRM_ROUNDS > 0
static esp_err_t touch_wait_release(uint32_t timeout_ms);
#endif

static uint8_t exio_config_register(void)
{
#ifdef CONFIG_ZV_BOARD_EXIO_VENDOR_COMPAT
    /* Upstream TCA9554PWR_Init(0x00): all pins outputs, output register untouched. */
    return 0x00;
#else
    /* Only pins with source-established functions become outputs. */
    uint8_t outputs = (uint8_t)((1u << (ZV_EXIO_TOUCH_RESET - 1)) | (1u << (ZV_EXIO_LCD_RESET - 1)));
    return (uint8_t)~outputs;
#endif
}

esp_err_t zv_board_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = ZV_I2C_PORT,
        .sda_io_num = ZV_PIN_I2C_SDA,
        .scl_io_num = ZV_PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = false, /* V2 schematic shows external pull-ups */
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_i2c_bus), TAG, "I2C master bus init failed");

    const gpio_config_t pa_cfg = {
        .pin_bit_mask = 1ULL << ZV_PIN_PA_CTRL,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&pa_cfg), TAG, "PA_CTRL GPIO config failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(ZV_PIN_PA_CTRL, 0), TAG, "PA_CTRL drive low failed");

    ESP_RETURN_ON_ERROR(zv_tca9554_init(&s_exio, s_i2c_bus, ZV_I2C_ADDR_TCA9554, exio_config_register()), TAG,
                        "TCA9554 init failed (gate HW-I2C-01)");

    s_initialized = true;
    ESP_LOGI(TAG, "board init complete (Rev2.0 profile, upstream %s)", "8ead4a96");
    return ESP_OK;
}

i2c_master_bus_handle_t zv_board_i2c_bus(void)
{
    return s_i2c_bus;
}

static bool rtc_decode_bcd(uint8_t raw, uint8_t mask, int *value)
{
    const uint8_t digits = raw & mask;
    const int ones = digits & 0x0fu;
    const int tens = (digits >> 4) & 0x0fu;
    if (value == NULL || ones > 9 || tens > 9) {
        return false;
    }
    *value = tens * 10 + ones;
    return true;
}

esp_err_t zv_board_rtc_read(struct tm *out_time)
{
    ESP_RETURN_ON_FALSE(out_time != NULL, ESP_ERR_INVALID_ARG, TAG, "null RTC output");
    ESP_RETURN_ON_FALSE(s_initialized && s_i2c_bus != NULL, ESP_ERR_INVALID_STATE, TAG, "I2C bus not initialized");
    if (s_rtc_dev == NULL) {
        const i2c_device_config_t cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = ZV_I2C_ADDR_PCF85063,
            .scl_speed_hz = 100000,
        };
        ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c_bus, &cfg, &s_rtc_dev), TAG, "RTC add failed");
    }

    uint8_t reg = 0x00u;
    uint8_t control = 0u;
    ESP_RETURN_ON_ERROR(i2c_master_transmit_receive(s_rtc_dev, &reg, 1u, &control, 1u, 100u), TAG,
                        "RTC control read failed");
    if ((control & 0x20u) != 0u) {
        return ESP_ERR_INVALID_STATE; /* oscillator stopped */
    }

    uint8_t raw[7] = {0};
    reg = 0x04u;
    ESP_RETURN_ON_ERROR(i2c_master_transmit_receive(s_rtc_dev, &reg, 1u, raw, sizeof(raw), 100u), TAG,
                        "RTC calendar read failed");
    if ((raw[0] & 0x80u) != 0u) {
        return ESP_ERR_INVALID_STATE; /* oscillator-stop flag */
    }

    int second, minute, hour, day, month, year;
    if (!rtc_decode_bcd(raw[0], 0x7fu, &second) ||
        !rtc_decode_bcd(raw[1], 0x7fu, &minute) ||
        !rtc_decode_bcd(raw[2], (control & 0x02u) != 0u ? 0x1fu : 0x3fu, &hour) ||
        !rtc_decode_bcd(raw[3], 0x3fu, &day) ||
        !rtc_decode_bcd(raw[5], 0x1fu, &month) ||
        !rtc_decode_bcd(raw[6], 0xffu, &year)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const int weekday = raw[4] & 0x07u;
    const bool twelve_hour = (control & 0x02u) != 0u;
    if (second > 59 || minute > 59 || weekday > 6 || day < 1 || month < 1 || month > 12 ||
        year < 20 || hour < (twelve_hour ? 1 : 0) || hour > (twelve_hour ? 12 : 23)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (twelve_hour) {
        hour = (hour % 12) + ((raw[2] & 0x20u) != 0u ? 12 : 0);
    }

    const int full_year = 2000 + year;
    static const int days_in_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int month_days = days_in_month[month - 1];
    if (month == 2 && (full_year % 4) == 0) {
        month_days = 29;
    }
    if (day > month_days) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    *out_time = (struct tm){
        .tm_sec = second, .tm_min = minute, .tm_hour = hour,
        .tm_mday = day, .tm_mon = month - 1, .tm_year = full_year - 1900,
        .tm_wday = weekday, .tm_isdst = -1,
    };
    return ESP_OK;
}

esp_err_t zv_board_exio_pulse_reset(uint8_t exio, uint32_t low_ms, uint32_t settle_ms)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "board not initialized");
    ESP_RETURN_ON_ERROR(zv_tca9554_set_level(&s_exio, exio, false), TAG, "EXIO%u low failed", exio);
    vTaskDelay(pdMS_TO_TICKS(low_ms));
    ESP_RETURN_ON_ERROR(zv_tca9554_set_level(&s_exio, exio, true), TAG, "EXIO%u high failed", exio);
    vTaskDelay(pdMS_TO_TICKS(settle_ms));
    return ESP_OK;
}

esp_err_t zv_board_pa_enable(bool enable)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "board not initialized");
    return gpio_set_level(ZV_PIN_PA_CTRL, enable ? 1 : 0);
}

esp_err_t zv_board_backlight_init(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = BACKLIGHT_RESOLUTION,
        .timer_num = BACKLIGHT_TIMER,
        .freq_hz = ZV_LCD_BACKLIGHT_PWM_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "backlight timer config failed");
    const ledc_channel_config_t channel = {
        .gpio_num = ZV_PIN_LCD_BACKLIGHT,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = BACKLIGHT_CHANNEL,
        .timer_sel = BACKLIGHT_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel), TAG, "backlight channel config failed");
    s_backlight_ready = true;
    return ESP_OK;
}

esp_err_t zv_board_backlight_set(uint8_t percent)
{
    ESP_RETURN_ON_FALSE(s_backlight_ready, ESP_ERR_INVALID_STATE, TAG, "backlight not initialized");
    ESP_RETURN_ON_FALSE(percent <= 100, ESP_ERR_INVALID_ARG, TAG, "backlight %u%% out of range", percent);
    /* Upstream ST77916.c: Duty = MAX - 81 * (100 - Light), 0 turns the backlight off. */
    uint32_t duty = (percent == 0) ? 0u : BACKLIGHT_MAX_DUTY - 81u * (100u - percent);
    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, BACKLIGHT_CHANNEL, duty), TAG, "set duty failed");
    return ledc_update_duty(LEDC_LOW_SPEED_MODE, BACKLIGHT_CHANNEL);
}

esp_err_t zv_board_battery_init(void)
{
    /* M1 then soak both call this; ADC1 may already be owned. */
    if (s_adc != NULL) {
        return ESP_OK;
    }
    const adc_oneshot_unit_init_cfg_t unit_cfg = {.unit_id = ADC_UNIT_1};
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&unit_cfg, &s_adc), TAG, "ADC1 unit init failed");
    const adc_oneshot_chan_cfg_t chan_cfg = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc, (adc_channel_t)ZV_BAT_ADC_CHANNEL, &chan_cfg), TAG,
                        "ADC1 channel config failed");
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    const adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = ADC_UNIT_1,
        .chan = (adc_channel_t)ZV_BAT_ADC_CHANNEL,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    esp_err_t err = adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_adc_cali);
    s_adc_calibrated = (err == ESP_OK);
    if (!s_adc_calibrated) {
        ESP_LOGW(TAG, "ADC calibration unavailable (%s); battery readings disabled", esp_err_to_name(err));
    }
#else
    ESP_LOGW(TAG, "curve-fitting ADC calibration not supported; battery readings disabled");
#endif
    return ESP_OK;
}

esp_err_t zv_board_battery_read_mv(uint32_t *battery_mv)
{
    ESP_RETURN_ON_FALSE(battery_mv != NULL, ESP_ERR_INVALID_ARG, TAG, "null output");
    ESP_RETURN_ON_FALSE(s_adc != NULL, ESP_ERR_INVALID_STATE, TAG, "battery ADC not initialized");
    ESP_RETURN_ON_FALSE(s_adc_calibrated, ESP_ERR_INVALID_STATE, TAG, "ADC not calibrated; refusing raw estimate");
    int raw = 0;
    ESP_RETURN_ON_ERROR(adc_oneshot_read(s_adc, (adc_channel_t)ZV_BAT_ADC_CHANNEL, &raw), TAG, "ADC read failed");
    int pin_mv = 0;
    ESP_RETURN_ON_ERROR(adc_cali_raw_to_voltage(s_adc_cali, raw, &pin_mv), TAG, "ADC calibration failed");
    /* Divider ratio from upstream BAT_Driver.c; board-level calibration is gate HW-BAT-01. */
    *battery_mv = (uint32_t)pin_mv * ZV_BAT_DIVIDER_RATIO;
    return ESP_OK;
}

esp_err_t zv_board_probe_i2c(zv_i2c_probe_report_t *report)
{
    ESP_RETURN_ON_FALSE(report != NULL, ESP_ERR_INVALID_ARG, TAG, "null report");
    ESP_RETURN_ON_FALSE(s_i2c_bus != NULL, ESP_ERR_INVALID_STATE, TAG, "I2C bus not initialized");
    const zv_i2c_probe_entry_t expected[ZV_BOARD_I2C_EXPECTED_COUNT] = {
        {ZV_I2C_ADDR_CST816, "CST816 touch", false},    {ZV_I2C_ADDR_ES8311, "ES8311 DAC", false},
        {ZV_I2C_ADDR_TCA9554, "TCA9554 expander", false}, {ZV_I2C_ADDR_ES7210, "ES7210 ADC", false},
        {ZV_I2C_ADDR_PCF85063, "PCF85063 RTC", false},
    };
    report->missing = 0;
    for (size_t i = 0; i < ZV_BOARD_I2C_EXPECTED_COUNT; i++) {
        report->entries[i] = expected[i];
        esp_err_t err = i2c_master_probe(s_i2c_bus, expected[i].address, PROBE_TIMEOUT_MS);
        report->entries[i].present = (err == ESP_OK);
        if (err != ESP_OK) {
            report->missing++;
            ESP_LOGW(TAG, "I2C 0x%02x (%s) not found: %s", expected[i].address, expected[i].name, esp_err_to_name(err));
        }
    }
    return ESP_OK;
}

esp_err_t zv_board_memory_report(zv_board_memory_report_t *report)
{
    ESP_RETURN_ON_FALSE(report != NULL, ESP_ERR_INVALID_ARG, TAG, "null report");
    report->psram_bytes = esp_psram_is_initialized() ? esp_psram_get_size() : 0;
    ESP_RETURN_ON_ERROR(esp_flash_get_size(NULL, &report->flash_bytes), TAG, "flash size query failed");
    report->internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    report->internal_min_free = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    report->psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    report->dma_largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    report->psram_matches_rev2 = report->psram_bytes >= (8u * 1024u * 1024u);
    report->flash_matches_rev2 = report->flash_bytes == (16u * 1024u * 1024u);
    return ESP_OK;
}


/*
 * Complete byte-for-byte command/data/delay sequence copied from Waveshare
 * vendor_specific_init_new[] in
 * ESP-IDF/ESP32-S3-Touch-LCD-1.85C-Test/main/LCD_Driver/ST77916.c at revision
 * 8ead4a96bf3a278fc4ebd8ef4768657e17fa2880. The transport opcodes, reset
 * timing, and backlight curve are also copied from that source.
 *
 * Upstream applies this table only when the panel identifies as the
 * 00 02 7F 7F variant; the other variant uses the driver's own
 * vendor_specific_init_default. lcd_init() reproduces that choice.
 */
static const st77916_lcd_init_cmd_t LCD_INIT[] = {
  {0xF0, (const uint8_t[]){0x28}, 1, 0},
  {0xF2, (const uint8_t[]){0x28}, 1, 0},
  {0x73, (const uint8_t[]){0xF0}, 1, 0},
  {0x7C, (const uint8_t[]){0xD1}, 1, 0},
  {0x83, (const uint8_t[]){0xE0}, 1, 0},
  {0x84, (const uint8_t[]){0x61}, 1, 0},
  {0xF2, (const uint8_t[]){0x82}, 1, 0},
  {0xF0, (const uint8_t[]){0x00}, 1, 0},
  {0xF0, (const uint8_t[]){0x01}, 1, 0},
  {0xF1, (const uint8_t[]){0x01}, 1, 0},
  {0xB0, (const uint8_t[]){0x56}, 1, 0},
  {0xB1, (const uint8_t[]){0x4D}, 1, 0},
  {0xB2, (const uint8_t[]){0x24}, 1, 0},
  {0xB4, (const uint8_t[]){0x87}, 1, 0},
  {0xB5, (const uint8_t[]){0x44}, 1, 0},
  {0xB6, (const uint8_t[]){0x8B}, 1, 0},
  {0xB7, (const uint8_t[]){0x40}, 1, 0},
  {0xB8, (const uint8_t[]){0x86}, 1, 0},
  {0xBA, (const uint8_t[]){0x00}, 1, 0},
  {0xBB, (const uint8_t[]){0x08}, 1, 0},
  {0xBC, (const uint8_t[]){0x08}, 1, 0},
  {0xBD, (const uint8_t[]){0x00}, 1, 0},
  {0xC0, (const uint8_t[]){0x80}, 1, 0},
  {0xC1, (const uint8_t[]){0x10}, 1, 0},
  {0xC2, (const uint8_t[]){0x37}, 1, 0},
  {0xC3, (const uint8_t[]){0x80}, 1, 0},
  {0xC4, (const uint8_t[]){0x10}, 1, 0},
  {0xC5, (const uint8_t[]){0x37}, 1, 0},
  {0xC6, (const uint8_t[]){0xA9}, 1, 0},
  {0xC7, (const uint8_t[]){0x41}, 1, 0},
  {0xC8, (const uint8_t[]){0x01}, 1, 0},
  {0xC9, (const uint8_t[]){0xA9}, 1, 0},
  {0xCA, (const uint8_t[]){0x41}, 1, 0},
  {0xCB, (const uint8_t[]){0x01}, 1, 0},
  {0xD0, (const uint8_t[]){0x91}, 1, 0},
  {0xD1, (const uint8_t[]){0x68}, 1, 0},
  {0xD2, (const uint8_t[]){0x68}, 1, 0},
  {0xF5, (const uint8_t[]){0x00, 0xA5}, 2, 0},
  {0xDD, (const uint8_t[]){0x4F}, 1, 0},
  {0xDE, (const uint8_t[]){0x4F}, 1, 0},
  {0xF1, (const uint8_t[]){0x10}, 1, 0},
  {0xF0, (const uint8_t[]){0x00}, 1, 0},
  {0xF0, (const uint8_t[]){0x02}, 1, 0},
  {0xE0, (const uint8_t[]){0xF0, 0x0A, 0x10, 0x09, 0x09, 0x36, 0x35, 0x33, 0x4A, 0x29, 0x15, 0x15, 0x2E, 0x34}, 14, 0},
  {0xE1, (const uint8_t[]){0xF0, 0x0A, 0x0F, 0x08, 0x08, 0x05, 0x34, 0x33, 0x4A, 0x39, 0x15, 0x15, 0x2D, 0x33}, 14, 0},
  {0xF0, (const uint8_t[]){0x10}, 1, 0},
  {0xF3, (const uint8_t[]){0x10}, 1, 0},
  {0xE0, (const uint8_t[]){0x07}, 1, 0},
  {0xE1, (const uint8_t[]){0x00}, 1, 0},
  {0xE2, (const uint8_t[]){0x00}, 1, 0},
  {0xE3, (const uint8_t[]){0x00}, 1, 0},
  {0xE4, (const uint8_t[]){0xE0}, 1, 0},
  {0xE5, (const uint8_t[]){0x06}, 1, 0},
  {0xE6, (const uint8_t[]){0x21}, 1, 0},
  {0xE7, (const uint8_t[]){0x01}, 1, 0},
  {0xE8, (const uint8_t[]){0x05}, 1, 0},
  {0xE9, (const uint8_t[]){0x02}, 1, 0},
  {0xEA, (const uint8_t[]){0xDA}, 1, 0},
  {0xEB, (const uint8_t[]){0x00}, 1, 0},
  {0xEC, (const uint8_t[]){0x00}, 1, 0},
  {0xED, (const uint8_t[]){0x0F}, 1, 0},
  {0xEE, (const uint8_t[]){0x00}, 1, 0},
  {0xEF, (const uint8_t[]){0x00}, 1, 0},
  {0xF8, (const uint8_t[]){0x00}, 1, 0},
  {0xF9, (const uint8_t[]){0x00}, 1, 0},
  {0xFA, (const uint8_t[]){0x00}, 1, 0},
  {0xFB, (const uint8_t[]){0x00}, 1, 0},
  {0xFC, (const uint8_t[]){0x00}, 1, 0},
  {0xFD, (const uint8_t[]){0x00}, 1, 0},
  {0xFE, (const uint8_t[]){0x00}, 1, 0},
  {0xFF, (const uint8_t[]){0x00}, 1, 0},
  {0x60, (const uint8_t[]){0x40}, 1, 0},
  {0x61, (const uint8_t[]){0x04}, 1, 0},
  {0x62, (const uint8_t[]){0x00}, 1, 0},
  {0x63, (const uint8_t[]){0x42}, 1, 0},
  {0x64, (const uint8_t[]){0xD9}, 1, 0},
  {0x65, (const uint8_t[]){0x00}, 1, 0},
  {0x66, (const uint8_t[]){0x00}, 1, 0},
  {0x67, (const uint8_t[]){0x00}, 1, 0},
  {0x68, (const uint8_t[]){0x00}, 1, 0},
  {0x69, (const uint8_t[]){0x00}, 1, 0},
  {0x6A, (const uint8_t[]){0x00}, 1, 0},
  {0x6B, (const uint8_t[]){0x00}, 1, 0},
  {0x70, (const uint8_t[]){0x40}, 1, 0},
  {0x71, (const uint8_t[]){0x03}, 1, 0},
  {0x72, (const uint8_t[]){0x00}, 1, 0},
  {0x73, (const uint8_t[]){0x42}, 1, 0},
  {0x74, (const uint8_t[]){0xD8}, 1, 0},
  {0x75, (const uint8_t[]){0x00}, 1, 0},
  {0x76, (const uint8_t[]){0x00}, 1, 0},
  {0x77, (const uint8_t[]){0x00}, 1, 0},
  {0x78, (const uint8_t[]){0x00}, 1, 0},
  {0x79, (const uint8_t[]){0x00}, 1, 0},
  {0x7A, (const uint8_t[]){0x00}, 1, 0},
  {0x7B, (const uint8_t[]){0x00}, 1, 0},
  {0x80, (const uint8_t[]){0x48}, 1, 0},
  {0x81, (const uint8_t[]){0x00}, 1, 0},
  {0x82, (const uint8_t[]){0x06}, 1, 0},
  {0x83, (const uint8_t[]){0x02}, 1, 0},
  {0x84, (const uint8_t[]){0xD6}, 1, 0},
  {0x85, (const uint8_t[]){0x04}, 1, 0},
  {0x86, (const uint8_t[]){0x00}, 1, 0},
  {0x87, (const uint8_t[]){0x00}, 1, 0},
  {0x88, (const uint8_t[]){0x48}, 1, 0},
  {0x89, (const uint8_t[]){0x00}, 1, 0},
  {0x8A, (const uint8_t[]){0x08}, 1, 0},
  {0x8B, (const uint8_t[]){0x02}, 1, 0},
  {0x8C, (const uint8_t[]){0xD8}, 1, 0},
  {0x8D, (const uint8_t[]){0x04}, 1, 0},
  {0x8E, (const uint8_t[]){0x00}, 1, 0},
  {0x8F, (const uint8_t[]){0x00}, 1, 0},
  {0x90, (const uint8_t[]){0x48}, 1, 0},
  {0x91, (const uint8_t[]){0x00}, 1, 0},
  {0x92, (const uint8_t[]){0x0A}, 1, 0},
  {0x93, (const uint8_t[]){0x02}, 1, 0},
  {0x94, (const uint8_t[]){0xDA}, 1, 0},
  {0x95, (const uint8_t[]){0x04}, 1, 0},
  {0x96, (const uint8_t[]){0x00}, 1, 0},
  {0x97, (const uint8_t[]){0x00}, 1, 0},
  {0x98, (const uint8_t[]){0x48}, 1, 0},
  {0x99, (const uint8_t[]){0x00}, 1, 0},
  {0x9A, (const uint8_t[]){0x0C}, 1, 0},
  {0x9B, (const uint8_t[]){0x02}, 1, 0},
  {0x9C, (const uint8_t[]){0xDC}, 1, 0},
  {0x9D, (const uint8_t[]){0x04}, 1, 0},
  {0x9E, (const uint8_t[]){0x00}, 1, 0},
  {0x9F, (const uint8_t[]){0x00}, 1, 0},
  {0xA0, (const uint8_t[]){0x48}, 1, 0},
  {0xA1, (const uint8_t[]){0x00}, 1, 0},
  {0xA2, (const uint8_t[]){0x05}, 1, 0},
  {0xA3, (const uint8_t[]){0x02}, 1, 0},
  {0xA4, (const uint8_t[]){0xD5}, 1, 0},
  {0xA5, (const uint8_t[]){0x04}, 1, 0},
  {0xA6, (const uint8_t[]){0x00}, 1, 0},
  {0xA7, (const uint8_t[]){0x00}, 1, 0},
  {0xA8, (const uint8_t[]){0x48}, 1, 0},
  {0xA9, (const uint8_t[]){0x00}, 1, 0},
  {0xAA, (const uint8_t[]){0x07}, 1, 0},
  {0xAB, (const uint8_t[]){0x02}, 1, 0},
  {0xAC, (const uint8_t[]){0xD7}, 1, 0},
  {0xAD, (const uint8_t[]){0x04}, 1, 0},
  {0xAE, (const uint8_t[]){0x00}, 1, 0},
  {0xAF, (const uint8_t[]){0x00}, 1, 0},
  {0xB0, (const uint8_t[]){0x48}, 1, 0},
  {0xB1, (const uint8_t[]){0x00}, 1, 0},
  {0xB2, (const uint8_t[]){0x09}, 1, 0},
  {0xB3, (const uint8_t[]){0x02}, 1, 0},
  {0xB4, (const uint8_t[]){0xD9}, 1, 0},
  {0xB5, (const uint8_t[]){0x04}, 1, 0},
  {0xB6, (const uint8_t[]){0x00}, 1, 0},
  {0xB7, (const uint8_t[]){0x00}, 1, 0},

  {0xB8, (const uint8_t[]){0x48}, 1, 0},
  {0xB9, (const uint8_t[]){0x00}, 1, 0},
  {0xBA, (const uint8_t[]){0x0B}, 1, 0},
  {0xBB, (const uint8_t[]){0x02}, 1, 0},
  {0xBC, (const uint8_t[]){0xDB}, 1, 0},
  {0xBD, (const uint8_t[]){0x04}, 1, 0},
  {0xBE, (const uint8_t[]){0x00}, 1, 0},
  {0xBF, (const uint8_t[]){0x00}, 1, 0},
  {0xC0, (const uint8_t[]){0x10}, 1, 0},
  {0xC1, (const uint8_t[]){0x47}, 1, 0},
  {0xC2, (const uint8_t[]){0x56}, 1, 0},
  {0xC3, (const uint8_t[]){0x65}, 1, 0},
  {0xC4, (const uint8_t[]){0x74}, 1, 0},
  {0xC5, (const uint8_t[]){0x88}, 1, 0},
  {0xC6, (const uint8_t[]){0x99}, 1, 0},
  {0xC7, (const uint8_t[]){0x01}, 1, 0},
  {0xC8, (const uint8_t[]){0xBB}, 1, 0},
  {0xC9, (const uint8_t[]){0xAA}, 1, 0},
  {0xD0, (const uint8_t[]){0x10}, 1, 0},
  {0xD1, (const uint8_t[]){0x47}, 1, 0},
  {0xD2, (const uint8_t[]){0x56}, 1, 0},
  {0xD3, (const uint8_t[]){0x65}, 1, 0},
  {0xD4, (const uint8_t[]){0x74}, 1, 0},
  {0xD5, (const uint8_t[]){0x88}, 1, 0},
  {0xD6, (const uint8_t[]){0x99}, 1, 0},
  {0xD7, (const uint8_t[]){0x01}, 1, 0},
  {0xD8, (const uint8_t[]){0xBB}, 1, 0},
  {0xD9, (const uint8_t[]){0xAA}, 1, 0},
  {0xF3, (const uint8_t[]){0x01}, 1, 0},
  {0xF0, (const uint8_t[]){0x00}, 1, 0},
  {0x21, (const uint8_t[]){0x00}, 1, 0},
  {0x11, (const uint8_t[]){0x00}, 1, 120},
  {0x29, (const uint8_t[]){0x00}, 1, 0},
};

/*
 * esp_lcd_panel_draw_bitmap() queues the colour transfer and returns before DMA
 * has read the buffer. Without waiting for this callback, a caller that reuses
 * or frees its fill buffer corrupts the transfer still in flight, which paints
 * part of a rectangle in the wrong colour.
 */
static bool lcd_color_trans_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *event, void *ctx)
{
    BaseType_t higher_priority_woken = pdFALSE;

    (void)io;
    (void)event;
    (void)ctx;
    if (s_lcd_trans_done != NULL) {
        xSemaphoreGiveFromISR(s_lcd_trans_done, &higher_priority_woken);
    }
    return higher_priority_woken == pdTRUE;
}

static esp_err_t lcd_panel_io_new(uint32_t pclk_hz, esp_lcd_panel_io_handle_t *out)
{
    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .on_color_trans_done = lcd_color_trans_done,
        .cs_gpio_num = ZV_PIN_LCD_QSPI_CS,
        .dc_gpio_num = -1,
        .spi_mode = 0,
        .pclk_hz = pclk_hz,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 32,
        .lcd_param_bits = 8,
        .flags.quad_mode = 1,
    };
    return esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, out);
}

/*
 * Upstream ST77916.c reads register 0x04 over a temporary 3 MHz IO handle and
 * deletes it before creating the real one, then picks the init table from the
 * four bytes it read. Reproduced here: applying the wrong table is silent, the
 * panel simply never renders.
 */
static esp_err_t lcd_read_id(uint8_t id[4])
{
    esp_lcd_panel_io_handle_t io = NULL;

    ESP_RETURN_ON_ERROR(lcd_panel_io_new(ZV_LCD_ID_READ_PCLK_HZ, &io), TAG, "LCD id IO init failed");
    int cmd = (int)(((uint32_t)LCD_OPCODE_READ_CMD << 24) | ((uint32_t)LCD_ID_REG << 8));
    esp_err_t err = esp_lcd_panel_io_rx_param(io, cmd, id, 4);
    esp_err_t del_err = esp_lcd_panel_io_del(io);

    if (err != ESP_OK) {
        return err;
    }
    return del_err;
}

static esp_err_t lcd_init(void)
{
    if (s_lcd_ready) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(zv_board_exio_pulse_reset(ZV_EXIO_LCD_RESET, 10, 50), TAG, "LCD EXIO reset failed");

    if (s_lcd_trans_done == NULL) {
        s_lcd_trans_done = xSemaphoreCreateBinary();
        ESP_RETURN_ON_FALSE(s_lcd_trans_done != NULL, ESP_ERR_NO_MEM, TAG, "LCD transfer semaphore failed");
    }

    const spi_bus_config_t bus_cfg = {
        .sclk_io_num = ZV_PIN_LCD_QSPI_SCK,
        .data0_io_num = ZV_PIN_LCD_QSPI_D0,
        .data1_io_num = ZV_PIN_LCD_QSPI_D1,
        .data2_io_num = ZV_PIN_LCD_QSPI_D2,
        .data3_io_num = ZV_PIN_LCD_QSPI_D3,
        .data4_io_num = -1,
        .data5_io_num = -1,
        .data6_io_num = -1,
        .data7_io_num = -1,
        .max_transfer_sz = LCD_WIDTH * (int)LCD_FILL_LINES * (int)sizeof(uint16_t),
        .flags = SPICOMMON_BUSFLAG_MASTER,
    };
    esp_err_t err = spi_bus_initialize(LCD_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    ESP_RETURN_ON_FALSE(err == ESP_OK || err == ESP_ERR_INVALID_STATE, err, TAG, "QSPI bus init failed");

    uint8_t id[4] = {0};
    ESP_RETURN_ON_ERROR(lcd_read_id(id), TAG, "LCD id read failed");

    static st77916_vendor_config_t vendor_cfg;
    vendor_cfg = (st77916_vendor_config_t){.flags = {.use_qspi_interface = 1}};

    bool custom_table = (id[0] == 0x00 && id[1] == 0x02 && id[2] == 0x7F && id[3] == 0x7F);
    if (custom_table) {
        vendor_cfg.init_cmds = LCD_INIT;
        vendor_cfg.init_cmds_size = sizeof(LCD_INIT) / sizeof(LCD_INIT[0]);
    }
    ESP_LOGI(TAG, "ST77916 id=%02x %02x %02x %02x -> %s init table", id[0], id[1], id[2], id[3],
             custom_table ? "pinned upstream" : "driver default");

    ESP_RETURN_ON_ERROR(lcd_panel_io_new(ZV_LCD_PCLK_HZ, &s_lcd_io), TAG, "LCD panel IO init failed");

    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = -1, /* Reset is EXIO2 on the TCA9554, not an MCU GPIO */
        /*
         * Rev2 acceptance uses native RGB565. The previous BGR override swaps
         * red/blue at MADCTL and is contradicted by the physical color gate.
         */
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = LCD_BITS_PER_PIXEL,
        .flags = {.reset_active_high = 0},
        .vendor_config = (void *)&vendor_cfg,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st77916(s_lcd_io, &panel_cfg, &s_lcd_panel), TAG,
                        "ST77916 panel create failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_lcd_panel), TAG, "ST77916 reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_lcd_panel), TAG, "ST77916 init failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_lcd_panel, true), TAG, "ST77916 display on failed");

    s_lcd_ready = true;
    return ESP_OK;
}

/*
 * Fills through the panel driver, in the same 16-line chunks upstream
 * lcd_clear_screen() uses. The previous hand-rolled version re-issued RAMWR for
 * every row, which restarts each write at the window origin instead of
 * continuing it, so it could not paint a band even when the transport worked.
 */
esp_err_t zv_board_lcd_fill_rect(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint16_t color)
{
    ESP_RETURN_ON_FALSE(!s_sprite_dma_fault, ESP_ERR_INVALID_STATE, TAG, "LCD sprite DMA fault; restart required");
    ESP_RETURN_ON_FALSE(s_lcd_panel != NULL && s_lcd_trans_done != NULL,
                        ESP_ERR_INVALID_STATE, TAG, "LCD panel not initialized");
    if (x0 >= LCD_WIDTH || y0 >= LCD_HEIGHT) {
        return ESP_OK;
    }
    if (x1 > LCD_WIDTH) {
        x1 = LCD_WIDTH;
    }
    if (y1 > LCD_HEIGHT) {
        y1 = LCD_HEIGHT;
    }
    ESP_RETURN_ON_FALSE(x1 > x0 && y1 > y0, ESP_ERR_INVALID_ARG, TAG, "empty fill rectangle");

    size_t width = (size_t)(x1 - x0);
    size_t pixels = width * LCD_FILL_LINES;
    uint16_t *buffer = heap_caps_malloc(pixels * sizeof(uint16_t), MALLOC_CAP_DMA);
    ESP_RETURN_ON_FALSE(buffer != NULL, ESP_ERR_NO_MEM, TAG, "LCD fill buffer allocation failed");

    /*
     * The ST77916 QSPI driver passes color_data directly to the SPI transfer.
     * RGB565 words are stored little-endian in ESP32 memory, while this panel
     * consumes the high byte first. Swap the bytes at this shared board boundary
     * so calibration colors and every UI fill use the same panel wire format.
     */
    uint16_t wire_color = (uint16_t)((color << 8) | (color >> 8));
    for (size_t i = 0; i < pixels; i++) {
        buffer[i] = wire_color;
    }

    esp_err_t err = ESP_OK;
    for (uint16_t y = y0; y < y1; y += (uint16_t)LCD_FILL_LINES) {
        uint16_t y_end = (uint16_t)(y + LCD_FILL_LINES);
        if (y_end > y1) {
            y_end = y1;
        }
        err = esp_lcd_panel_draw_bitmap(s_lcd_panel, x0, y, x1, y_end, buffer);
        if (err != ESP_OK) {
            break;
        }
        /* The buffer is reused by the next chunk and freed below, so the
           in-flight transfer has to finish before either happens. */
        if (xSemaphoreTake(s_lcd_trans_done, pdMS_TO_TICKS(1000)) != pdTRUE) {
            /* DMA may still read this allocation. Match row/patch failure
             * handling: retain it until restart and reject all further writes.
             * At most one fill buffer is retained because the fault is latched. */
            s_sprite_dma_fault = true;
            return ESP_ERR_TIMEOUT;
        }
    }

    free(buffer);
    return err;
}


esp_err_t zv_board_lcd_draw_row(uint16_t x, uint16_t y, uint16_t width, const uint16_t *pixels)
{
    ESP_RETURN_ON_FALSE(s_lcd_panel != NULL && s_lcd_trans_done != NULL && !s_sprite_dma_fault,
                        ESP_ERR_INVALID_STATE, TAG, "LCD not ready for sprite row");
    ESP_RETURN_ON_FALSE(pixels != NULL && width > 0u && (uint32_t)x + width <= LCD_WIDTH && y < LCD_HEIGHT,
                        ESP_ERR_INVALID_ARG, TAG, "invalid sprite row");
    if (s_sprite_dma_row == NULL) {
        s_sprite_dma_row = heap_caps_malloc(LCD_WIDTH * sizeof(uint16_t), MALLOC_CAP_DMA);
        ESP_RETURN_ON_FALSE(s_sprite_dma_row != NULL, ESP_ERR_NO_MEM, TAG, "sprite DMA row allocation failed");
    }
    for (uint16_t i = 0u; i < width; i++)
        s_sprite_dma_row[i] = (uint16_t)((pixels[i] << 8) | (pixels[i] >> 8));
    esp_err_t err = esp_lcd_panel_draw_bitmap(s_lcd_panel, x, y, (int)(x + width), (int)(y + 1u), s_sprite_dma_row);
    if (err != ESP_OK) return err;
    if (xSemaphoreTake(s_lcd_trans_done, pdMS_TO_TICKS(1000)) != pdTRUE) {
        /* Keep the in-flight DMA storage alive and refuse further LCD writes. */
        s_sprite_dma_fault = true;
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t zv_board_lcd_draw_patch(uint16_t x, uint16_t y, uint16_t width, uint16_t height,
                                const uint16_t *pixels)
{
    enum { PATCH_PIXELS = 240 * 14 };
    static uint16_t *dma_patch;
    ESP_RETURN_ON_FALSE(s_lcd_panel != NULL && s_lcd_trans_done != NULL && !s_sprite_dma_fault,
                        ESP_ERR_INVALID_STATE, TAG, "LCD not ready for patch");
    size_t count = (size_t)width * height;
    ESP_RETURN_ON_FALSE(pixels != NULL && width > 0u && height > 0u &&
                        (uint32_t)x + width <= LCD_WIDTH && (uint32_t)y + height <= LCD_HEIGHT &&
                        count <= PATCH_PIXELS,
                        ESP_ERR_INVALID_ARG, TAG, "invalid LCD patch");
    if (dma_patch == NULL) {
        dma_patch = heap_caps_malloc(PATCH_PIXELS * sizeof(uint16_t), MALLOC_CAP_DMA);
        ESP_RETURN_ON_FALSE(dma_patch != NULL, ESP_ERR_NO_MEM, TAG, "LCD patch allocation failed");
    }
    for (size_t i = 0; i < count; ++i)
        dma_patch[i] = (uint16_t)((pixels[i] << 8) | (pixels[i] >> 8));
    esp_err_t err = esp_lcd_panel_draw_bitmap(s_lcd_panel, x, y, x + width, y + height, dma_patch);
    if (err != ESP_OK) return err;
    if (xSemaphoreTake(s_lcd_trans_done, pdMS_TO_TICKS(1000)) != pdTRUE) {
        /* DMA may still own storage: retain it and refuse all subsequent writes. */
        s_sprite_dma_fault = true;
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t zv_board_lcd_init(void)
{
    return lcd_init();
}

esp_err_t zv_board_touch_init(void)
{
    return touch_init();
}

esp_err_t zv_board_touch_poll(bool *touched, uint16_t *x, uint16_t *y)
{
    ESP_RETURN_ON_FALSE(touched != NULL, ESP_ERR_INVALID_ARG, TAG, "null touched");
    return touch_poll_once(touched, x, y);
}

void zv_board_touch_wait(uint32_t timeout_ms)
{
    if (s_touch_irq_ready) (void)xSemaphoreTake(s_touch_wake, pdMS_TO_TICKS(timeout_ms));
    else vTaskDelay(pdMS_TO_TICKS(timeout_ms));
}

esp_err_t zv_board_touch_wait_release(uint32_t timeout_ms)
{
#if CONFIG_ZV_M0_DISPLAY_CONFIRM_ROUNDS > 0
    return touch_wait_release(timeout_ms);
#else
    (void)timeout_ms;
    return ESP_OK;
#endif
}

static esp_err_t touch_init(void)
{
    if (s_touch_dev == NULL) {
        const i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = ZV_I2C_ADDR_CST816,
            .scl_speed_hz = 400000,
        };
        ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_touch_dev), TAG, "CST816 add failed");
    }
    ESP_RETURN_ON_ERROR(zv_board_exio_pulse_reset(ZV_EXIO_TOUCH_RESET, 10, 50), TAG, "touch EXIO reset failed");
    uint8_t reg = TOUCH_CHIP_ID_REG;
    uint8_t id = 0;
    ESP_RETURN_ON_ERROR(i2c_master_transmit_receive(s_touch_dev, &reg, 1, &id, 1, 100), TAG, "touch id read failed");
    uint8_t autosleep_off = 1;
    uint8_t frame[2] = {TOUCH_AUTOSLEEP_REG, autosleep_off};
    ESP_RETURN_ON_ERROR(i2c_master_transmit(s_touch_dev, frame, sizeof(frame), 100), TAG, "touch autosleep write failed");
    touch_wake_init();
    ESP_LOGI(TAG, "CST816 id=0x%02x, touch wake IRQ=%s", id, s_touch_irq_ready ? "ready" : "fallback");
    return ESP_OK;
}

static esp_err_t touch_poll_once(bool *touched, uint16_t *out_x, uint16_t *out_y)
{
    uint8_t reg = TOUCH_DATA_START_REG;
    uint8_t point[5] = {0};
    ESP_RETURN_ON_ERROR(i2c_master_transmit_receive(s_touch_dev, &reg, 1, point, sizeof(point), 100), TAG,
                        "touch data read failed");
    uint8_t count = point[0] > 1 ? 1 : point[0];
    *touched = count > 0;
    if (*touched) {
        uint16_t x = ((uint16_t)(point[1] & 0x0f) << 8) | point[2];
        uint16_t y = ((uint16_t)(point[3] & 0x0f) << 8) | point[4];
        if (out_x != NULL) {
            *out_x = x;
        }
        if (out_y != NULL) {
            *out_y = y;
        }
    }
    return ESP_OK;
}

bool zv_board_m0_display_confirmed(void)
{
    return s_display_confirmed;
}

#if CONFIG_ZV_M0_DISPLAY_CONFIRM_ROUNDS > 0
static esp_err_t touch_wait_release(uint32_t timeout_ms)
{
    for (uint32_t waited = 0; waited < timeout_ms; waited += M0_POLL_MS) {
        bool touched = false;
        ESP_RETURN_ON_ERROR(touch_poll_once(&touched, NULL, NULL), TAG, "touch poll failed");
        if (!touched) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(M0_POLL_MS));
    }
    return ESP_ERR_TIMEOUT;
}

/*
 * One positional display challenge.
 *
 * A white marker is painted in a randomly chosen cell of a 3x3 grid and the
 * operator must touch that cell. The target is deliberately not written to the
 * log until after the touch has been read, so an operator watching this serial
 * output cannot answer a round without seeing the panel. That is what separates
 * this from the transport-only check it replaces: returning ESP_OK from a QSPI
 * write proves nothing about whether any pixel lit.
 *
 * A mismatch is reported with both cells so a systematic axis flip or rotation
 * is legible from the log rather than looking like a dead panel.
 */
static esp_err_t display_confirm_round(unsigned round_index, bool *passed)
{
    *passed = false;

    /*
     * Rounds must be independent presses. Without this, a finger left down or
     * dragged across the panel answers the next round the instant it is drawn,
     * so the second round contributes no evidence and a two-round guess drops
     * from 1 in 81 to 1 in 9.
     */
    if (touch_wait_release(CONFIG_ZV_M0_DISPLAY_RELEASE_TIMEOUT_MS) != ESP_OK) {
        ESP_LOGE(TAG, "M0 display challenge round %u: previous touch never released; each round needs its own "
                      "separate press",
                 round_index + 1u);
        return ESP_ERR_INVALID_STATE;
    }
    vTaskDelay(pdMS_TO_TICKS(M0_DEBOUNCE_MS));

    uint32_t target = esp_random() % (M0_GRID * M0_GRID);
    uint16_t cell_x = (uint16_t)((target % M0_GRID) * (uint32_t)M0_CELL);
    uint16_t cell_y = (uint16_t)((target / M0_GRID) * (uint32_t)M0_CELL);

    ESP_RETURN_ON_ERROR(zv_board_lcd_fill_rect(0, 0, LCD_WIDTH, LCD_HEIGHT, 0x0000), TAG, "challenge clear failed");
    ESP_RETURN_ON_ERROR(zv_board_lcd_fill_rect((uint16_t)(cell_x + M0_MARK_INSET), (uint16_t)(cell_y + M0_MARK_INSET),
                                      (uint16_t)(cell_x + M0_CELL - M0_MARK_INSET),
                                      (uint16_t)(cell_y + M0_CELL - M0_MARK_INSET), 0xFFFF),
                        TAG, "challenge marker failed");

    ESP_LOGW(TAG, "M0 display challenge round %u of %d: touch the white square", round_index + 1u,
             CONFIG_ZV_M0_DISPLAY_CONFIRM_ROUNDS);

    for (uint32_t waited = 0; waited < (uint32_t)CONFIG_ZV_M0_DISPLAY_CONFIRM_TIMEOUT_MS; waited += M0_POLL_MS) {
        bool touched = false;
        uint16_t x = 0;
        uint16_t y = 0;

        ESP_RETURN_ON_ERROR(touch_poll_once(&touched, &x, &y), TAG, "touch poll failed");
        if (touched) {
            uint32_t hit = (uint32_t)(M0_GRID * M0_GRID); /* out of range unless both axes land */
            if (x < LCD_WIDTH && y < LCD_HEIGHT) {
                hit = ((uint32_t)(y / M0_CELL) * M0_GRID) + (uint32_t)(x / M0_CELL);
            }
            *passed = (hit == target);
            ESP_LOGI(TAG, "M0 display challenge round %u: target cell %" PRIu32 ", touched cell %" PRIu32
                          " at x=%u y=%u -> %s",
                     round_index + 1u, target, hit, x, y, *passed ? "match" : "MISMATCH");
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(M0_POLL_MS));
    }

    ESP_LOGE(TAG, "M0 display challenge round %u: no touch within %d ms (target cell was %" PRIu32 ")",
             round_index + 1u, CONFIG_ZV_M0_DISPLAY_CONFIRM_TIMEOUT_MS, target);
    return ESP_ERR_TIMEOUT;
}
#endif /* CONFIG_ZV_M0_DISPLAY_CONFIRM_ROUNDS > 0 */

esp_err_t zv_board_voice_ui_init(void)
{
    /* Transport/controller checks remain mandatory. Human display acceptance
     * is deliberately not claimed by normal unattended startup. */
    ESP_RETURN_ON_ERROR(zv_board_init(), TAG, "board init failed");
    ESP_RETURN_ON_ERROR(zv_board_backlight_init(), TAG, "backlight init failed");
    ESP_RETURN_ON_ERROR(lcd_init(), TAG, "LCD init failed");
    ESP_RETURN_ON_ERROR(touch_init(), TAG, "touch init failed");
    return zv_board_backlight_set(35);
}

esp_err_t zv_board_m0_acceptance_demo(void)
{
    ESP_LOGW(TAG, "M0 acceptance mode enabled: display/backlight/touch only; all other gates remain closed");
    ESP_RETURN_ON_ERROR(zv_board_init(), TAG, "board init failed");
    ESP_RETURN_ON_ERROR(zv_board_backlight_init(), TAG, "backlight init failed");
    ESP_RETURN_ON_ERROR(lcd_init(), TAG, "LCD init failed");
    ESP_RETURN_ON_ERROR(touch_init(), TAG, "touch init failed");

    ESP_RETURN_ON_ERROR(zv_board_lcd_fill_rect(0, 0, LCD_WIDTH, 120, 0x07E0), TAG, "green band failed");
    ESP_RETURN_ON_ERROR(zv_board_lcd_fill_rect(0, 120, LCD_WIDTH, 240, 0x001F), TAG, "blue band failed");
    ESP_RETURN_ON_ERROR(zv_board_lcd_fill_rect(0, 240, LCD_WIDTH, LCD_HEIGHT, 0xF800), TAG, "red band failed");
    ESP_RETURN_ON_ERROR(zv_board_backlight_set(80), TAG, "backlight 80 failed");

#if CONFIG_ZV_M0_DISPLAY_CONFIRM_ROUNDS > 0
    vTaskDelay(pdMS_TO_TICKS(1500)); /* let the operator see the bands before the first challenge */

    for (unsigned round = 0; round < (unsigned)CONFIG_ZV_M0_DISPLAY_CONFIRM_ROUNDS; round++) {
        bool passed = false;
        esp_err_t err = display_confirm_round(round, &passed);

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "M0 acceptance failed: display challenge round %u did not complete", round + 1u);
            return err;
        }
        if (!passed) {
            ESP_LOGE(TAG, "M0 acceptance failed: display challenge answered incorrectly. The panel is blank, "
                          "stale, or its axes do not match the touch controller.");
            return ESP_ERR_INVALID_RESPONSE;
        }
    }

    ESP_RETURN_ON_ERROR(zv_board_backlight_set(35), TAG, "confirm feedback backlight failed");
    s_display_confirmed = true;
    ESP_LOGI(TAG, "M0 demo complete; touch_observed=yes; display_rendering=confirmed over %d positional round(s)",
             CONFIG_ZV_M0_DISPLAY_CONFIRM_ROUNDS);
    return ESP_OK;
#else
    ESP_LOGW(TAG, "M0 machine-verifies touch only. Display and backlight writes are checked for transport "
                  "errors, which does NOT prove pixels were lit; confirm both visually before accepting M0.");

    bool saw_touch = false;
    for (int i = 0; i < 120; i++) {
        bool touched = false;
        ESP_RETURN_ON_ERROR(touch_poll_once(&touched, NULL, NULL), TAG, "touch poll failed");
        if (touched && !saw_touch) {
            saw_touch = true;
            ESP_RETURN_ON_ERROR(zv_board_backlight_set(35), TAG, "touch feedback backlight failed");
            ESP_RETURN_ON_ERROR(zv_board_lcd_fill_rect(110, 110, 250, 250, 0xFFFF), TAG, "touch feedback draw failed");
            ESP_LOGI(TAG, "M0 touch feedback rendered");
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    if (!saw_touch) {
        ESP_LOGE(TAG, "M0 acceptance failed: no touch observed within 30 seconds");
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG, "M0 demo complete; touch_observed=yes; display_rendering=unverified");
    ESP_LOGW(TAG, "Operator must confirm visually: colour bands at start, white square and dimmed backlight "
                  "after touch. A blank or stale panel still reaches this line.");
    return ESP_OK;
#endif
}
