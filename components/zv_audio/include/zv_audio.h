/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 *
 * Bounded M1 audio hardware acceptance for the Waveshare
 * ESP32-S3-Touch-LCD-1.85C Rev2 (ES7210 capture, ES8311 playback, NS4150B).
 *
 * Every I2S pin, clock, slot format, codec address, channel selection, and
 * codec setting used here is copied from the pinned upstream revision
 * 8ead4a96bf3a278fc4ebd8ef4768657e17fa2880. Deliberate deviations (playback
 * volume and tone amplitude, both reduced) are recorded in
 * third_party/waveshare/UPSTREAM.md.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** M3a half-duplex audio path; available only after M2 session acceptance. */
esp_err_t zv_audio_voice_open(void);
esp_err_t zv_audio_voice_set_volume(uint8_t volume);
esp_err_t zv_audio_voice_capture(int16_t pcm[256]);
esp_err_t zv_audio_voice_playback_start(void);
esp_err_t zv_audio_voice_playback(const int16_t *pcm, size_t samples);
/** Drain the final speech block through DMA, then close; cancel uses close directly. */
esp_err_t zv_audio_voice_playback_finish(void);
void zv_audio_voice_close(void);

/** Result of one bounded M1 acceptance run. Carries measurements, never audio. */
typedef struct {
    bool es7210_present;      /**< ES7210 answered on the shared I2C bus */
    bool es8311_present;      /**< ES8311 answered on the shared I2C bus */
    bool i2s_ready;           /**< Duplex I2S channels created and enabled */
    bool capture_ready;       /**< ES7210 capture device opened */
    bool playback_ready;      /**< ES8311 playback device opened */
    bool mic_activity;        /**< Bounded activity window observed sound */
    bool tone_played;         /**< Finite diagnostic tone completed */
    bool amplifier_off;       /**< NS4150B confirmed disabled at exit */
    uint16_t mic_peak_permille; /**< Loudest sample observed, permille of full scale */
    uint16_t mic_rms_permille;  /**< Session RMS, permille of full scale */
    uint32_t settle_blocks;   /**< Capture blocks discarded while the ES7210 settled */
    uint32_t blocks_read;     /**< I2S capture blocks evaluated for activity */
    uint32_t active_blocks;   /**< Blocks that exceeded the activity floor */
    /*
     * Board budgets and the battery path. These are M1 evidence, not audio.
     * Memory identity (flash/PSRAM) is fail-closed for this Rev2 acceptance
     * mode. A missing battery reading is reported honestly and does not fail
     * the audio gate: USB-only lab units may have no calibrated sense path yet.
     */
    bool memory_report_ok;        /**< Both heap snapshots succeeded */
    bool flash_matches_rev2;      /**< Flash size is 16 MiB */
    bool psram_matches_rev2;      /**< PSRAM size is at least 8 MiB */
    bool battery_ready;           /**< ADC battery path initialized */
    bool battery_read_ok;         /**< A calibrated millivolt reading was taken */
    uint32_t flash_bytes;         /**< Observed flash capacity */
    size_t psram_bytes;           /**< Observed PSRAM capacity */
    size_t internal_free;         /**< Internal heap free after board bring-up */
    size_t internal_free_after;   /**< Internal heap free after the diagnostic tone */
    size_t internal_min_free;     /**< Boot watermark for internal heap */
    size_t psram_free;            /**< PSRAM free after board bring-up */
    size_t dma_largest_block;     /**< Largest internal DMA block before audio */
    size_t dma_largest_block_after; /**< Largest internal DMA block after audio */
    uint32_t battery_mv;          /**< Battery voltage in mV when readable */
} zv_audio_m1_report_t;

/**
 * Runs the bounded M1 audio acceptance sequence.
 *
 * Order is fixed and fail-closed: probe both codecs, bring up duplex I2S,
 * open ES7210 then ES8311, discard the ES7210 start-up transient, measure
 * microphone activity without retaining any audio, and only on a passing
 * microphone measurement enable the amplifier for one finite low-volume
 * diagnostic tone.
 *
 * The NS4150B amplifier is disabled on entry and is driven off again on every
 * success and every error path before this function returns.
 *
 * Network, gateway, Hermes, credentials, OTA/eFuse, erase, and other
 * destructive operations are not touched here and remain gated.
 *
 * @param report Optional; populated on both success and failure when non-NULL.
 * @return ESP_OK only when every stage passed.
 */
esp_err_t zv_audio_m1_acceptance(zv_audio_m1_report_t *report);

/** Result of one bounded M1 soak run. Measurements only; never audio. */
typedef struct {
    uint32_t duration_ms;       /**< Requested soak length */
    uint32_t elapsed_ms;        /**< Wall time actually spent in the loop */
    uint32_t capture_slices;    /**< Capture slices completed */
    uint32_t capture_blocks;    /**< I2S blocks read and discarded */
    uint32_t tones_played;      /**< Finite diagnostic tones completed */
    uint32_t report_samples;    /**< Periodic heap/battery samples logged */
    bool amplifier_off;         /**< NS4150B confirmed disabled at exit */
    size_t internal_free_start;
    size_t internal_free_end;
    size_t internal_min_free;   /**< Lowest watermark observed during soak */
    size_t dma_largest_start;
    size_t dma_largest_end;
    bool battery_read_ok;       /**< At least one calibrated battery sample */
    uint32_t battery_mv_last;   /**< Last readable battery millivolts */
} zv_audio_soak_report_t;

/**
 * Bounded M1 soak: exercises duplex I2S capture and periodic low-volume
 * playback for a fixed wall-clock duration while sampling heap, DMA, and
 * battery. Audio is never retained — each capture slice is zeroed and the
 * working block is released before return. The amplifier is enabled only
 * inside the finite tone path and is driven off on every exit.
 *
 * Requires M1 acceptance to have already passed (platform runs it first).
 * Duration is clamped in code, not trusted to Kconfig alone.
 * Platform ready stays false; network and credentials remain gated.
 *
 * @param report Optional; populated on success and failure when non-NULL.
 */
esp_err_t zv_audio_m1_soak(zv_audio_soak_report_t *report);

#ifdef __cplusplus
}
#endif
