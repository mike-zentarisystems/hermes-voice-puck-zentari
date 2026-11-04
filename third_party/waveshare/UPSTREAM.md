# Upstream reference: Waveshare ESP32-S3-Touch-LCD-1.85C

Every hardware fact in this repository is traced to this exact upstream
revision. If the upstream repository changes, facts must be re-verified and
this file, `NOTICE`, and `hardware/pinmap.json` updated together.

| Field | Value |
|---|---|
| Repository | https://github.com/waveshareteam/ESP32-S3-Touch-LCD-1.85C |
| Commit | `8ead4a96bf3a278fc4ebd8ef4768657e17fa2880` |
| Commit date | 2026-07-30T11:11:43+08:00 |
| Commit subject | `updata idf code` |
| License | Apache License 2.0 (`third_party/waveshare/LICENSE`, byte-identical copy) |
| LICENSE SHA-256 | `1eb85fc97224598dad1852b5d6483bbcf0aa8608790dcc657a5a2a761ae9c8c6` |
| Reference project | `ESP-IDF/ESP32-S3-Touch-LCD-1.85C-Test` (generated with ESP-IDF 5.5.1) |

## Artifact fingerprints (SHA-256, not redistributed here)

| Upstream path | SHA-256 |
|---|---|
| `hardware/schematics/ESP32-S3-Touch-LCD-1.85C-V2--Schematic.pdf_.pdf` | `88f11c93958fbda384ef7bf825924c4b8cd663052bd37c215f2f9a053c981c3a` |
| `hardware/schematics/ESP32-S3-Touch-LCD-1.85C-V1--Schematic.pdf_.pdf` | `13ab1b27514e12b67b5f9a8357c7604285e201e89260973bc2365d1da2744dc7` |
| `hardware/dimensions/ESP32-S3-Touch-LCD-1.85C-dimension.pdf` | `1b682d44395a6bbaa5287f1cc91161fbbb2444e7ebe497b603a864632af3f31d` |
| `hardware/dimensions/ESP32-S3-Touch-LCD-1.85C-dimension.dxf` | `118df52543f426788f217f2b53e1b63abd2292e1fccb082fb2247f8da38c6b48` |
| `Firmware/ESP32-S3-Touch-LCD-1.85C.bin` (6,220,298 bytes) | `5b20359c49ac05a3326b1fd6e0f438fc921af301b2928e7c7404661be24f00ae` |
| `Firmware/dlna.bin` (4,194,304 bytes) | `94167eef2dc2fad9e6fb9484ab67f8899eac967733b3476f07374ecd008663a9` |

## Observations recorded during inspection

- The **V2** schematic title block reads `ESP32-S3-Touch-LCD-1.85C` and shows
  ES8311 (DAC), ES7210 (4-channel ADC), NS4150B (class-D amplifier), two
  analog microphones on ES7210 MIC1/MIC2, and an `AEC` network feeding the
  speaker output (OUTP/OUTN) back into ES7210 MIC3.
- The **V1** schematic title block reads `ESP32-S3-Touch-LCD-1.85C-Box` and
  shows a different audio path (I2S digital microphone and I2S DAC nets).
  The "Box" naming therefore does **not** by itself identify the revision.
  Gate `HW-REV-01` requires physically confirming the ES8311/ES7210/NS4150B
  parts on the unit before any Rev2.0 firmware is trusted on it.
- Both vendor `.bin` files begin with image magic `0xE9` at offset `0x0`,
  partition table magic `0xAA50` at `0x8000`, and app image magic `0xE9` at
  `0x10000`, which is consistent with merged full-flash images written at
  offset `0x0`. This is an inference from header bytes; M0 backs up the
  unit's own flash instead of relying on these files.
- The vendor reference firmware ignores most I2C and codec error returns and
  has no OTA partitions (`factory` 3 MB + `flash_test` FAT + `model` SPIFFS).
  Only facts, not structure, are taken from it.

## What was copied

