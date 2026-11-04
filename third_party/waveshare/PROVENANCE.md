# Waveshare source provenance

Hardware facts were inspected from the sibling `../waveshare-v2-source` tree
at exact Git revision `8ead4a96bf3a278fc4ebd8ef4768657e17fa2880`.
The source root `LICENSE` is Apache License 2.0 and is preserved as `LICENSE`
in this directory. This repository supports Rev2 only.

One piece of implementation source **is** copied: the Espressif ST77916 panel
driver (Apache-2.0, `SPDX-FileCopyrightText: 2023 Espressif Systems (Shanghai)
CO LTD`), vendored verbatim from the pinned tree at
`main/LCD_Driver/esp_lcd_st77916/` into `components/zv_lcd_st77916/`. Upstream
vendors the same driver rather than pulling it from the component registry, and
its copy carries the exact default init table this board was validated against.
`tools/validate.py` check `lcd-driver-vendored-verbatim` re-hashes both files
against the pinned tree on every run. No dependency pin is invented.
