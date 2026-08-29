/*
 * SPDX-FileCopyrightText: 2026 Kuro
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * One-call bring-up: PHY power, DSI bus, I2C, panel, touch.
 */

#include "soc/soc_caps.h"

#if SOC_MIPI_DSI_SUPPORTED
#include <stdlib.h>
#include "esp_check.h"
#include "esp_log.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_rpi_dsi_panel.h"
#include "esp_lcd_io_i2c.h"
#include "esp_lcd_touch_ft5x06.h"

static const char *TAG = "rpi_dsi";

struct esp_lcd_rpi_dsi_t {
    esp_ldo_channel_handle_t ldo;
    i2c_master_bus_handle_t i2c_bus;
    bool owns_i2c_bus;
    esp_lcd_dsi_bus_handle_t dsi_bus;
    esp_lcd_panel_io_handle_t dbi_io;
    esp_lcd_panel_handle_t panel;
    esp_lcd_panel_io_handle_t touch_io;
    esp_lcd_touch_handle_t touch;
};

/* The touch controller is an ordinary FT5x06 on the same bus, so it is left to
 * espressif/esp_lcd_touch_ft5x06 rather than reimplemented here. Its reset line is
 * the one thing that is ours: PC_RST_TP_N on the ATTINY, released by the panel's
 * power-on sequence, so this has to run after the panel is up. */
static esp_err_t rpi_dsi_touch_start(esp_lcd_rpi_dsi_handle_t disp, uint16_t h_res, uint16_t v_res)
{
    esp_lcd_panel_io_i2c_config_t io_config = ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(disp->i2c_bus, &io_config, &disp->touch_io), TAG,
                        "creating the touch panel IO failed");

    esp_lcd_touch_config_t touch_config = {
        .x_max = h_res,
        .y_max = v_res,
        .rst_gpio_num = GPIO_NUM_NC, // held by the ATTINY, not by a GPIO of ours
        .int_gpio_num = GPIO_NUM_NC,
        .flags = {
            .swap_xy = CONFIG_ESP_LCD_RPI_DSI_TOUCH_SWAP_XY_VAL,
            .mirror_x = CONFIG_ESP_LCD_RPI_DSI_TOUCH_INVERT_X_VAL,
            .mirror_y = CONFIG_ESP_LCD_RPI_DSI_TOUCH_INVERT_Y_VAL,
        },
    };
    return esp_lcd_touch_new_i2c_ft5x06(disp->touch_io, &touch_config, &disp->touch);
}

