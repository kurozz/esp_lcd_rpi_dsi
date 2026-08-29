/*
 * SPDX-FileCopyrightText: 2026 Kuro
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Panel-level API, used by esp_lcd_rpi_dsi.c. Callers who want the display to
 * just come up should use esp_lcd_rpi_dsi_start() instead; this is here for the
 * case where you need to own the DSI bus and the DPI configuration yourself.
 */

#pragma once

#include "esp_lcd_rpi_dsi.h"

#if SOC_MIPI_DSI_SUPPORTED

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    struct {
        esp_lcd_dsi_bus_handle_t dsi_bus;
        const esp_lcd_dpi_panel_config_t *dpi_config;
    } mipi_config;
    i2c_master_bus_handle_t i2c_bus; /*!< bus the panel controller is on */
    uint8_t i2c_addr;                /*!< 0 falls back to ESP_LCD_RPI_DSI_ATTINY_ADDR */
    uint32_t i2c_scl_speed_hz;       /*!< 0 falls back to 100 kHz */
    uint8_t brightness;              /*!< 0 falls back to 255 */
    struct {
        uint32_t non_burst_video: 1;
    } flags;
} esp_lcd_rpi_dsi_vendor_config_t;

/**
 * @brief Create the panel
 *
 * @param[in] io Panel IO from esp_lcd_new_panel_io_dbi(). No DCS command is ever sent
 *               through it, but creating it is what puts the DSI host's generic packet
 *               path into low-power mode, which the bridge requires.
 */
esp_err_t esp_lcd_new_panel_rpi_dsi(esp_lcd_panel_io_handle_t io,
                                    const esp_lcd_panel_dev_config_t *panel_dev_config,
                                    esp_lcd_panel_handle_t *ret_panel);

esp_err_t esp_lcd_panel_rpi_dsi_set_brightness(esp_lcd_panel_handle_t panel, uint8_t brightness);

#ifdef __cplusplus
}
#endif

#endif // SOC_MIPI_DSI_SUPPORTED
