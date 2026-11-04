# Vendored Espressif ST77916 panel driver, Apache-2.0.
# Copied verbatim from the pinned Waveshare revision
# 8ead4a96bf3a278fc4ebd8ef4768657e17fa2880 at
# ESP-IDF/ESP32-S3-Touch-LCD-1.85C-Test/main/LCD_Driver/esp_lcd_st77916/.
# It is vendored rather than pulled from the registry because the pinned copy
# carries the exact default init table this board was validated against.
idf_component_register(
    SRCS
        "src/esp_lcd_st77916.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        esp_lcd
        esp_driver_gpio
        driver
)
