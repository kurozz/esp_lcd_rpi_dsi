# Changelog

All notable changes to this component are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project uses
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- ESP-IDF v5.5 support, from v5.5.4. From v5.5.5 the DSI clock lane is pinned to high speed by the
  `clock_lane_force_hs` bus flag, as on 6.x; on v5.5.4, which lacks that flag, the panel pins it after the
  DPI stream starts. On 5.5, DMA2D is enabled through the DPI panel config and the LCD HAL comes from `hal`
  rather than `esp_hal_lcd`.
- `esp_lcd_rpi_dsi_vendor_config_t::flags.continuous_clock`, used only before ESP-IDF v5.5.5.
- `esp_lcd_rpi_dsi_config_t::num_fbs`, the number of screen-sized frame buffers
  the DPI panel allocates, passed to `esp_lcd_dpi_panel_config_t::num_fbs`.
  `ESP_LCD_RPI_DSI_DEFAULT_CONFIG()` sets it to 1, the IDF's default.

### Fixed

- `esp_lcd_rpi_dsi_stop()` disables DMA2D before deleting the DPI panel on ESP-IDF 6.x, which refuses to delete a
  panel with DMA2D still enabled. The panel's frame buffers and its DSI bridge interrupt are released, so
  `esp_lcd_rpi_dsi_start()` works again after a stop. The same applies to the cleanup after a failed start.
- `esp_lcd_rpi_dsi_stop()` returns the first error of the teardown instead of always `ESP_OK`, and still goes
  through the remaining steps after a failure.
- Deleting the panel keeps its context when the DPI panel refuses to be deleted, so the panel can still be
  deleted again.

## [0.1.0] - 2026-08-29

Initial release.

### Added

- `esp_lcd_rpi_dsi_start()`, which brings the display up in one call: MIPI DSI PHY
  power, the DSI bus with a continuous clock lane, the I2C bus, the panel power
  and reset sequencing, the bridge initialisation, the backlight, and the touch
  controller.
- Panel-level access through `esp_lcd_rpi_dsi_get_panel()`, the shared I2C bus
  through `esp_lcd_rpi_dsi_get_i2c_bus()`, backlight control through
  `esp_lcd_rpi_dsi_set_brightness()`, and the touch controller through
  `esp_lcd_rpi_dsi_get_touch()`, which returns the `esp_lcd_touch_handle_t` from
  `espressif/esp_lcd_touch_ft5x06`.
- Two selectable 800x480 timings: the Raspberry Pi legacy mode and the roomier
  Waveshare-style mode.
- Support for both panel controller firmware revisions, `0xde` and `0xc3`.
