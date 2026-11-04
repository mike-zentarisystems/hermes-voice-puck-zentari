/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 */
#include "zv_audio.h"

#include <string.h>

#include "sdkconfig.h"

#if CONFIG_ZV_M1_ACCEPTANCE_MODE

#include <inttypes.h>
#include <stdlib.h>

#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "zv_audio_probe.h"
#include "zv_board.h"
#include "zv_board_pins.h"

static const char *TAG = "zv_audio";

/*
 * Pinned upstream audio configuration, copied from
 * components/auido_borad/boards/ESP32_S3_AUDIO_Board/bsp_board.c and
 * components/auido_borad/boards/include/ESP32_S3_AUDIO_Board.h at revision
 * 8ead4a96bf3a278fc4ebd8ef4768657e17fa2880:
 *
 *   bsp_board_init()   -> bsp_i2s_init(I2S_NUM_1, 16000, 2, 32)
 *                         bsp_codec_init(16000, 16000, 2, 32)
 *   I2S_CONFIG_DEFAULT -> I2S_STD_CLK_DEFAULT_CONFIG(16000)
 *                         I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(32, I2S_SLOT_MODE_STEREO)
 *   gpio_cfg           -> mclk 2, bclk 48, ws 38, dout 47, din 39
 *   RECORD_VOLUME      -> 30.0
 *   ADC_I2S_CHANNEL    -> 4 (ES7210 MIC1..MIC4)
 */
#define M1_I2S_PORT I2S_NUM_1
#define M1_SAMPLE_RATE_HZ 16000
#define M1_SLOT_BITS 32
#define M1_CHANNELS 2
#define M1_RECORD_GAIN_DB 30.0f
#define M1_CAPTURE_CHANNEL_COUNT 4

#define M1_BLOCK_FRAMES 256u
#define M1_BLOCK_SAMPLES (M1_BLOCK_FRAMES * (size_t)M1_CHANNELS)
#define M1_BLOCK_BYTES (M1_BLOCK_SAMPLES * sizeof(int32_t))
#define M1_BLOCK_MS ((M1_BLOCK_FRAMES * 1000u) / M1_SAMPLE_RATE_HZ)
#define M1_PA_SETTLE_MS 20u
#define M1_DMA_DESCRIPTORS 6u
#define M1_DMA_FRAMES 240u

typedef struct {
    i2s_chan_handle_t tx;
    i2s_chan_handle_t rx;
    const audio_codec_data_if_t *record_data_if;
    const audio_codec_ctrl_if_t *record_ctrl_if;
    const audio_codec_if_t *record_codec_if;
    esp_codec_dev_handle_t record_dev;
    bool record_open;
    const audio_codec_data_if_t *play_data_if;
    const audio_codec_ctrl_if_t *play_ctrl_if;
    const audio_codec_gpio_if_t *play_gpio_if;
    const audio_codec_if_t *play_codec_if;
    esp_codec_dev_handle_t play_dev;
    bool play_open;
} m1_ctx_t;

/* ---------------------------------------------------------------- probing */

static esp_err_t probe_codecs(zv_audio_m1_report_t *report)
{
    zv_i2c_probe_report_t probe = {0};

    ESP_RETURN_ON_ERROR(zv_board_probe_i2c(&probe), TAG, "I2C probe failed");

    for (size_t i = 0; i < ZV_BOARD_I2C_EXPECTED_COUNT; i++) {
        if (probe.entries[i].address == ZV_I2C_ADDR_ES7210) {
            report->es7210_present = probe.entries[i].present;
        } else if (probe.entries[i].address == ZV_I2C_ADDR_ES8311) {
            report->es8311_present = probe.entries[i].present;
        }
    }

    ESP_RETURN_ON_FALSE(report->es7210_present, ESP_ERR_NOT_FOUND, TAG,
                        "ES7210 not found at 0x%02x (gate HW-AUD-IN-01)", ZV_I2C_ADDR_ES7210);
    ESP_RETURN_ON_FALSE(report->es8311_present, ESP_ERR_NOT_FOUND, TAG,
                        "ES8311 not found at 0x%02x (gate HW-AUD-OUT-01)", ZV_I2C_ADDR_ES8311);
    ESP_LOGI(TAG, "codec probe ok: ES7210 0x%02x, ES8311 0x%02x", ZV_I2C_ADDR_ES7210, ZV_I2C_ADDR_ES8311);
    return ESP_OK;
}

/* ------------------------------------------------------------------- I2S */

static esp_err_t i2s_duplex_init(m1_ctx_t *ctx)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(M1_I2S_PORT, I2S_ROLE_MASTER);
    /* Network/UI gaps must send silence, never replay the previous DMA ring. */
    chan_cfg.auto_clear_after_cb = true;
    chan_cfg.dma_desc_num = M1_DMA_DESCRIPTORS;
    chan_cfg.dma_frame_num = M1_DMA_FRAMES;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &ctx->tx, &ctx->rx), TAG, "i2s_new_channel failed");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(M1_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(M1_SLOT_BITS, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = ZV_PIN_I2S_MCLK,
            .bclk = ZV_PIN_I2S_BCLK,
            .ws = ZV_PIN_I2S_WS,
            .dout = ZV_PIN_I2S_DOUT,
            .din = ZV_PIN_I2S_DIN,
            .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false},
        },
    };

    /* Upstream applies the identical std config to both directions. */
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(ctx->tx, &std_cfg), TAG, "i2s tx std init failed");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(ctx->rx, &std_cfg), TAG, "i2s rx std init failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(ctx->tx), TAG, "i2s tx enable failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(ctx->rx), TAG, "i2s rx enable failed");

    ESP_LOGI(TAG, "duplex I2S%d ready: %d Hz, %d-bit slots, mclk=%d bclk=%d ws=%d dout=%d din=%d",
             (int)M1_I2S_PORT, M1_SAMPLE_RATE_HZ, M1_SLOT_BITS, ZV_PIN_I2S_MCLK, ZV_PIN_I2S_BCLK, ZV_PIN_I2S_WS,
             ZV_PIN_I2S_DOUT, ZV_PIN_I2S_DIN);
    return ESP_OK;
}

