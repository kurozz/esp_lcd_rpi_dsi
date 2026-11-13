# The LCD HAL became its own component (esp_hal_lcd) in ESP-IDF 6; before that it is part of `hal`
if(IDF_VERSION_MAJOR GREATER_EQUAL 6)
    set(rpi_dsi_lcd_hal esp_hal_lcd)
else()
    set(rpi_dsi_lcd_hal hal)
endif()

idf_component_register(SRCS "esp_lcd_rpi_dsi.c" "esp_lcd_rpi_dsi_panel.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_lcd esp_driver_i2c esp_hw_support esp_lcd_touch_ft5x06
                       PRIV_REQUIRES ${rpi_dsi_lcd_hal})
