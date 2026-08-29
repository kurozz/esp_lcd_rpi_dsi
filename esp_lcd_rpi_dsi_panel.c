/*
 * SPDX-FileCopyrightText: 2026 Kuro
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "soc/soc_caps.h"

#if SOC_MIPI_DSI_SUPPORTED
#include <stdlib.h>
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_mipi_dsi.h"
#include "hal/mipi_dsi_hal.h"
#include "hal/mipi_dsi_host_ll.h"
#include "hal/mipi_dsi_types.h"
#include "esp_lcd_rpi_dsi_panel.h"

static const char *TAG = "rpi_dsi";

/* ATTINY88 register map, from panel-raspberrypi-touchscreen.c */
#define REG_ID      0x80
#define REG_PORTA   0x81
#define REG_PORTB   0x82
#define REG_PORTC   0x83
#define REG_POWERON 0x85 // revision-1 firmware only, unused here
#define REG_PWM     0x86
#define REG_ADDR_L  0x8c
#define REG_ADDR_H  0x8d
#define REG_WRITE_DATA_H 0x90
#define REG_WRITE_DATA_L 0x91

/* ATTINY firmware revisions, from the REG_ID check in
 * drivers/regulator/rpi-panel-attiny-regulator.c */
#define ATTINY_FW_V1 0xde
#define ATTINY_FW_V2 0xc3

/* Port bits. PORTA/PORTB/PORTC are write-only: the Linux driver keeps a shadow
 * copy in state->port_states[] and never reads them back from the device, so
 * there is no status here to poll -- only the delays below. */
#define PA_LCD_LR       (1 << 2) // orientation the closed-source firmware uses
#define PB_LCD_VCC_N    (1 << 1)
#define PB_LCD_MAIN     (1 << 7)
#define PC_LED_EN       (1 << 0)
#define PC_RST_TP_N     (1 << 1) // touch controller reset, active low
#define PC_RST_LCD_N    (1 << 2) // LCD reset, active low
#define PC_RST_BRIDGE_N (1 << 3) // TC358762 reset, active low

/* TC358762 registers, from the same driver */
#define PPI_STARTPPI         0x0104
#define PPI_LPTXTIMECNT      0x0114
#define PPI_D0S_ATMR         0x0144
#define PPI_D1S_ATMR         0x0148
#define PPI_D0S_CLRSIPOCOUNT 0x0164
#define PPI_D1S_CLRSIPOCOUNT 0x0168
#define DSI_STARTDSI         0x0204
#define DSI_LANEENABLE       0x0210
#define LCDCTRL              0x0420
#define SPICMR               0x0450
#define SYSCTRL              0x0464

#define DSI_LANEENABLE_CLOCK (1 << 0)
#define DSI_LANEENABLE_D0    (1 << 1)

/* The bridge init sequence, verbatim from rpi_touchscreen_prepare(). Note that
 * LCDCTRL 0x00100150 decodes to VSDELAY(1) | RGB888 | UNK6 | VTGEN, and that the
 * LCD_HS_HBP / LCD_HDISP_HFP / LCD_VS_VBP / LCD_VDISP_VFP timing registers are
 * deliberately left at their reset values -- programming them is what the newer
 * tc358762.c does differently. */
typedef struct {
    uint16_t reg;
    uint32_t val;
    uint16_t delay_ms;
} rpi_dsi_bridge_cmd_t;

static const rpi_dsi_bridge_cmd_t s_bridge_init[] = {
    { DSI_LANEENABLE,       DSI_LANEENABLE_CLOCK | DSI_LANEENABLE_D0, 0 },
    { PPI_D0S_CLRSIPOCOUNT, 0x05,       0 },
    { PPI_D1S_CLRSIPOCOUNT, 0x05,       0 },
    { PPI_D0S_ATMR,         0x00,       0 },
    { PPI_D1S_ATMR,         0x00,       0 },
    { PPI_LPTXTIMECNT,      0x03,       0 },
    { SPICMR,               0x00,       0 },
    { LCDCTRL,              0x00100150, 0 },
    { SYSCTRL,              0x040f,     100 },
    { PPI_STARTPPI,         0x01,       0 },
    { DSI_STARTDSI,         0x01,       100 },
};

/* Layout mirror of the private `esp_lcd_dsi_bus_t` from
 * $IDF_PATH/components/esp_lcd/dsi/mipi_dsi_priv.h (IDF v6.1). We only need the
 * HAL context, to emit generic long writes and to override the video mode --
 * neither is reachable through the public esp_lcd API. rpi_dsi_bus_hal() sanity
 * checks the result so a layout change in a future IDF fails loudly here rather
 * than silently scribbling on registers. */
