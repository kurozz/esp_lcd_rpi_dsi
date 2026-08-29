idf_component_register(SRCS "esp_lcd_rpi_dsi.c" "esp_lcd_rpi_dsi_panel.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_lcd esp_driver_i2c esp_hw_support esp_lcd_touch_ft5x06
                       PRIV_REQUIRES esp_hal_lcd)