esp_err_t esp_lcd_rpi_dsi_start(const esp_lcd_rpi_dsi_config_t *config,
                                esp_lcd_rpi_dsi_handle_t *ret_handle)
{
    ESP_RETURN_ON_FALSE(config && ret_handle, ESP_ERR_INVALID_ARG, TAG, "invalid arguments");

    esp_err_t ret = ESP_OK;
    esp_lcd_rpi_dsi_handle_t disp = calloc(1, sizeof(struct esp_lcd_rpi_dsi_t));
    ESP_RETURN_ON_FALSE(disp, ESP_ERR_NO_MEM, TAG, "no mem for the display");

    ESP_LOGI(TAG, "%dx%d, 1 lane @ %g Mbps, DPI clock %g MHz",
             (int)config->timing.h_size, (int)config->timing.v_size,
             (double)config->lane_bit_rate_mbps, (double)config->dpi_clock_freq_mhz);
    ESP_LOGI(TAG, "timing: hsync=%d hbp=%d hfp=%d / vsync=%d vbp=%d vfp=%d",
             (int)config->timing.hsync_pulse_width, (int)config->timing.hsync_back_porch,
             (int)config->timing.hsync_front_porch, (int)config->timing.vsync_pulse_width,
             (int)config->timing.vsync_back_porch, (int)config->timing.vsync_front_porch);
    ESP_LOGI(TAG, "flags: continuous_clock=%d non_burst=%d disable_lp=%d touch=%d",
             config->flags.continuous_clock, config->flags.non_burst_video,
             config->flags.disable_lp, config->flags.enable_touch);

    // VDD_MIPI_DPHY, without which the PHY never leaves its "no power" state
    if (config->ldo_chan >= 0) {
        esp_ldo_channel_config_t ldo_config = {
            .chan_id = config->ldo_chan,
            .voltage_mv = config->ldo_voltage_mv ? config->ldo_voltage_mv : 2500,
        };
        ESP_GOTO_ON_ERROR(esp_ldo_acquire_channel(&ldo_config, &disp->ldo), err, TAG,
                          "powering the MIPI DSI PHY failed");
    }

    if (config->i2c.bus) {
        disp->i2c_bus = config->i2c.bus;
    } else {
        i2c_master_bus_config_t i2c_config = {
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .i2c_port = -1,
            .sda_io_num = config->i2c.sda_gpio,
            .scl_io_num = config->i2c.scl_gpio,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,
        };
        ESP_GOTO_ON_ERROR(i2c_new_master_bus(&i2c_config, &disp->i2c_bus), err, TAG,
                          "creating the I2C bus on SDA=%d SCL=%d failed",
                          config->i2c.sda_gpio, config->i2c.scl_gpio);
        disp->owns_i2c_bus = true;
    }

    esp_lcd_dsi_bus_config_t bus_config = {
        .bus_id = config->dsi_bus_id,
        .num_data_lanes = 1,
        .lane_bit_rate_mbps = config->lane_bit_rate_mbps,
        // the bridge makes its pixel clock out of the DSI clock lane, so that lane
        // cannot be parked in low-power between packets
        .flags.clock_lane_force_hs = config->flags.continuous_clock,
    };
    ESP_GOTO_ON_ERROR(esp_lcd_new_dsi_bus(&bus_config, &disp->dsi_bus), err, TAG, "creating the DSI bus failed");

    // No DCS command is ever sent through this, but creating it is what switches
    // the host's generic packet path to low-power mode, which the bridge needs
    esp_lcd_dbi_io_config_t dbi_config = {
        .virtual_channel = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_io_dbi(disp->dsi_bus, &dbi_config, &disp->dbi_io), err, TAG,
                      "creating the DBI IO failed");

    esp_lcd_dpi_panel_config_t dpi_config = {
        .virtual_channel = 0,
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = config->dpi_clock_freq_mhz,
        .in_color_format = LCD_COLOR_FMT_RGB888,
        .video_timing = config->timing,
        .flags.disable_lp = config->flags.disable_lp,
    };
    esp_lcd_rpi_dsi_vendor_config_t vendor_config = {
        .mipi_config = {
            .dsi_bus = disp->dsi_bus,
            .dpi_config = &dpi_config,
        },
        .i2c_bus = disp->i2c_bus,
        .brightness = config->brightness,
        .flags.non_burst_video = config->flags.non_burst_video,
    };
    esp_lcd_panel_dev_config_t panel_dev_config = {
        .reset_gpio_num = -1,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 24,
        .vendor_config = &vendor_config,
    };
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_rpi_dsi(disp->dbi_io, &panel_dev_config, &disp->panel), err, TAG,
                      "creating the panel failed");

    if (config->flags.use_dma2d) {
        ESP_GOTO_ON_ERROR(esp_lcd_dpi_panel_enable_dma2d(disp->panel), err, TAG, "enabling DMA2D failed");
    }

    ESP_GOTO_ON_ERROR(esp_lcd_panel_reset(disp->panel), err, TAG, "panel reset failed");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_init(disp->panel), err, TAG, "panel init failed");

    if (config->flags.enable_touch) {
        // Non-fatal: a display with a dead or absent touch controller is still a display
        if (rpi_dsi_touch_start(disp, config->timing.h_size, config->timing.v_size) != ESP_OK) {
            ESP_LOGW(TAG, "no touch controller found, continuing without it");
            disp->touch = NULL;
        }
    }

    *ret_handle = disp;
    return ESP_OK;

err:
    esp_lcd_rpi_dsi_stop(disp);
    return ret;
}

esp_err_t esp_lcd_rpi_dsi_stop(esp_lcd_rpi_dsi_handle_t handle)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "invalid handle");

    if (handle->touch) {
        esp_lcd_touch_del(handle->touch);
    }
    if (handle->touch_io) {
        esp_lcd_panel_io_del(handle->touch_io);
    }
    if (handle->panel) {
        esp_lcd_panel_del(handle->panel);
    }
    if (handle->dbi_io) {
        esp_lcd_panel_io_del(handle->dbi_io);
    }
    if (handle->dsi_bus) {
        esp_lcd_del_dsi_bus(handle->dsi_bus);
    }
    if (handle->owns_i2c_bus && handle->i2c_bus) {
        i2c_del_master_bus(handle->i2c_bus);
    }
    if (handle->ldo) {
        esp_ldo_release_channel(handle->ldo);
    }
    free(handle);
    return ESP_OK;
}

esp_lcd_panel_handle_t esp_lcd_rpi_dsi_get_panel(esp_lcd_rpi_dsi_handle_t handle)
{
    return handle ? handle->panel : NULL;
}

i2c_master_bus_handle_t esp_lcd_rpi_dsi_get_i2c_bus(esp_lcd_rpi_dsi_handle_t handle)
{
    return handle ? handle->i2c_bus : NULL;
}

esp_err_t esp_lcd_rpi_dsi_set_brightness(esp_lcd_rpi_dsi_handle_t handle, uint8_t brightness)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "invalid handle");
    return esp_lcd_panel_rpi_dsi_set_brightness(handle->panel, brightness);
}

esp_lcd_touch_handle_t esp_lcd_rpi_dsi_get_touch(esp_lcd_rpi_dsi_handle_t handle)
{
    return handle ? handle->touch : NULL;
}

#endif // SOC_MIPI_DSI_SUPPORTED
