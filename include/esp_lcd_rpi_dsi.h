/*
 * SPDX-FileCopyrightText: 2026 Kuro
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Driver for the Raspberry Pi DSI touch display (and its clones, e.g. the
 * 5" OSOYOO): a TC358762-compatible DSI-to-DPI bridge in front of an 800x480
 * RGB888 panel, with an ATTINY-compatible controller on I2C 0x45 doing power
 * sequencing and backlight PWM. What is physically on the board varies -- the
 * Raspberry Pi display carries a Toshiba TC358762 and an ATTINY88, clones such
 * as the OSOYOO emulate both in an FPGA -- so this talks to the interface
 * rather than to the parts.
 *
 * The panel speaks no DCS at all. The bridge is configured with MIPI DSI
 * *generic* long writes, and nothing on the flex comes up until the controller
 * has been told to power the panel rail. The sequence implemented here is
 * the one from the legacy Linux driver
 * drivers/gpu/drm/panel/panel-raspberrypi-touchscreen.c, which mirrors what
 * the Raspberry Pi firmware does under FKMS -- deliberately *not* the newer
 * drivers/gpu/drm/bridge/tc358762.c path, which additionally programs the
 * bridge's own LCD_* timing registers and is the one that produces wrong
 * colors on these clone panels.
 *
 * Two things are needed that neither kernel driver makes obvious, and without
 * either one the panel just sits there white with its backlight on:
 *
 *  - The bridge and LCD resets have to be released explicitly. In Linux that is
 *    not part of the panel power-on at all: it happens when the bridge driver
 *    probes, through a regulator hanging off the controller's gpio_chip, whose
 *    GPIO 0 drives PC_RST_BRIDGE_N | PC_RST_LCD_N. See rpi_dsi_power_on().
 *  - The DSI clock lane must stay in high speed continuously. The bridge builds
 *    its DPI pixel clock from that lane, so the IDF default of parking it in
 *    low-power between packets stops the panel cold. Set
 *    esp_lcd_dsi_bus_config_t::flags.clock_lane_force_hs.
 *
 * Verified working at 800x480 with the legacy timing, one data lane, burst mode
 * and low-power blanking transitions left at the IDF defaults.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "soc/soc_caps.h"

#if SOC_MIPI_DSI_SUPPORTED
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_mipi_dsi.h"
#include "driver/i2c_master.h"
#include "esp_lcd_touch.h"

#ifdef __cplusplus
extern "C" {
#endif

/// I2C address of the panel controller on the display board
#define ESP_LCD_RPI_DSI_ATTINY_ADDR 0x45

#define ESP_LCD_RPI_DSI_H_RES 800
#define ESP_LCD_RPI_DSI_V_RES 480

/* Timing from the legacy Linux driver's display mode:
 * 25979400 / ((800+1+2+46) * (480+7+2+21)) = 60.0 Hz */
#define ESP_LCD_RPI_DSI_LEGACY_CLK_MHZ 26
#define ESP_LCD_RPI_DSI_LEGACY_HSYNC   2
#define ESP_LCD_RPI_DSI_LEGACY_HBP     46
#define ESP_LCD_RPI_DSI_LEGACY_HFP     1
#define ESP_LCD_RPI_DSI_LEGACY_VSYNC   2
#define ESP_LCD_RPI_DSI_LEGACY_VBP     21
#define ESP_LCD_RPI_DSI_LEGACY_VFP     7

/* Timing from vc4-kms-dsi-waveshare-800x480.dtbo: the same bridge with roomier
 * porches, 27777000 / ((800+59+2+45) * (480+7+2+22)) = 60 Hz */
#define ESP_LCD_RPI_DSI_WS_CLK_MHZ 28
#define ESP_LCD_RPI_DSI_WS_HSYNC   2
#define ESP_LCD_RPI_DSI_WS_HBP     45
#define ESP_LCD_RPI_DSI_WS_HFP     59
#define ESP_LCD_RPI_DSI_WS_VSYNC   2
#define ESP_LCD_RPI_DSI_WS_VBP     22
#define ESP_LCD_RPI_DSI_WS_VFP     7

#if CONFIG_ESP_LCD_RPI_DSI_TIMING_WAVESHARE
#define ESP_LCD_RPI_DSI_DEFAULT_TIMING()  \
    {                                     \
        .h_size = ESP_LCD_RPI_DSI_H_RES,  \
        .v_size = ESP_LCD_RPI_DSI_V_RES,  \
        .hsync_pulse_width = ESP_LCD_RPI_DSI_WS_HSYNC, \
        .hsync_back_porch = ESP_LCD_RPI_DSI_WS_HBP,    \
        .hsync_front_porch = ESP_LCD_RPI_DSI_WS_HFP,   \
        .vsync_pulse_width = ESP_LCD_RPI_DSI_WS_VSYNC, \
        .vsync_back_porch = ESP_LCD_RPI_DSI_WS_VBP,    \
        .vsync_front_porch = ESP_LCD_RPI_DSI_WS_VFP,   \
    }