| This repository | Upstream origin | Change |
|---|---|---|
| `LICENSE`, `third_party/waveshare/LICENSE` | `LICENSE` | Verbatim |
| `components/zv_board/src/zv_board.c` M0 display/touch path | Complete `vendor_specific_init_new[]`, QSPI opcodes, EXIO reset timing, and backlight duty curve in `main/LCD_Driver/ST77916.c`; CST816 registers, coordinate parsing, autosleep setting, and EXIO reset in `main/Touch_Driver/CST816.c` | Values copied for bounded M0 display/touch feedback |
| Pin, address, and timing constants in `components/zv_board/include/zv_board_pins.h` | Files cited per constant in `hardware/pinmap.json` | Values only |
| `components/zv_audio/src/zv_audio.c` M1 audio path | `components/auido_borad/boards/ESP32_S3_AUDIO_Board/bsp_board.c` and `components/auido_borad/boards/include/ESP32_S3_AUDIO_Board.h` | Configuration values only; see the table below |
| `components/zv_audio/idf_component.yml` | `components/auido_borad/idf_component.yml` | Same dependency and constraint (`espressif/esp_codec_dev: ^1.3.0`) |
| `components/zv_lcd_st77916/` | `main/LCD_Driver/esp_lcd_st77916/` | **Verbatim source copy** (Apache-2.0, Espressif). SHA-256 with LF endings: `esp_lcd_st77916.c` `45ff8a9223460aa053b6916c94d1a46622eda02c19046ce181e49d3c4ff66b22`, `esp_lcd_st77916.h` `21f444cdc25ed1ceec379fe91fb5fd09d31ef179a946c9a7bd6f67d2e3599bfa` |
| `components/zv_board/src/zv_board.c` LCD bring-up | `main/LCD_Driver/ST77916.c` `QSPI_Init()` | Panel-IO config, register 0x04 ID read at 3 MHz, init-table selection, reset/init/display-on order, and 16-line `draw_bitmap` fills |

Apart from the vendored ST77916 panel driver listed above, no other upstream
code, binaries, schematics, or third-party libraries bundled in the upstream
repository (for example the Arduino LVGL and audio libraries) are copied or
redistributed.

## Display bring-up notes

The panel exists in two variants, distinguished by four bytes read from
register `0x04` over a temporary 3 MHz panel IO. `00 02 7F 7F` takes the pinned
`vendor_specific_init_new` table; anything else takes the driver's own
`vendor_specific_init_default`. The unit on hand reports `00 02 7F 7F`.

The earlier hand-rolled QSPI path in this repository re-issued `RAMWR` (`0x2C`)
for every row, which restarts each write at the window origin instead of
continuing it, and it never ran the driver's reset/init/display-on sequence.
Fills now go through `esp_lcd_panel_draw_bitmap` in the same 16-line chunks
upstream `lcd_clear_screen()` uses.

Colour byte order is not asserted by the M0 gate. The acceptance criterion is
that distinct bands and the touch square are visible, not that a given band is
a particular hue.


## M1 audio configuration copied from the pinned source

Every value below is used verbatim by `components/zv_audio/src/zv_audio.c`.
`tools/validate.py` check `m1-upstream-audio-config` re-extracts each one from
the pinned tree on every run and fails if the local constant and the upstream
value diverge.

