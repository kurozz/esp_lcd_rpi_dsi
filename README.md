# esp_lcd_rpi_dsi

Raspberry Pi DSI touch display (and clones such as the 5" OSOYOO) on the ESP32-P4.

800x480 RGB888 over a single MIPI DSI data lane, through a TC358762-compatible
DSI-to-DPI bridge, with an ATTINY-compatible controller on I2C 0x45 doing power
sequencing, resets and backlight PWM, and an FT5x06 touch controller on I2C 0x38.

What is physically on the board varies. The Raspberry Pi display carries a Toshiba
TC358762 and an ATTINY88; clones such as the OSOYOO emulate both in an FPGA. This
driver talks to the interface, not to the parts, so either works.

## Use

Drop the directory into your project's `components/`, then:

```c
#include "esp_lcd_rpi_dsi.h"

esp_lcd_rpi_dsi_config_t config = ESP_LCD_RPI_DSI_DEFAULT_CONFIG();
esp_lcd_rpi_dsi_handle_t display;

ESP_ERROR_CHECK(esp_lcd_rpi_dsi_start(&config, &display));

esp_lcd_panel_handle_t panel = esp_lcd_rpi_dsi_get_panel(display);
```

That one call powers the DSI PHY, creates the I2C bus, brings the panel and the
bridge up, turns the backlight on, and attaches the touch controller. What you get
back is an ordinary `esp_lcd_panel_handle_t`, so `esp_lcd_panel_draw_bitmap()` and
`esp_lcd_dpi_panel_get_frame_buffer()` work as they do with any other panel driver.

If you write into the frame buffer directly rather than through
`esp_lcd_panel_draw_bitmap()`, follow it with
`esp_cache_msync(fb, size, ESP_CACHE_MSYNC_FLAG_DIR_C2M)`: the buffer is cached and the
DSI DMA reads PSRAM, so anything still dirty in L2 never reaches the panel. See
`examples/panel_demo`.

Touch is handled by `espressif/esp_lcd_touch_ft5x06`, not reimplemented here;
`esp_lcd_rpi_dsi_get_touch()` returns its `esp_lcd_touch_handle_t`, which is also what
`lvgl_port_add_touch()` expects. Also available: `esp_lcd_rpi_dsi_get_i2c_bus()` to
share the bus, and `esp_lcd_rpi_dsi_set_brightness()` for the backlight.

Defaults come from `Component config -> Raspberry Pi DSI display`; the config struct
overrides any of them per call.

## Two things worth knowing

Both were needed to get a picture, and neither is obvious from the Linux drivers:

- **The bridge and LCD resets have to be released explicitly.** In Linux this is
  not part of the panel power-on; it happens when the bridge driver probes, via a
  regulator hanging off the controller's gpio_chip, whose GPIO 0 drives
  `PC_RST_BRIDGE_N | PC_RST_LCD_N`. Leaving it out gives a white screen with a
  working backlight.
- **The DSI clock lane must stay in high speed continuously.** The bridge derives
  its DPI pixel clock from that lane, so the IDF default of parking it in low-power
  between packets stops the panel cold. Hence `clock_lane_force_hs`.

Non-burst video mode and disabling low-power blanking transitions are both exposed
as Kconfig knobs, and both measured unnecessary once the clock lane is continuous.

## Bring-up notes

The panel is initialised the way the legacy
`drivers/gpu/drm/panel/panel-raspberrypi-touchscreen.c` does it, deliberately not
the newer `tc358762.c` path, which also programs the bridge's `LCD_*` timing
registers and produces wrong colours on clone panels.

The controller's `PORTA`/`PORTB`/`PORTC` are write-only; reading them back returns
values unrelated to what was written. `REG_ID` is readable and is the only reliable
way to tell the firmware revisions apart: `0xde` is revision 1, `0xc3` revision 2.
Both are driven with the same port sequence here.

## License

GPL-2.0-only.

## Requirements

ESP-IDF v6.0 or newer, ESP32-P4. Pulls in `espressif/esp_lcd_touch_ft5x06` for the
touch controller.

Verified on an ESP32-P4-Function-EV-Board v1.5.2 (chip revision v1.3) under ESP-IDF
v6.1, driving a 5" OSOYOO panel v1.1 with firmware revision 2 (`REG_ID` `0xc3`). It
builds clean on v6.0, but has not been run there. The revision 1 firmware path is
ported from the kernel driver and has not been run at all.