/* ----------------------------------------------------------------- codecs */

static esp_err_t es7210_capture_init(m1_ctx_t *ctx)
{
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = M1_I2S_PORT,
        .rx_handle = ctx->rx,
        .tx_handle = NULL,
    };
    ctx->record_data_if = audio_codec_new_i2s_data(&i2s_cfg);
    ESP_RETURN_ON_FALSE(ctx->record_data_if != NULL, ESP_ERR_NO_MEM, TAG, "ES7210 I2S data interface failed");

    audio_codec_i2c_cfg_t i2c_cfg = {.addr = ES7210_CODEC_DEFAULT_ADDR, .bus_handle = zv_board_i2c_bus()};
    ctx->record_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    ESP_RETURN_ON_FALSE(ctx->record_ctrl_if != NULL, ESP_ERR_NOT_FOUND, TAG, "ES7210 I2C control interface failed");

    /* Upstream selects all four ES7210 inputs (MIC1|MIC2|MIC3|MIC4). */
    es7210_codec_cfg_t es7210_cfg = {
        .ctrl_if = ctx->record_ctrl_if,
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 | ES7210_SEL_MIC3 | ES7210_SEL_MIC4,
    };
    ctx->record_codec_if = es7210_codec_new(&es7210_cfg);
    ESP_RETURN_ON_FALSE(ctx->record_codec_if != NULL, ESP_ERR_NOT_FOUND, TAG, "ES7210 codec probe failed");

    esp_codec_dev_cfg_t dev_cfg = {
        .codec_if = ctx->record_codec_if,
        .data_if = ctx->record_data_if,
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
    };
    ctx->record_dev = esp_codec_dev_new(&dev_cfg);
    ESP_RETURN_ON_FALSE(ctx->record_dev != NULL, ESP_ERR_NO_MEM, TAG, "ES7210 codec device failed");

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = M1_SLOT_BITS,
        .channel = M1_CHANNELS,
        .sample_rate = M1_SAMPLE_RATE_HZ,
    };
    ESP_RETURN_ON_FALSE(esp_codec_dev_open(ctx->record_dev, &fs) == ESP_CODEC_DEV_OK, ESP_ERR_INVALID_RESPONSE, TAG,
                        "ES7210 open failed");
    ctx->record_open = true;

    for (int channel = 0; channel < M1_CAPTURE_CHANNEL_COUNT; channel++) {
        ESP_RETURN_ON_FALSE(esp_codec_dev_set_in_channel_gain(ctx->record_dev,
                                                              ESP_CODEC_DEV_MAKE_CHANNEL_MASK(channel),
                                                              M1_RECORD_GAIN_DB) == ESP_CODEC_DEV_OK,
                            ESP_ERR_INVALID_RESPONSE, TAG, "ES7210 channel %d gain failed", channel);
    }

    ESP_LOGI(TAG, "ES7210 capture open: %d Hz, %d ch, %d-bit, MIC1..MIC4 at %.1f dB", M1_SAMPLE_RATE_HZ, M1_CHANNELS,
             M1_SLOT_BITS, (double)M1_RECORD_GAIN_DB);
    return ESP_OK;
}

static esp_err_t es8311_playback_init(m1_ctx_t *ctx)
{
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = M1_I2S_PORT,
        .rx_handle = NULL,
        .tx_handle = ctx->tx,
    };
    ctx->play_data_if = audio_codec_new_i2s_data(&i2s_cfg);
    ESP_RETURN_ON_FALSE(ctx->play_data_if != NULL, ESP_ERR_NO_MEM, TAG, "ES8311 I2S data interface failed");

    audio_codec_i2c_cfg_t i2c_cfg = {.addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = zv_board_i2c_bus()};
    ctx->play_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    ESP_RETURN_ON_FALSE(ctx->play_ctrl_if != NULL, ESP_ERR_NOT_FOUND, TAG, "ES8311 I2C control interface failed");

    ctx->play_gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(ctx->play_gpio_if != NULL, ESP_ERR_NO_MEM, TAG, "ES8311 GPIO interface failed");

    /*
     * pa_pin stays -1 exactly as upstream (GPIO_PWR_CTRL): the codec driver
     * must never own the NS4150B. zv_board_pa_enable() is the single amplifier
     * owner, which is what lets this gate guarantee the amplifier is off on
     * every exit path.
     */
    es8311_codec_cfg_t es8311_cfg = {
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .ctrl_if = ctx->play_ctrl_if,
        .gpio_if = ctx->play_gpio_if,
        .pa_pin = -1,
        .use_mclk = false,
    };
    ctx->play_codec_if = es8311_codec_new(&es8311_cfg);
    ESP_RETURN_ON_FALSE(ctx->play_codec_if != NULL, ESP_ERR_NOT_FOUND, TAG, "ES8311 codec probe failed");

    esp_codec_dev_cfg_t dev_cfg = {
        .codec_if = ctx->play_codec_if,
        .data_if = ctx->play_data_if,
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
    };
    ctx->play_dev = esp_codec_dev_new(&dev_cfg);
    ESP_RETURN_ON_FALSE(ctx->play_dev != NULL, ESP_ERR_NO_MEM, TAG, "ES8311 codec device failed");

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = M1_SLOT_BITS,
        .channel = M1_CHANNELS,
        .sample_rate = M1_SAMPLE_RATE_HZ,
    };
    ESP_RETURN_ON_FALSE(esp_codec_dev_open(ctx->play_dev, &fs) == ESP_CODEC_DEV_OK, ESP_ERR_INVALID_RESPONSE, TAG,
                        "ES8311 open failed");
    ctx->play_open = true;

    /*
     * Volume is set AFTER open, which is a deliberate deviation from upstream.
     * bsp_codec_dac_init() calls esp_codec_dev_set_out_vol() before
     * esp_codec_dev_open(); at that point es8311_set_vol() returns
     * ESP_CODEC_DEV_WRONG_STATE because the codec is not open yet, and
     * esp_codec_dev_set_out_vol() discards that inner status and reports OK.
     * The volume register is therefore never written upstream and the part runs
     * at its power-on default. Calling after open is what actually reaches
     * ES8311_DAC_REG32.
     *
     * The read-back below is a defensive check on the API contract only:
     * esp_codec_dev_get_out_vol() returns the cached dev->volume, not the
     * codec register, so it proves the level was accepted and recorded, not
     * that the part latched it. The ordering above is the substantive fix.
     *
     * The value itself is deliberately below upstream PLAYER_VOLUME (50).
     */
    ESP_RETURN_ON_FALSE(esp_codec_dev_set_out_vol(ctx->play_dev, CONFIG_ZV_M1_PLAYBACK_VOLUME) == ESP_CODEC_DEV_OK,
                        ESP_ERR_INVALID_RESPONSE, TAG, "ES8311 volume failed");

    int applied_volume = -1;
    ESP_RETURN_ON_FALSE(esp_codec_dev_get_out_vol(ctx->play_dev, &applied_volume) == ESP_CODEC_DEV_OK,
                        ESP_ERR_INVALID_RESPONSE, TAG, "ES8311 volume read-back failed");
    ESP_RETURN_ON_FALSE(applied_volume == CONFIG_ZV_M1_PLAYBACK_VOLUME, ESP_ERR_INVALID_RESPONSE, TAG,
                        "ES8311 volume read-back %d != requested %d; refusing to play at an unexpected level",
                        applied_volume, CONFIG_ZV_M1_PLAYBACK_VOLUME);

    ESP_LOGI(TAG, "ES8311 playback open: %d Hz, %d ch, %d-bit, volume %d", M1_SAMPLE_RATE_HZ, M1_CHANNELS,
             M1_SLOT_BITS, CONFIG_ZV_M1_PLAYBACK_VOLUME);
    return ESP_OK;
}