typedef struct {
    int bus_id;
    mipi_dsi_hal_context_t hal;
} rpi_dsi_bus_layout_t;

typedef struct {
    esp_lcd_panel_io_handle_t io;
    mipi_dsi_hal_context_t *hal;
    i2c_master_dev_handle_t attiny;
    uint8_t virtual_channel;
    uint8_t brightness;
    // saved originals of the MIPI DPI panel
    esp_err_t (*del)(esp_lcd_panel_t *panel);
    esp_err_t (*init)(esp_lcd_panel_t *panel);
} rpi_dsi_panel_t;

static esp_err_t panel_rpi_dsi_del(esp_lcd_panel_t *panel);
static esp_err_t panel_rpi_dsi_init(esp_lcd_panel_t *panel);
static esp_err_t panel_rpi_dsi_reset(esp_lcd_panel_t *panel);
static esp_err_t panel_rpi_dsi_disp_on_off(esp_lcd_panel_t *panel, bool on_off);

static esp_err_t rpi_dsi_bus_hal(esp_lcd_dsi_bus_handle_t bus, mipi_dsi_hal_context_t **ret_hal)
{
    mipi_dsi_hal_context_t *hal = &((rpi_dsi_bus_layout_t *)bus)->hal;
    ESP_RETURN_ON_FALSE(hal->host && hal->bridge && hal->lane_bit_rate_mbps >= 80 && hal->lane_bit_rate_mbps <= 1500,
                        ESP_ERR_INVALID_STATE, TAG,
                        "unexpected esp_lcd_dsi_bus_t layout, this driver needs updating for your IDF version");
    *ret_hal = hal;
    return ESP_OK;
}

static esp_err_t rpi_dsi_attiny_write(rpi_dsi_panel_t *ctx, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(ctx->attiny, buf, sizeof(buf), 100);
}

static esp_err_t rpi_dsi_attiny_read(rpi_dsi_panel_t *ctx, uint8_t reg, uint8_t *val)
{
    // The ATTINY firmware bit-bangs I2C and does not tolerate a repeated start,
    // so address and data go out as two separate transactions, as in Linux.
    ESP_RETURN_ON_ERROR(i2c_master_transmit(ctx->attiny, &reg, 1, 100), TAG, "address write failed");
    esp_rom_delay_us(200);
    return i2c_master_receive(ctx->attiny, val, 1, 100);
}

static void rpi_dsi_bridge_write(rpi_dsi_panel_t *ctx, uint16_t reg, uint32_t val)
{
    // 16-bit register address then 32-bit value, both little endian, in one
    // MIPI DSI generic long write (DT 0x29)
    uint8_t msg[6] = {
        reg & 0xFF, (reg >> 8) & 0xFF,
        val & 0xFF, (val >> 8) & 0xFF, (val >> 16) & 0xFF, (val >> 24) & 0xFF,
    };
    mipi_dsi_hal_host_gen_write_long_packet(ctx->hal, ctx->virtual_channel,
                                            MIPI_DSI_DT_GENERIC_LONG_WRITE, msg, sizeof(msg));
}

/* Power sequence from attiny_lcd_power_enable() / attiny_lcd_power_disable().
 * This is the path the maintained Linux driver uses for both firmware
 * revisions; the older REG_POWERON shortcut only exists on revision 1. */
