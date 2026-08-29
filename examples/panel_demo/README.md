| Supported Targets | ESP32-P4 |
| ----------------- | -------- |

# Raspberry Pi DSI panel demo

Brings the display up with a single `esp_lcd_rpi_dsi_start()` call, paints colour
bars straight into the frame buffer, and logs the coordinates of every touch.

No UI toolkit is involved. This component is a panel driver: it hands back an
`esp_lcd_panel_handle_t` and leaves LVGL, or anything else, to the application.

## Hardware

An ESP32-P4 board whose 15-pin MIPI DSI FPC connector follows the Raspberry Pi
pinout, and the display connected by its flex cable, which also carries the I2C bus
the panel controller and the touch controller sit on.

The I2C pins default to GPIO7 (SDA) and GPIO8 (SCL); change them under
`Component config -> Raspberry Pi DSI display` if your board differs.

## Build and flash

```
idf.py set-target esp32p4
idf.py build flash monitor
```

On an ESP32-P4 earlier than revision v3.0, also set `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y`,
`CONFIG_ESP32P4_REV_MIN_100=y` and `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_360=y`.