/* ------------------------------------------------ microphone activity test */

static esp_err_t mic_activity_test(m1_ctx_t *ctx, zv_audio_m1_report_t *report)
{
    int32_t *block = heap_caps_malloc(M1_BLOCK_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(block != NULL, ESP_ERR_NO_MEM, TAG, "capture block allocation failed");

    zv_audio_probe_t session;
    esp_err_t err = ESP_OK;
    uint32_t elapsed_ms = 0;

    zv_audio_probe_reset(&session);

    /*
     * Discard the ES7210 start-up transient before evaluating anything. The
     * codec emits a near-full-scale burst in the first blocks after open; left
     * in, it satisfies the activity floor immediately and the gate passes on an
     * artifact instead of on real acoustic input.
     */
    zv_audio_probe_t settle;
    uint32_t settle_ms = 0;

    zv_audio_probe_reset(&settle);
    while (settle_ms < (uint32_t)CONFIG_ZV_M1_MIC_SETTLE_MS) {
        if (esp_codec_dev_read(ctx->record_dev, block, M1_BLOCK_BYTES) != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "I2S capture transfer failed while settling after %" PRIu32 " blocks",
                     report->settle_blocks);
            err = ESP_ERR_INVALID_RESPONSE;
            break;
        }
        report->settle_blocks++;
        (void)zv_audio_probe_feed_i32(&settle, block, M1_BLOCK_SAMPLES);
        settle_ms += M1_BLOCK_MS;
    }
    (void)zv_audio_probe_finish(&settle);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "microphone settled: discarded %" PRIu32 " blocks (transient rms=%u peak=%u permille)",
                 report->settle_blocks, (unsigned)settle.rms_permille, (unsigned)settle.peak_permille);
    }

    if (err == ESP_OK) {
        ESP_LOGW(TAG, "M1 microphone window open for %d ms: speak near the microphones until %d active blocks "
                      "accumulate (about %d ms of sound)",
                 CONFIG_ZV_M1_MIC_WINDOW_MS, CONFIG_ZV_M1_MIC_ACTIVE_BLOCKS,
                 CONFIG_ZV_M1_MIC_ACTIVE_BLOCKS * (int)M1_BLOCK_MS);
    }

    while (err == ESP_OK && elapsed_ms < (uint32_t)CONFIG_ZV_M1_MIC_WINDOW_MS) {
        if (esp_codec_dev_read(ctx->record_dev, block, M1_BLOCK_BYTES) != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "I2S capture transfer failed after %" PRIu32 " blocks", report->blocks_read);
            err = ESP_ERR_INVALID_RESPONSE;
            break;
        }
        report->blocks_read++;

        /* Per-block measurement decides activity; the session probe reports the run. */
        zv_audio_probe_t window;
        zv_audio_probe_reset(&window);
        (void)zv_audio_probe_feed_i32(&window, block, M1_BLOCK_SAMPLES);
        (void)zv_audio_probe_finish(&window);
        (void)zv_audio_probe_feed_i32(&session, block, M1_BLOCK_SAMPLES);

        if (zv_audio_probe_is_active(&window, (uint16_t)CONFIG_ZV_M1_MIC_FLOOR_PERMILLE)) {
            report->active_blocks++;
            /*
             * One loud block is not evidence of intentional sound. Handling the
             * puck, or tapping it while finishing the display challenge, clears
             * the floor for a block or two. Requiring an accumulated total
             * keeps this gate on sound the operator meant to make.
             */
            if (report->active_blocks >= (uint32_t)CONFIG_ZV_M1_MIC_ACTIVE_BLOCKS) {
                report->mic_activity = true;
                ESP_LOGI(TAG, "microphone activity observed: %" PRIu32 " active blocks, last rms=%u peak=%u permille",
                         report->active_blocks, (unsigned)window.rms_permille, (unsigned)window.peak_permille);
                break;
            }
        }
        elapsed_ms += M1_BLOCK_MS;
    }

    /*
     * Captured audio is never recorded, stored, transmitted, or retained: the
     * block is zeroed and released here, and only scalar measurements survive.
     */
    memset(block, 0, M1_BLOCK_BYTES);
    heap_caps_free(block);

    (void)zv_audio_probe_finish(&session);
    report->mic_rms_permille = session.rms_permille;
    report->mic_peak_permille = session.peak_permille;

    if (err != ESP_OK) {
        return err;
    }
    if (!report->mic_activity) {
        ESP_LOGE(TAG, "M1 acceptance failed: only %" PRIu32 " of %d required active blocks within %d ms "
                      "(session rms=%u permille)",
                 report->active_blocks, CONFIG_ZV_M1_MIC_ACTIVE_BLOCKS, CONFIG_ZV_M1_MIC_WINDOW_MS,
                 (unsigned)report->mic_rms_permille);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

/* ----------------------------------------------------- diagnostic playback */

static esp_err_t play_diagnostic_tone(m1_ctx_t *ctx, zv_audio_m1_report_t *report)
{
    size_t total_frames = zv_audio_tone_frame_count(M1_SAMPLE_RATE_HZ, CONFIG_ZV_M1_TONE_MS);
    ESP_RETURN_ON_FALSE(total_frames > 0, ESP_ERR_INVALID_SIZE, TAG, "tone length resolved to zero frames");

    int32_t *block = heap_caps_malloc(M1_BLOCK_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(block != NULL, ESP_ERR_NO_MEM, TAG, "tone block allocation failed");

    size_t fade_frames = total_frames / 8u;
    uint32_t phase = 0;
    esp_err_t err;

    /* The amplifier is enabled only here, and only after the microphone passed. */
    err = zv_board_pa_enable(true);
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(M1_PA_SETTLE_MS));
        ESP_LOGW(TAG, "NS4150B enabled for one %d ms tone at %d Hz, amplitude %u permille (clamped to %u)",
                 CONFIG_ZV_M1_TONE_MS, CONFIG_ZV_M1_TONE_HZ, (unsigned)CONFIG_ZV_M1_TONE_AMPLITUDE_PERMILLE,
                 (unsigned)zv_audio_tone_clamp_permille((uint16_t)CONFIG_ZV_M1_TONE_AMPLITUDE_PERMILLE));

        while (phase < total_frames) {
            size_t frames = total_frames - (size_t)phase;
            if (frames > M1_BLOCK_FRAMES) {
                frames = M1_BLOCK_FRAMES;
            }
            if (zv_audio_tone_fill_i32(block, frames, M1_CHANNELS, M1_SAMPLE_RATE_HZ, CONFIG_ZV_M1_TONE_HZ,
                                       (uint16_t)CONFIG_ZV_M1_TONE_AMPLITUDE_PERMILLE, total_frames, fade_frames,
                                       &phase) != ZV_OK) {
                err = ESP_ERR_INVALID_ARG;
                break;
            }
            if (esp_codec_dev_write(ctx->play_dev, block, (int)(frames * M1_CHANNELS * sizeof(int32_t))) !=
                ESP_CODEC_DEV_OK) {
                ESP_LOGE(TAG, "I2S playback transfer failed at frame %" PRIu32, phase);
                err = ESP_ERR_INVALID_RESPONSE;
                break;
            }
        }
    }

    /* Amplifier off before anything else, on both the success and failure path. */
    esp_err_t pa_err = zv_board_pa_enable(false);

    memset(block, 0, M1_BLOCK_BYTES);
    heap_caps_free(block);

    if (err == ESP_OK && pa_err == ESP_OK) {
        report->tone_played = true;
        ESP_LOGI(TAG, "diagnostic tone complete; NS4150B disabled");
    }
    return (err != ESP_OK) ? err : pa_err;
}