#define ESP_LCD_RPI_DSI_DEFAULT_CLK_MHZ ESP_LCD_RPI_DSI_WS_CLK_MHZ
#else
#define ESP_LCD_RPI_DSI_DEFAULT_TIMING()  \
    {                                     \
        .h_size = ESP_LCD_RPI_DSI_H_RES,  \
        .v_size = ESP_LCD_RPI_DSI_V_RES,  \
        .hsync_pulse_width = ESP_LCD_RPI_DSI_LEGACY_HSYNC, \
        .hsync_back_porch = ESP_LCD_RPI_DSI_LEGACY_HBP,    \
        .hsync_front_porch = ESP_LCD_RPI_DSI_LEGACY_HFP,   \
        .vsync_pulse_width = ESP_LCD_RPI_DSI_LEGACY_VSYNC, \
        .vsync_back_porch = ESP_LCD_RPI_DSI_LEGACY_VBP,    \
        .vsync_front_porch = ESP_LCD_RPI_DSI_LEGACY_VFP,   \
    }
#define ESP_LCD_RPI_DSI_DEFAULT_CLK_MHZ ESP_LCD_RPI_DSI_LEGACY_CLK_MHZ
#endif

/**
 * @brief Everything the display needs to come up
 *
 * Fill with ESP_LCD_RPI_DSI_DEFAULT_CONFIG(), which takes its values from Kconfig,
 * then override whatever your board needs.
 */
typedef struct {
    struct {
        int sda_gpio;                 /*!< SDA on the DSI flex, ignored when `bus` is set */
        int scl_gpio;                 /*!< SCL on the DSI flex, ignored when `bus` is set */
        i2c_master_bus_handle_t bus;  /*!< Existing I2C bus to reuse. NULL to have one created */
    } i2c;
    int ldo_chan;                     /*!< Internal LDO channel for VDD_MIPI_DPHY, -1 if supplied elsewhere */
    int ldo_voltage_mv;               /*!< LDO voltage, normally 2500 */
    uint8_t dsi_bus_id;               /*!< DSI controller index */
    float lane_bit_rate_mbps;         /*!< Bit rate on the single data lane */
    float dpi_clock_freq_mhz;         /*!< Pixel clock */
    esp_lcd_video_timing_t timing;    /*!< Video timing */
    uint8_t brightness;               /*!< Backlight PWM applied once the panel is up, 0-255 */
    struct {
        uint32_t continuous_clock: 1; /*!< Keep the clock lane in HS. Required, see the notes above */
        uint32_t non_burst_video: 1;  /*!< Non-burst with sync pulses instead of the IDF's burst default */
        uint32_t disable_lp: 1;       /*!< Forbid low-power transitions during blanking */
        uint32_t enable_touch: 1;     /*!< Bring the FT5x06 up too, through esp_lcd_touch_ft5x06 */
        uint32_t use_dma2d: 1;        /*!< Let DMA2D do the copy into the frame buffer */
    } flags;
} esp_lcd_rpi_dsi_config_t;

#define ESP_LCD_RPI_DSI_DEFAULT_CONFIG()                                     \
    {                                                                        \
        .i2c = {                                                             \
            .sda_gpio = CONFIG_ESP_LCD_RPI_DSI_I2C_SDA_GPIO,                 \
            .scl_gpio = CONFIG_ESP_LCD_RPI_DSI_I2C_SCL_GPIO,                 \
            .bus = NULL,                                                     \
        },                                                                   \
        .ldo_chan = CONFIG_ESP_LCD_RPI_DSI_LDO_CHAN,                         \
        .ldo_voltage_mv = 2500,                                              \
        .dsi_bus_id = 0,                                                     \
        .lane_bit_rate_mbps = 700,                                           \
        .dpi_clock_freq_mhz = ESP_LCD_RPI_DSI_DEFAULT_CLK_MHZ,               \
        .timing = ESP_LCD_RPI_DSI_DEFAULT_TIMING(),                          \
        .brightness = 255,                                                   \
        .flags = {                                                           \
            .continuous_clock = CONFIG_ESP_LCD_RPI_DSI_CONTINUOUS_CLOCK_VAL, \
            .non_burst_video = CONFIG_ESP_LCD_RPI_DSI_NON_BURST_VIDEO_VAL,   \
            .disable_lp = CONFIG_ESP_LCD_RPI_DSI_DISABLE_LP_VAL,             \
            .enable_touch = CONFIG_ESP_LCD_RPI_DSI_TOUCH_VAL,                \
            .use_dma2d = CONFIG_ESP_LCD_RPI_DSI_USE_DMA2D_VAL,               \
        },                                                                   \
    }