| Setting | Value | Upstream origin |
|---|---|---|
| I2S port | `I2S_NUM_1` | `bsp_board.c` `bsp_i2s_init(I2S_NUM_1, 16000, 2, 32)` |
| Sample rate | 16000 Hz | `bsp_board.c` `bsp_i2s_init` / `bsp_codec_init` |
| Slot width | 32-bit | `bsp_board.c` `bsp_i2s_init` / `bsp_codec_init` |
| Channels | 2 (stereo) | `bsp_board.c` `bsp_i2s_init` / `bsp_codec_init` |
| Clock config | `I2S_STD_CLK_DEFAULT_CONFIG(16000)` | `ESP32_S3_AUDIO_Board.h` `I2S_CONFIG_DEFAULT` |
| Slot config | `I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(32, I2S_SLOT_MODE_STEREO)` | `ESP32_S3_AUDIO_Board.h` `I2S_CONFIG_DEFAULT` |
| Role | `I2S_ROLE_MASTER` | `bsp_board.c` `I2S_CHANNEL_DEFAULT_CONFIG(i2s_num, I2S_ROLE_MASTER)` |
| Duplex | one std config applied to both tx and rx | `bsp_board.c` `i2s_channel_init_std_mode(tx_handle/rx_handle, &std_cfg)` |
| MCLK / BCLK / WS / DOUT / DIN | 2 / 48 / 38 / 47 / 39 | `ESP32_S3_AUDIO_Board.h` `GPIO_I2S_MCLK/SCLK/LRCK/DOUT/SDIN` |
| ES7210 address | `ES7210_CODEC_DEFAULT_ADDR` (0x80 8-bit, 0x40 7-bit) | `bsp_board.c` `audio_codec_i2c_cfg_t.addr` |
| ES7210 inputs | `MIC1 \| MIC2 \| MIC3 \| MIC4` | `bsp_board.c` `es7210_cfg.mic_selected` |
| ES7210 input gain | 30.0 dB on channels 0..3 | `ESP32_S3_AUDIO_Board.h` `RECORD_VOLUME`, applied in `bsp_codec_adc_init` |
| ES7210 channel count | 4 | `bsp_board.c` `ADC_I2S_CHANNEL` |
| ES8311 address | `ES8311_CODEC_DEFAULT_ADDR` (0x30 8-bit, 0x18 7-bit) | `bsp_board.c` `audio_codec_i2c_cfg_t.addr` |
| ES8311 mode | `ESP_CODEC_DEV_WORK_MODE_DAC` | `bsp_board.c` `es8311_cfg.codec_mode` |
| ES8311 `use_mclk` | false | `bsp_board.c` `es8311_cfg.use_mclk` |
| ES8311 `pa_pin` | -1 (codec never drives the amplifier) | `bsp_board.c` `es8311_cfg.pa_pin = GPIO_PWR_CTRL`, and `ESP32_S3_AUDIO_Board.h` `#define GPIO_PWR_CTRL (-1)` |
| NS4150B enable | GPIO15, high = on, low = off, initialized low | `main/Audio_Driver/Audio_Driver.c` `pipeline_init()`, `Audio_PA_EN()`, `Audio_PA_DIS()` |

### Deliberate deviations from upstream

These are the only audio values that differ from the pinned source. Both reduce
output, and both exist because M1 is a bounded hardware diagnostic rather than
playback.

| Setting | Upstream | Here | Reason |
|---|---|---|---|
| ES8311 output volume | `PLAYER_VOLUME` 50 | `CONFIG_ZV_M1_PLAYBACK_VOLUME`, default 20 | A diagnostic tone must be plainly audible without being loud. |
| Tone amplitude | not applicable; upstream plays MP3 assets | `CONFIG_ZV_M1_TONE_AMPLITUDE_PERMILLE`, default 60, clamped in code to 200 | Bounds the digital level independently of the codec volume. |
| Volume call ordering | `esp_codec_dev_set_out_vol()` before `esp_codec_dev_open()` | after `esp_codec_dev_open()`, plus a read-back assertion | Before open, `es8311_set_vol()` returns `ESP_CODEC_DEV_WRONG_STATE` and `esp_codec_dev_set_out_vol()` discards that inner status, so upstream never writes the volume register and the part runs at its power-on default. Calling after open is what reaches `ES8311_DAC_REG32`. The read-back is a defensive API-contract check only: `esp_codec_dev_get_out_vol()` returns the cached value, not the register. |

Upstream additionally ignores most codec and I2C return values and leaves its
amplifier control commented out. This firmware checks every return and owns the
amplifier explicitly, which is a safety change, not a configuration change.
