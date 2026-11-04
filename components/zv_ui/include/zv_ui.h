/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 *
 * Bounded UI acceptance: prove all eight avatar states render on the Rev2
 * panel through the procedural painter. Default OFF; platform stays gated.
 */
#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "zv_link_supervisor.h"
#include "zv_app_state.h"
#include "zv_volume.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * After the M0 display challenge has already proven the panel, walks every
 * avatar state (idle, listening, thinking, speaking, executing, success,
 * error, approval), then the menu overlay. Each screen is held until the
 * operator taps, so a state cannot be skipped without looking at it.
 *
 * Network, gateway, Hermes, credentials, OTA/eFuse, erase remain gated.
 */
esp_err_t zv_ui_acceptance_demo(void);

/** Paints a fail-closed network status screen using measured Wi-Fi signal bars (0..3). */
esp_err_t zv_ui_show_link_state(zv_link_state_t state, uint32_t retry_ms, uint8_t wifi_signal_bars);

/**
 * Runs the interactive menu shell until the operator long-presses home to leave.
 *
 * Offers a focus-driven menu with a volume screen, a mute toggle that remembers
 * the level across mute, and a restart that has to be confirmed on a second
 * screen. The clock refreshes on CONFIG_ZV_UI_REPAINT_PERIOD_MS because it is
 * the only chrome that changes without input.
 *
 * Default OFF, selected by ZV_UI_SHELL_MODE. Network, gateway, Hermes,
 * credentials, OTA/eFuse and erase remain gated; a restart is the only
 * destructive action reachable from here and it requires the confirmation screen.
 */
esp_err_t zv_ui_shell(void);

/**
 * Paints the one-shot boot screen: Zenti waking with a concise STARTING
 * status, zero Wi-Fi bars, and an unauthenticated gateway dot until a link is
 * proven. Presentation only; safe to call once before networking starts.
 */
esp_err_t zv_ui_show_booting(void);

/**
 * Open the round-safe menu after the caller validates the visible home touch.
 * Waits for dismissal. Voice callers cancel active audio before opening it
 * and restore their state afterward. Executes no gateway/Hermes actions.
 */
esp_err_t zv_ui_service_status_menu(void);

/** Shared settings state; polling never waits for another touch. */
void zv_ui_settings_init(void);
bool zv_ui_take_conversation_reset(void);
uint8_t zv_ui_voice_volume(void);
zv_volume_t zv_ui_voice_volume_state(void);
bool zv_ui_settings_active(void);
esp_err_t zv_ui_settings_open(uint8_t bars, bool online);
esp_err_t zv_ui_settings_open_volume(uint8_t bars, bool online);
esp_err_t zv_ui_settings_resume(uint8_t bars, bool online);
esp_err_t zv_ui_settings_poll(bool touched, uint16_t x, uint16_t y, uint32_t stamp);
esp_err_t zv_ui_voice_audio(zv_avatar_state_t state, const int16_t *pcm, size_t count);
/** Advance timed Thinking feedback without audio or header/menu repaint. */
esp_err_t zv_ui_voice_tick(void);
esp_err_t zv_ui_voice_refresh(uint8_t signal_bars);

/** M3a authenticated turn screen. Keeps date, battery and status-menu chrome. */
esp_err_t zv_ui_show_voice_state(zv_avatar_state_t state, const char *status);

#ifdef __cplusplus
}
#endif
