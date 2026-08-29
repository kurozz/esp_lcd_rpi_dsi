/*
 * SPDX-FileCopyrightText: 2026 Kuro
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Brings the Raspberry Pi DSI display up in one call, paints colour bars straight
 * into the frame buffer, and reports where the panel is touched. No UI toolkit is
 * involved: this is the whole surface the component offers.
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_cache.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_rpi_dsi.h"
#include "esp_lcd_touch.h"

static const char *TAG = "example";

static const uint32_t bar_colors[] = {
    0xffffff, 0xffff00, 0x00ffff, 0x00ff00, 0xff00ff, 0xff0000, 0x0000ff, 0x000000,
};
#define BAR_COUNT (sizeof(bar_colors) / sizeof(bar_colors[0]))

static void paint_colour_bars(uint8_t *fb, int h_res, int v_res)
{
    // The panel is fed RGB888, so three bytes per pixel, blue first
    for (int y = 0; y < v_res; y++) {
        uint8_t *row = fb + (size_t)y * h_res * 3;
        for (int x = 0; x < h_res; x++) {
            uint32_t colour = bar_colors[(x * BAR_COUNT) / h_res];
            row[x * 3 + 0] = colour & 0xFF;
            row[x * 3 + 1] = (colour >> 8) & 0xFF;
            row[x * 3 + 2] = (colour >> 16) & 0xFF;
        }
    }
}

void app_main(void)
{
    esp_lcd_rpi_dsi_config_t config = ESP_LCD_RPI_DSI_DEFAULT_CONFIG();
    esp_lcd_rpi_dsi_handle_t display = NULL;

    ESP_ERROR_CHECK(esp_lcd_rpi_dsi_start(&config, &display));
    esp_lcd_panel_handle_t panel = esp_lcd_rpi_dsi_get_panel(display);

    void *frame_buffer = NULL;
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_get_frame_buffer(panel, 1, &frame_buffer));
    paint_colour_bars(frame_buffer, config.timing.h_size, config.timing.v_size);

    /* The frame buffer lives in PSRAM and is cached, but the DSI DMA reads PSRAM
     * directly, so whatever is still dirty in L2 is invisible to the panel. Painting
     * top to bottom leaves the last ~128 KB -- the bottom of the screen -- unwritten
     * back, which shows up as a corrupted band. esp_lcd_panel_draw_bitmap() does this
     * sync itself; writing into the frame buffer by hand means doing it here. */
    size_t fb_size = (size_t)config.timing.h_size * config.timing.v_size * 3;
    ESP_ERROR_CHECK(esp_cache_msync(frame_buffer, fb_size,
                                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED));

    ESP_LOGI(TAG, "colour bars painted, touch the panel to see coordinates");

    // The touch controller comes from espressif/esp_lcd_touch_ft5x06, so it is read
    // with the ordinary esp_lcd_touch API
    esp_lcd_touch_handle_t touch = esp_lcd_rpi_dsi_get_touch(display);
    bool was_down = false;
    while (touch) {
        esp_lcd_touch_point_data_t point = {0};
        uint8_t points = 0;
        esp_lcd_touch_read_data(touch);
        esp_lcd_touch_get_data(touch, &point, &points, 1);
        bool is_down = points > 0;
        if (is_down && !was_down) {
            ESP_LOGI(TAG, "touch at x=%u y=%u", point.x, point.y);
        }
        was_down = is_down;
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    ESP_LOGW(TAG, "no touch controller, nothing left to do");
    vTaskDelete(NULL);
}
