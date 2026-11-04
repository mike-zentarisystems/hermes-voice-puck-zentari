/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

/**
 * Device identity constants. The board id is embedded in OTA manifests and the
 * device.hello message so that images built for other boards (including the
 * V1 "-Box" schematic variant, which uses a different audio path) are rejected.
 */
#define ZV_BOARD_ID "waveshare-esp32-s3-touch-lcd-1.85c-box-rev2"

/** WebSocket protocol major version implemented by this firmware. */
#define ZV_PROTOCOL_VERSION 1

/** WebSocket subprotocol token negotiated during the HTTP upgrade. */
#define ZV_PROTOCOL_SUBPROTOCOL "zentari.voice.v1"

/** Upstream vendor reference revision used for every hardware fact. */
#define ZV_UPSTREAM_WAVESHARE_REPO "https://github.com/waveshareteam/ESP32-S3-Touch-LCD-1.85C"
#define ZV_UPSTREAM_WAVESHARE_COMMIT "8ead4a96bf3a278fc4ebd8ef4768657e17fa2880"