/* Kconfig bools are either defined as 1 or not defined at all, which a struct
 * initialiser cannot use directly */
#ifdef CONFIG_ESP_LCD_RPI_DSI_CONTINUOUS_CLOCK
#define CONFIG_ESP_LCD_RPI_DSI_CONTINUOUS_CLOCK_VAL 1
#else
#define CONFIG_ESP_LCD_RPI_DSI_CONTINUOUS_CLOCK_VAL 0
#endif
#ifdef CONFIG_ESP_LCD_RPI_DSI_NON_BURST_VIDEO
#define CONFIG_ESP_LCD_RPI_DSI_NON_BURST_VIDEO_VAL 1
#else
#define CONFIG_ESP_LCD_RPI_DSI_NON_BURST_VIDEO_VAL 0
#endif
#ifdef CONFIG_ESP_LCD_RPI_DSI_DISABLE_LP
#define CONFIG_ESP_LCD_RPI_DSI_DISABLE_LP_VAL 1
#else
#define CONFIG_ESP_LCD_RPI_DSI_DISABLE_LP_VAL 0
#endif
#ifdef CONFIG_ESP_LCD_RPI_DSI_TOUCH
#define CONFIG_ESP_LCD_RPI_DSI_TOUCH_VAL 1
#else
#define CONFIG_ESP_LCD_RPI_DSI_TOUCH_VAL 0
#endif
#ifdef CONFIG_ESP_LCD_RPI_DSI_USE_DMA2D
#define CONFIG_ESP_LCD_RPI_DSI_USE_DMA2D_VAL 1
#else
#define CONFIG_ESP_LCD_RPI_DSI_USE_DMA2D_VAL 0
#endif
#ifdef CONFIG_ESP_LCD_RPI_DSI_TOUCH_SWAP_XY
#define CONFIG_ESP_LCD_RPI_DSI_TOUCH_SWAP_XY_VAL 1
#else
#define CONFIG_ESP_LCD_RPI_DSI_TOUCH_SWAP_XY_VAL 0
#endif
#ifdef CONFIG_ESP_LCD_RPI_DSI_TOUCH_INVERT_X
#define CONFIG_ESP_LCD_RPI_DSI_TOUCH_INVERT_X_VAL 1
#else
#define CONFIG_ESP_LCD_RPI_DSI_TOUCH_INVERT_X_VAL 0
#endif
#ifdef CONFIG_ESP_LCD_RPI_DSI_TOUCH_INVERT_Y
#define CONFIG_ESP_LCD_RPI_DSI_TOUCH_INVERT_Y_VAL 1
#else
#define CONFIG_ESP_LCD_RPI_DSI_TOUCH_INVERT_Y_VAL 0
#endif

/// Opaque handle to a running display
typedef struct esp_lcd_rpi_dsi_t *esp_lcd_rpi_dsi_handle_t;

/**
 * @brief Bring the whole display up: PHY power, DSI bus, I2C, panel power
 *        sequencing, bridge init, backlight and, optionally, the touch controller
 *
 * @param[in]  config Configuration, normally ESP_LCD_RPI_DSI_DEFAULT_CONFIG()
 * @param[out] ret_handle Returned display handle
 */
esp_err_t esp_lcd_rpi_dsi_start(const esp_lcd_rpi_dsi_config_t *config,
                                esp_lcd_rpi_dsi_handle_t *ret_handle);

/**
 * @brief Power the panel down and release everything esp_lcd_rpi_dsi_start() took
 *
 * @note An I2C bus supplied through config.i2c.bus is left alone; one created by
 *       the driver is deleted.
 */
esp_err_t esp_lcd_rpi_dsi_stop(esp_lcd_rpi_dsi_handle_t handle);

/// The LCD panel, for esp_lcd_panel_draw_bitmap() and friends
esp_lcd_panel_handle_t esp_lcd_rpi_dsi_get_panel(esp_lcd_rpi_dsi_handle_t handle);

/// The I2C bus the panel is on, so callers can share it
i2c_master_bus_handle_t esp_lcd_rpi_dsi_get_i2c_bus(esp_lcd_rpi_dsi_handle_t handle);

/// Backlight level, 0 (off) to 255 (full)
esp_err_t esp_lcd_rpi_dsi_set_brightness(esp_lcd_rpi_dsi_handle_t handle, uint8_t brightness);

/**
 * @brief The touch controller, driven by espressif/esp_lcd_touch_ft5x06
 *
 * Read it with esp_lcd_touch_read_data() and esp_lcd_touch_get_coordinates(), or hand
 * it straight to lvgl_port_add_touch(). NULL when touch is disabled or was not found.
 */
esp_lcd_touch_handle_t esp_lcd_rpi_dsi_get_touch(esp_lcd_rpi_dsi_handle_t handle);

#ifdef __cplusplus
}
#endif

#endif // SOC_MIPI_DSI_SUPPORTED