/* ---------------------------------------------------------------- teardown */

static void disable_channel_tolerantly(i2s_chan_handle_t channel)
{
    if (channel == NULL) {
        return;
    }
    /*
     * esp_codec_dev may already have disabled this channel when its device was
     * closed. In duplex mode it does that for the tx channel but not the rx
     * channel, and which one it touches is an internal detail of that
     * component, so the already-disabled case is tolerated rather than
     * modelled. i2s_channel_disable() logs at ERROR level before returning
     * ESP_ERR_INVALID_STATE, and i2s_del_channel() refuses a running channel,
     * so the call cannot simply be skipped; the i2s_common log level is lowered
     * across just this call and restored immediately afterwards.
     */
    esp_log_level_t previous = esp_log_level_get("i2s_common");
    esp_log_level_set("i2s_common", ESP_LOG_NONE);
    esp_err_t err = i2s_channel_disable(channel);
    esp_log_level_set("i2s_common", previous);

    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "I2S channel disable failed: %s", esp_err_to_name(err));
    }
}

static void m1_teardown(m1_ctx_t *ctx)
{
    if (ctx->play_dev != NULL) {
        if (ctx->play_open) {
            esp_codec_dev_close(ctx->play_dev);
            ctx->play_open = false;
        }
        esp_codec_dev_delete(ctx->play_dev);
        ctx->play_dev = NULL;
    }
    if (ctx->play_codec_if != NULL) {
        audio_codec_delete_codec_if(ctx->play_codec_if);
        ctx->play_codec_if = NULL;
    }
    if (ctx->play_ctrl_if != NULL) {
        audio_codec_delete_ctrl_if(ctx->play_ctrl_if);
        ctx->play_ctrl_if = NULL;
    }
    if (ctx->play_gpio_if != NULL) {
        audio_codec_delete_gpio_if(ctx->play_gpio_if);
        ctx->play_gpio_if = NULL;
    }
    if (ctx->play_data_if != NULL) {
        audio_codec_delete_data_if(ctx->play_data_if);
        ctx->play_data_if = NULL;
    }

    if (ctx->record_dev != NULL) {
        if (ctx->record_open) {
            esp_codec_dev_close(ctx->record_dev);
            ctx->record_open = false;
        }
        esp_codec_dev_delete(ctx->record_dev);
        ctx->record_dev = NULL;
    }
    if (ctx->record_codec_if != NULL) {
        audio_codec_delete_codec_if(ctx->record_codec_if);
        ctx->record_codec_if = NULL;
    }
    if (ctx->record_ctrl_if != NULL) {
        audio_codec_delete_ctrl_if(ctx->record_ctrl_if);
        ctx->record_ctrl_if = NULL;
    }
    if (ctx->record_data_if != NULL) {
        audio_codec_delete_data_if(ctx->record_data_if);
        ctx->record_data_if = NULL;
    }

    if (ctx->tx != NULL) {
        disable_channel_tolerantly(ctx->tx);
        (void)i2s_del_channel(ctx->tx);
        ctx->tx = NULL;
    }
    if (ctx->rx != NULL) {
        disable_channel_tolerantly(ctx->rx);
        (void)i2s_del_channel(ctx->rx);
        ctx->rx = NULL;
    }
}