static esp_err_t rpi_dsi_power_on(rpi_dsi_panel_t *ctx)
{
    // hold the bridge, the LCD and the touch controller in reset
    ESP_RETURN_ON_ERROR(rpi_dsi_attiny_write(ctx, REG_PORTC, 0), TAG, "PORTC write failed");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(rpi_dsi_attiny_write(ctx, REG_PORTA, PA_LCD_LR), TAG, "PORTA write failed");
    vTaskDelay(pdMS_TO_TICKS(10));
    // main regulator on, and power to the panel
    ESP_RETURN_ON_ERROR(rpi_dsi_attiny_write(ctx, REG_PORTB, PB_LCD_MAIN), TAG, "PORTB write failed");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(rpi_dsi_attiny_write(ctx, REG_PORTC, PC_LED_EN), TAG, "PORTC write failed");
    vTaskDelay(pdMS_TO_TICKS(80));

    /* Deassert the bridge and LCD resets. In Linux this is NOT part of
     * attiny_lcd_power_enable(): the ATTINY exposes a gpio_chip whose GPIO 0
     * (RST_BRIDGE_N) maps to PC_RST_BRIDGE_N | PC_RST_LCD_N, and the tc358762's
     * vddc-supply is a regulator-fixed hanging off that GPIO, so the release only
     * happens later, when the bridge driver probes. Without it the TC358762 sits
     * in reset and ignores every register write. PC_RST_TP_N rides along: it is
     * GPIO 1, the touch controller's reset, harmless when touch is unused. */
    ESP_RETURN_ON_ERROR(rpi_dsi_attiny_write(ctx, REG_PORTC,
                                             PC_LED_EN | PC_RST_BRIDGE_N | PC_RST_LCD_N | PC_RST_TP_N),
                        TAG, "releasing the bridge reset failed");
    vTaskDelay(pdMS_TO_TICKS(8));

    /* The tail of attiny_gpio_set() for RST_BRIDGE_N: the ATTINY has its own
     * back-channel to the bridge's registers, and pokes 0x0000 into bridge
     * register 0x047c on the way out of reset. */
    ESP_RETURN_ON_ERROR(rpi_dsi_attiny_write(ctx, REG_ADDR_H, 0x04), TAG, "ADDR_H write failed");
    vTaskDelay(pdMS_TO_TICKS(8));
    ESP_RETURN_ON_ERROR(rpi_dsi_attiny_write(ctx, REG_ADDR_L, 0x7c), TAG, "ADDR_L write failed");
    vTaskDelay(pdMS_TO_TICKS(8));
    ESP_RETURN_ON_ERROR(rpi_dsi_attiny_write(ctx, REG_WRITE_DATA_H, 0x00), TAG, "DATA_H write failed");
    vTaskDelay(pdMS_TO_TICKS(8));
    ESP_RETURN_ON_ERROR(rpi_dsi_attiny_write(ctx, REG_WRITE_DATA_L, 0x00), TAG, "DATA_L write failed");
    vTaskDelay(pdMS_TO_TICKS(100));
    return ESP_OK;
}

static void rpi_dsi_power_off(rpi_dsi_panel_t *ctx)
{
    rpi_dsi_attiny_write(ctx, REG_PWM, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    rpi_dsi_attiny_write(ctx, REG_PORTA, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    rpi_dsi_attiny_write(ctx, REG_PORTB, PB_LCD_VCC_N);
    vTaskDelay(pdMS_TO_TICKS(10));
    rpi_dsi_attiny_write(ctx, REG_PORTC, 0);
    vTaskDelay(pdMS_TO_TICKS(30));
}

esp_err_t esp_lcd_new_panel_rpi_dsi(esp_lcd_panel_io_handle_t io,
                                    const esp_lcd_panel_dev_config_t *panel_dev_config,
                                    esp_lcd_panel_handle_t *ret_panel)
{
    ESP_RETURN_ON_FALSE(io && panel_dev_config && ret_panel, ESP_ERR_INVALID_ARG, TAG, "invalid arguments");
    esp_lcd_rpi_dsi_vendor_config_t *vendor_config = (esp_lcd_rpi_dsi_vendor_config_t *)panel_dev_config->vendor_config;
    ESP_RETURN_ON_FALSE(vendor_config && vendor_config->mipi_config.dsi_bus && vendor_config->mipi_config.dpi_config,
                        ESP_ERR_INVALID_ARG, TAG, "invalid vendor config");
    ESP_RETURN_ON_FALSE(vendor_config->i2c_bus, ESP_ERR_INVALID_ARG, TAG,
                        "an I2C bus is required to reach the panel's ATTINY");

    esp_err_t ret = ESP_OK;
    rpi_dsi_panel_t *ctx = calloc(1, sizeof(rpi_dsi_panel_t));
    ESP_RETURN_ON_FALSE(ctx, ESP_ERR_NO_MEM, TAG, "no mem for rpi_dsi panel");

    ESP_GOTO_ON_ERROR(rpi_dsi_bus_hal(vendor_config->mipi_config.dsi_bus, &ctx->hal), err, TAG, "no DSI HAL context");

    i2c_device_config_t attiny_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = vendor_config->i2c_addr ? vendor_config->i2c_addr : ESP_LCD_RPI_DSI_ATTINY_ADDR,
        .scl_speed_hz = vendor_config->i2c_scl_speed_hz ? vendor_config->i2c_scl_speed_hz : 100000,
    };
    ESP_GOTO_ON_ERROR(i2c_master_bus_add_device(vendor_config->i2c_bus, &attiny_config, &ctx->attiny), err, TAG,
                      "add ATTINY to I2C bus failed");

    ctx->io = io;
    ctx->virtual_channel = vendor_config->mipi_config.dpi_config->virtual_channel;
    ctx->brightness = vendor_config->brightness ? vendor_config->brightness : 255;

    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_dpi(vendor_config->mipi_config.dsi_bus,
                                            vendor_config->mipi_config.dpi_config, ret_panel), err, TAG,
                      "create MIPI DPI panel failed");

    if (vendor_config->flags.non_burst_video) {
        // esp_lcd_new_panel_dpi() has just hardcoded burst mode; the video stream is
        // only switched on later, in the panel's init, so overriding here is safe.
        mipi_dsi_host_ll_dpi_set_video_burst_type(ctx->hal->host, MIPI_DSI_LL_VIDEO_NON_BURST_WITH_SYNC_PULSES);
        ESP_LOGI(TAG, "video mode forced to non-burst with sync pulses");
    }

    ctx->del = (*ret_panel)->del;
    ctx->init = (*ret_panel)->init;
    (*ret_panel)->del = panel_rpi_dsi_del;
    (*ret_panel)->init = panel_rpi_dsi_init;
    (*ret_panel)->reset = panel_rpi_dsi_reset;
    (*ret_panel)->disp_on_off = panel_rpi_dsi_disp_on_off;
    (*ret_panel)->user_data = ctx;

    return ESP_OK;

err:
    if (ctx->attiny) {
        i2c_master_bus_rm_device(ctx->attiny);
    }
    free(ctx);
    return ret;
}

