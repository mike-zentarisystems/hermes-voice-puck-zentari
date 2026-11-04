/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include "zv_status.h"

typedef struct { bool ready; zv_status_t status; const char *detail; } zv_platform_report_t;

/**
 * Hardware bring-up gate. The foundation does not claim reviewed display,
 * touch, audio, provisioning, gateway or OTA drivers.
 */
zv_platform_report_t zv_platform_start(void);

zv_status_t zv_ui_start(void);
zv_status_t zv_avatar_start(void);
zv_status_t zv_audio_start(void);
zv_status_t zv_network_start(void);
zv_status_t zv_gateway_start(void);
zv_status_t zv_ota_start(void);
zv_status_t zv_provisioning_start(void);
zv_status_t zv_diagnostics_start(void);
zv_status_t zv_hermes_approval_start(void);