#if CONFIG_ZV_M3A_VOICE_MODE
static m1_ctx_t s_voice_ctx;
static bool s_voice_ready;
static bool s_voice_playing;
static uint8_t s_voice_volume = CONFIG_ZV_M1_PLAYBACK_VOLUME;
/* One voice turn owns the codec; keep DMA-facing blocks off the small main-task stack. */
static int32_t s_voice_capture_block[M1_BLOCK_SAMPLES];
static int32_t s_voice_playback_block[1024];

esp_err_t zv_audio_voice_set_volume(uint8_t volume)
{
    if (volume > 100u) return ESP_ERR_INVALID_ARG;
    if (s_voice_ready) {
        if (esp_codec_dev_set_out_vol(s_voice_ctx.play_dev, volume) != ESP_CODEC_DEV_OK) {
            zv_audio_voice_close(); return ESP_ERR_INVALID_RESPONSE;
        }
    }
    s_voice_volume = volume;
    return ESP_OK;
}

esp_err_t zv_audio_voice_open(void)
{
    if (s_voice_ready) return ESP_OK;
    memset(&s_voice_ctx, 0, sizeof(s_voice_ctx));
    esp_err_t err = zv_board_pa_enable(false);
    if (err == ESP_OK) err = i2s_duplex_init(&s_voice_ctx);
    if (err == ESP_OK) err = es7210_capture_init(&s_voice_ctx);
    if (err == ESP_OK) err = es8311_playback_init(&s_voice_ctx);
    if (err != ESP_OK) {
        zv_audio_voice_close();
        return err;
    }
    s_voice_ready = true;
    return zv_audio_voice_set_volume(s_voice_volume);
}

esp_err_t zv_audio_voice_capture(int16_t pcm[256])
{
    if (!s_voice_ready || s_voice_playing || pcm == NULL) return ESP_ERR_INVALID_STATE;
    if (esp_codec_dev_read(s_voice_ctx.record_dev, s_voice_capture_block, sizeof(s_voice_capture_block)) != ESP_CODEC_DEV_OK) {
        memset(s_voice_capture_block, 0, sizeof(s_voice_capture_block));
        return ESP_ERR_INVALID_RESPONSE;
    }
    for (size_t i = 0; i < M1_BLOCK_FRAMES; ++i) pcm[i] = (int16_t)(s_voice_capture_block[2u * i] >> 16);
    memset(s_voice_capture_block, 0, sizeof(s_voice_capture_block));
    return ESP_OK;
}

esp_err_t zv_audio_voice_playback_start(void)
{
    if (!s_voice_ready || s_voice_playing) return ESP_ERR_INVALID_STATE;
    esp_err_t err = zv_board_pa_enable(true);
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(M1_PA_SETTLE_MS));
        s_voice_playing = true;
    }
    return err;
}

esp_err_t zv_audio_voice_playback(const int16_t *pcm, size_t samples)
{
    if (!s_voice_ready || !s_voice_playing || pcm == NULL || samples == 0u || samples > 512u)
        return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < samples; ++i) {
        /* Every PCM16 value fits after scaling; shifting a negative signed value is undefined. */
        int32_t sample = (int32_t)pcm[i] * 65536;
        s_voice_playback_block[2u * i] = sample;
        s_voice_playback_block[2u * i + 1u] = sample;
    }
    esp_err_t err = esp_codec_dev_write(s_voice_ctx.play_dev, s_voice_playback_block,
                                        (int)(samples * 2u * sizeof(int32_t))) == ESP_CODEC_DEV_OK
                        ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
    memset(s_voice_playback_block, 0, sizeof(s_voice_playback_block));
    if (err != ESP_OK) zv_audio_voice_close();
    return err;
}

void zv_audio_voice_close(void)
{
    (void)zv_board_pa_enable(false);
    s_voice_playing = false;
    s_voice_ready = false;
    m1_teardown(&s_voice_ctx);
    memset(&s_voice_ctx, 0, sizeof(s_voice_ctx));
}