static esp_err_t panel_rpi_dsi_del(esp_lcd_panel_t *panel)
{
    rpi_dsi_panel_t *ctx = panel->user_data;
    rpi_dsi_power_off(ctx);
    i2c_master_bus_rm_device(ctx->attiny);
    esp_err_t ret = ctx->del(panel);
    free(ctx);
    return ret;
}

static esp_err_t panel_rpi_dsi_reset(esp_lcd_panel_t *panel)
{
    rpi_dsi_panel_t *ctx = panel->user_data;
    rpi_dsi_power_off(ctx);
    return ESP_OK;
}

static esp_err_t panel_rpi_dsi_init(esp_lcd_panel_t *panel)
{
    rpi_dsi_panel_t *ctx = panel->user_data;
    uint8_t id = 0;

    ESP_RETURN_ON_ERROR(rpi_dsi_attiny_read(ctx, REG_ID, &id), TAG, "the ATTINY is not answering");
    switch (id) {
    case ATTINY_FW_V1:
        ESP_LOGI(TAG, "ATTINY firmware revision 1 (REG_ID 0x%02x)", id);
        break;
    case ATTINY_FW_V2:
        ESP_LOGI(TAG, "ATTINY firmware revision 2 (REG_ID 0x%02x)", id);
        break;
    default:
        ESP_LOGW(TAG, "unknown ATTINY firmware revision 0x%02x, trying the standard sequence anyway", id);
        break;
    }

    ESP_RETURN_ON_ERROR(rpi_dsi_power_on(ctx), TAG, "powering on the panel failed");

    for (size_t i = 0; i < sizeof(s_bridge_init) / sizeof(s_bridge_init[0]); i++) {
        rpi_dsi_bridge_write(ctx, s_bridge_init[i].reg, s_bridge_init[i].val);
        if (s_bridge_init[i].delay_ms) {
            vTaskDelay(pdMS_TO_TICKS(s_bridge_init[i].delay_ms));
        }
    }

    // Read a register back before any video starts. If the bridge answers, the DSI
    // link works in low-power mode and the init writes landed, which separates a
    // link problem from a video-mode problem.
    // now let the DPI panel start streaming video into the bridge
    ESP_RETURN_ON_ERROR(ctx->init(panel), TAG, "starting the DPI video stream failed");

    // PA_LCD_LR was already set by the power-on sequence, so only the backlight is left
    ESP_RETURN_ON_ERROR(rpi_dsi_attiny_write(ctx, REG_PWM, ctx->brightness), TAG, "backlight on failed");
    return ESP_OK;
}

static esp_err_t panel_rpi_dsi_disp_on_off(esp_lcd_panel_t *panel, bool on_off)
{
    rpi_dsi_panel_t *ctx = panel->user_data;
    return rpi_dsi_attiny_write(ctx, REG_PWM, on_off ? ctx->brightness : 0);
}

esp_err_t esp_lcd_panel_rpi_dsi_set_brightness(esp_lcd_panel_handle_t panel, uint8_t brightness)
{
    ESP_RETURN_ON_FALSE(panel, ESP_ERR_INVALID_ARG, TAG, "invalid panel handle");
    rpi_dsi_panel_t *ctx = panel->user_data;
    ctx->brightness = brightness;
    return rpi_dsi_attiny_write(ctx, REG_PWM, brightness);
}

#endif // SOC_MIPI_DSI_SUPPORTED
