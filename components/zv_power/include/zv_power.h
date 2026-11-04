/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
/* Call after board/Wi-Fi initialization; the voice/link task is sole owner. */
esp_err_t zv_power_init(uint32_t now_ms);
esp_err_t zv_power_update(uint32_t now_ms, bool touched, bool voice_active);