esp_err_t zv_audio_voice_playback_finish(void)
{
    if (!s_voice_ready || !s_voice_playing) return ESP_ERR_INVALID_STATE;
    /* A codec write queues DMA; it does not mean the last sample was heard.
     * Queue more silence than the entire ring plus its partially filled block.
     * Blocking writes then prove the final speech samples have left the ring.
     * Cancellation keeps using close() directly, without this normal-end drain. */
    static const int16_t silence[512];
    size_t frames = (M1_DMA_DESCRIPTORS + 1u) * M1_DMA_FRAMES;
    esp_err_t err = ESP_OK;
    while (frames > 0u && err == ESP_OK) {
        size_t count = frames > 512u ? 512u : frames;
        err = zv_audio_voice_playback(silence, count);
        frames -= count;
    }
    zv_audio_voice_close();
    return err;
}
#else
esp_err_t zv_audio_voice_set_volume(uint8_t volume) { (void)volume; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t zv_audio_voice_open(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t zv_audio_voice_capture(int16_t pcm[256]) { (void)pcm; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t zv_audio_voice_playback_start(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t zv_audio_voice_playback(const int16_t *pcm, size_t samples)
{ (void)pcm; (void)samples; return ESP_ERR_NOT_SUPPORTED; }
void zv_audio_voice_close(void) {}
esp_err_t zv_audio_voice_playback_finish(void) { return ESP_ERR_NOT_SUPPORTED; }
#endif

/* ------------------------------------------------------------------ public */

/*
 * Board identity, heap/DMA budgets, and the battery sense path. These APIs
 * used to be compiled and never called, so every M1 run claimed budgets it
 * had not measured. Memory identity is fail-closed for this Rev2 mode; a
 * missing battery reading is recorded, not invented.
 */
static esp_err_t m1_board_measurements(zv_audio_m1_report_t *out)
{
    zv_board_memory_report_t mem = {0};

    ESP_RETURN_ON_ERROR(zv_board_memory_report(&mem), TAG, "memory report failed");
    out->memory_report_ok = true;
    out->flash_matches_rev2 = mem.flash_matches_rev2;
    out->psram_matches_rev2 = mem.psram_matches_rev2;
    out->flash_bytes = mem.flash_bytes;
    out->psram_bytes = mem.psram_bytes;
    out->internal_free = mem.internal_free;
    out->internal_min_free = mem.internal_min_free;
    out->psram_free = mem.psram_free;
    out->dma_largest_block = mem.dma_largest_block;

    ESP_RETURN_ON_FALSE(mem.flash_matches_rev2, ESP_ERR_INVALID_SIZE, TAG,
                        "flash is %" PRIu32 " bytes, Rev2 acceptance requires 16 MiB", mem.flash_bytes);
    ESP_RETURN_ON_FALSE(mem.psram_matches_rev2, ESP_ERR_INVALID_SIZE, TAG,
                        "PSRAM is %u bytes, Rev2 acceptance requires at least 8 MiB", (unsigned)mem.psram_bytes);

    ESP_RETURN_ON_ERROR(zv_board_battery_init(), TAG, "battery ADC init failed");
    out->battery_ready = true;
    ESP_LOGI(TAG,
             "M1 board: flash=%" PRIu32 " psram=%u int_free=%u int_min=%u psram_free=%u dma=%u",
             mem.flash_bytes, (unsigned)mem.psram_bytes, (unsigned)mem.internal_free,
             (unsigned)mem.internal_min_free, (unsigned)mem.psram_free, (unsigned)mem.dma_largest_block);
    return ESP_OK;
}

/*
 * Second heap/DMA snapshot after the audio path has run, plus one battery
 * reading. Battery failure is not fatal: a USB-powered lab unit may have no
 * calibrated sense path, and inventing a millivolt would be worse than saying
 * so. Heap/DMA identity stays fail-closed.
 */
static esp_err_t m1_board_finish_measurements(zv_audio_m1_report_t *out)
{
    zv_board_memory_report_t mem = {0};
    uint32_t battery_mv = 0;

    ESP_RETURN_ON_ERROR(zv_board_memory_report(&mem), TAG, "post-audio memory report failed");
    out->internal_free_after = mem.internal_free;
    out->dma_largest_block_after = mem.dma_largest_block;
    if (mem.internal_min_free < out->internal_min_free) {
        out->internal_min_free = mem.internal_min_free;
    }

    esp_err_t bat_err = zv_board_battery_read_mv(&battery_mv);
    if (bat_err == ESP_OK) {
        out->battery_read_ok = true;
        out->battery_mv = battery_mv;
    } else {
        out->battery_read_ok = false;
        ESP_LOGW(TAG, "battery reading unavailable (%s); report carries battery_read_ok=0", esp_err_to_name(bat_err));
    }

    ESP_LOGI(TAG, "M1 board after audio: int_free=%u int_min=%u dma=%u bat_ok=%d bat_mv=%" PRIu32,
             (unsigned)mem.internal_free, (unsigned)out->internal_min_free, (unsigned)mem.dma_largest_block,
             out->battery_read_ok ? 1 : 0, battery_mv);
    return ESP_OK;
}

esp_err_t zv_audio_m1_acceptance(zv_audio_m1_report_t *report)
{
    zv_audio_m1_report_t local;
    zv_audio_m1_report_t *out = (report != NULL) ? report : &local;
    m1_ctx_t ctx = {0};
    esp_err_t err;

    memset(out, 0, sizeof(*out));

    ESP_LOGW(TAG, "M1 acceptance mode enabled: ES7210 capture, ES8311 playback, NS4150B tone only; network, "
                  "gateway, Hermes, credentials, OTA/eFuse, erase remain gated");

    err = zv_board_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "board init failed: %s", esp_err_to_name(err));
        goto finish;
    }

    /* Amplifier off before any codec touches the bus. */
    err = zv_board_pa_enable(false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "amplifier disable failed: %s", esp_err_to_name(err));
        goto finish;
    }

    err = m1_board_measurements(out);
    if (err != ESP_OK) {
        goto finish;
    }

    err = probe_codecs(out);
    if (err != ESP_OK) {
        goto finish;
    }

    err = i2s_duplex_init(&ctx);
    if (err != ESP_OK) {
        goto finish;
    }
    out->i2s_ready = true;

    err = es7210_capture_init(&ctx);
    if (err != ESP_OK) {
        goto finish;
    }
    out->capture_ready = true;

    err = es8311_playback_init(&ctx);
    if (err != ESP_OK) {
        goto finish;
    }
    out->playback_ready = true;

    err = mic_activity_test(&ctx, out);
    if (err != ESP_OK) {
        goto finish;
    }

    /* Playback is reached only because the microphone stage passed. */
    err = play_diagnostic_tone(&ctx, out);

finish:
    if (out->memory_report_ok) {
        esp_err_t meas_err = m1_board_finish_measurements(out);
        if (err == ESP_OK && meas_err != ESP_OK) {
            err = meas_err;
        }
    }
    m1_teardown(&ctx);

    /*
     * Unconditional final amplifier disable. This runs after success, after any
     * error above, and after teardown, so no exit path can leave NS4150B on.
     */
    {
        esp_err_t pa_err = zv_board_pa_enable(false);
        out->amplifier_off = (pa_err == ESP_OK);
        if (pa_err != ESP_OK) {
            ESP_LOGE(TAG, "final amplifier disable failed: %s", esp_err_to_name(pa_err));
            if (err == ESP_OK) {
                err = pa_err;
            }
        }
    }

    ESP_LOGI(TAG,
             "M1 report: es7210=%d es8311=%d i2s=%d capture=%d playback=%d mic_activity=%d tone=%d amp_off=%d "
             "rms=%u peak=%u settle_blocks=%" PRIu32 " active_blocks=%" PRIu32 " blocks=%" PRIu32,
             out->es7210_present, out->es8311_present, out->i2s_ready, out->capture_ready, out->playback_ready,
             out->mic_activity, out->tone_played, out->amplifier_off, (unsigned)out->mic_rms_permille,
             (unsigned)out->mic_peak_permille, out->settle_blocks, out->active_blocks, out->blocks_read);
    ESP_LOGI(TAG,
             "M1 board report: mem=%d flash_ok=%d psram_ok=%d flash=%" PRIu32 " psram=%u "
             "int_free=%u int_free_after=%u int_min=%u psram_free=%u dma=%u dma_after=%u "
             "bat_ready=%d bat_ok=%d bat_mv=%" PRIu32,
             out->memory_report_ok ? 1 : 0, out->flash_matches_rev2 ? 1 : 0, out->psram_matches_rev2 ? 1 : 0,
             out->flash_bytes, (unsigned)out->psram_bytes, (unsigned)out->internal_free,
             (unsigned)out->internal_free_after, (unsigned)out->internal_min_free, (unsigned)out->psram_free,
             (unsigned)out->dma_largest_block, (unsigned)out->dma_largest_block_after,
             out->battery_ready ? 1 : 0, out->battery_read_ok ? 1 : 0, out->battery_mv);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "M1 audio acceptance passed; platform remains gated");
    } else {
        ESP_LOGE(TAG, "M1 audio acceptance failed: %s", esp_err_to_name(err));
    }
    return err;
}

#if CONFIG_ZV_M1_SOAK_MODE

/* Hard floors: below these the panel/DMA path can no longer paint reliably. */
#define SOAK_MIN_INTERNAL_FREE 24576u
#define SOAK_MIN_DMA_BLOCK 4096u

static uint32_t soak_clamp_ms(uint32_t requested, uint32_t lo, uint32_t hi, uint32_t fallback)
{
    if (requested < lo || requested > hi) {
        return fallback;
    }
    return requested;
}

static void soak_sample_board(zv_audio_soak_report_t *out, uint32_t sample_index)
{
    zv_board_memory_report_t mem = {0};
    uint32_t battery_mv = 0;

    if (zv_board_memory_report(&mem) == ESP_OK) {
        out->internal_free_end = mem.internal_free;
        out->dma_largest_end = mem.dma_largest_block;
        if (mem.internal_min_free < out->internal_min_free || sample_index == 0u) {
            out->internal_min_free = mem.internal_min_free;
        }
    }
    if (zv_board_battery_read_mv(&battery_mv) == ESP_OK) {
        out->battery_read_ok = true;
        out->battery_mv_last = battery_mv;
    }
    out->report_samples++;
    ESP_LOGI(TAG,
             "M1 soak sample %" PRIu32 ": int_free=%u int_min=%u dma=%u bat_ok=%d bat_mv=%" PRIu32
             " slices=%" PRIu32 " blocks=%" PRIu32 " tones=%" PRIu32,
             sample_index, (unsigned)out->internal_free_end, (unsigned)out->internal_min_free,
             (unsigned)out->dma_largest_end, out->battery_read_ok ? 1 : 0, out->battery_mv_last,
             out->capture_slices, out->capture_blocks, out->tones_played);
}

esp_err_t zv_audio_m1_soak(zv_audio_soak_report_t *report)
{
    zv_audio_soak_report_t local;
    zv_audio_soak_report_t *out = (report != NULL) ? report : &local;
    m1_ctx_t ctx = {0};
    zv_audio_m1_report_t scratch = {0};
    esp_err_t err;

    memset(out, 0, sizeof(*out));
    out->duration_ms = soak_clamp_ms((uint32_t)CONFIG_ZV_M1_SOAK_DURATION_MS, 60000u, 7200000u, 3600000u);
    const uint32_t report_period = soak_clamp_ms((uint32_t)CONFIG_ZV_M1_SOAK_REPORT_PERIOD_MS, 1000u, 600000u, 30000u);
    const uint32_t tone_period = soak_clamp_ms((uint32_t)CONFIG_ZV_M1_SOAK_TONE_PERIOD_MS, 10000u, 600000u, 120000u);
    const uint32_t slice_ms = soak_clamp_ms((uint32_t)CONFIG_ZV_M1_SOAK_CAPTURE_SLICE_MS, 16u, 2000u, 160u);

    ESP_LOGW(TAG,
             "M1 soak mode enabled: %" PRIu32 " ms duplex capture slices every %" PRIu32
             " ms, tone every %" PRIu32 " ms; audio never retained; network/gateway/Hermes/credentials/OTA gated",
             slice_ms, report_period, tone_period);

    err = zv_board_pa_enable(false);
    if (err != ESP_OK) {
        goto finish;
    }

    err = probe_codecs(&scratch);
    if (err != ESP_OK) {
        goto finish;
    }
    err = i2s_duplex_init(&ctx);
    if (err != ESP_OK) {
        goto finish;
    }
    err = es7210_capture_init(&ctx);
    if (err != ESP_OK) {
        goto finish;
    }
    err = es8311_playback_init(&ctx);
    if (err != ESP_OK) {
        goto finish;
    }

    {
        zv_board_memory_report_t mem = {0};
        if (zv_board_memory_report(&mem) == ESP_OK) {
            out->internal_free_start = mem.internal_free;
            out->internal_free_end = mem.internal_free;
            out->internal_min_free = mem.internal_min_free;
            out->dma_largest_start = mem.dma_largest_block;
            out->dma_largest_end = mem.dma_largest_block;
        }
        (void)zv_board_battery_init();
        soak_sample_board(out, 0);
    }

    int32_t *block = heap_caps_malloc(M1_BLOCK_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (block == NULL) {
        err = ESP_ERR_NO_MEM;
        goto finish;
    }

    {
        const TickType_t slice_ticks = pdMS_TO_TICKS(slice_ms);
        const int64_t t0_us = esp_timer_get_time();
        uint32_t next_report = report_period;
        uint32_t next_tone = tone_period;

        /*
         * Duration and periods use wall clock, not accounted slice_ms. Real
         * time spent in blocking I2S reads plus vTaskDelay runs longer than
         * slice_ms, so an accounted clock stretches a "60 minute" soak.
         */
        for (;;) {
            const int64_t now_us = esp_timer_get_time();
            const uint32_t elapsed = (uint32_t)((now_us - t0_us) / 1000);
            out->elapsed_ms = elapsed;
            if (elapsed >= out->duration_ms) {
                break;
            }

            const size_t slice_blocks = (slice_ms + M1_BLOCK_MS - 1u) / M1_BLOCK_MS;
            for (size_t i = 0; i < slice_blocks; i++) {
                if (esp_codec_dev_read(ctx.record_dev, block, M1_BLOCK_BYTES) != ESP_CODEC_DEV_OK) {
                    ESP_LOGE(TAG, "M1 soak capture transfer failed after %" PRIu32 " blocks", out->capture_blocks);
                    err = ESP_ERR_INVALID_RESPONSE;
                    break;
                }
                out->capture_blocks++;
            }
            memset(block, 0, M1_BLOCK_BYTES);
            out->capture_slices++;

            if (err == ESP_OK && elapsed >= next_tone) {
                zv_audio_m1_report_t tone_report = {0};
                err = play_diagnostic_tone(&ctx, &tone_report);
                if (err == ESP_OK) {
                    out->tones_played++;
                }
                next_tone = elapsed + tone_period;
            }
            if (err == ESP_OK && elapsed >= next_report) {
                soak_sample_board(out, out->report_samples);
                next_report = elapsed + report_period;
            }
            if (err != ESP_OK) {
                break;
            }

            vTaskDelay(slice_ticks);
        }
    }

    memset(block, 0, M1_BLOCK_BYTES);
    heap_caps_free(block);
    block = NULL;

    if (err == ESP_OK) {
        soak_sample_board(out, out->report_samples);
        if (out->internal_free_end < SOAK_MIN_INTERNAL_FREE) {
            ESP_LOGE(TAG, "M1 soak failed: internal free %u below floor %u after %" PRIu32 " ms",
                     (unsigned)out->internal_free_end, (unsigned)SOAK_MIN_INTERNAL_FREE, out->elapsed_ms);
            err = ESP_ERR_NO_MEM;
        } else if (out->dma_largest_end < SOAK_MIN_DMA_BLOCK) {
            ESP_LOGE(TAG, "M1 soak failed: largest DMA block %u below floor %u after %" PRIu32 " ms",
                     (unsigned)out->dma_largest_end, (unsigned)SOAK_MIN_DMA_BLOCK, out->elapsed_ms);
            err = ESP_ERR_NO_MEM;
        }
    }

finish:
    {
        esp_err_t pa_err = zv_board_pa_enable(false);
        out->amplifier_off = (pa_err == ESP_OK);
        if (pa_err != ESP_OK) {
            ESP_LOGE(TAG, "M1 soak final amplifier disable failed: %s", esp_err_to_name(pa_err));
            if (err == ESP_OK) {
                err = pa_err;
            }
        }
    }
    m1_teardown(&ctx);

    ESP_LOGI(TAG,
             "M1 soak report: elapsed=%" PRIu32 " slices=%" PRIu32 " blocks=%" PRIu32 " tones=%" PRIu32
             " samples=%" PRIu32 " int_free=%u->%u dma=%u->%u bat_ok=%d bat_mv=%" PRIu32 " amp_off=%d",
             out->elapsed_ms, out->capture_slices, out->capture_blocks, out->tones_played, out->report_samples,
             (unsigned)out->internal_free_start, (unsigned)out->internal_free_end, (unsigned)out->dma_largest_start,
             (unsigned)out->dma_largest_end, out->battery_read_ok ? 1 : 0, out->battery_mv_last,
             out->amplifier_off ? 1 : 0);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "M1 soak passed; platform remains gated");
    } else {
        ESP_LOGE(TAG, "M1 soak failed: %s", esp_err_to_name(err));
    }
    return err;
}

#else /* !CONFIG_ZV_M1_SOAK_MODE */

esp_err_t zv_audio_m1_soak(zv_audio_soak_report_t *report)
{
    if (report != NULL) {
        memset(report, 0, sizeof(*report));
    }
    return ESP_ERR_NOT_SUPPORTED;
}

#endif /* CONFIG_ZV_M1_SOAK_MODE */

#else /* !CONFIG_ZV_M1_ACCEPTANCE_MODE */

/*
 * Fail-closed stub. With M1 disabled the acceptance sequence is not compiled at
 * all, so a default-off build contains no path that can enable the NS4150B
 * amplifier or open the microphones.
 */
esp_err_t zv_audio_m1_acceptance(zv_audio_m1_report_t *report)
{
    if (report != NULL) {
        memset(report, 0, sizeof(*report));
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t zv_audio_m1_soak(zv_audio_soak_report_t *report)
{
    if (report != NULL) {
        memset(report, 0, sizeof(*report));
    }
    return ESP_ERR_NOT_SUPPORTED;
}

#endif /* CONFIG_ZV_M1_ACCEPTANCE_MODE */
