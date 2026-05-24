/**
 * display.c — ST7796 via esp_lcd, with LCD reset through TCA9554 IO expander
 *
 * The Waveshare ESP32-S3-Touch-LCD-3.5 routes LCD_RST and TP_INT through
 * a TCA9554PWR I2C IO expander (U5, addr 0x20) rather than directly to GPIOs.
 * We must toggle the expander's P1 output to assert reset before init.
 *
 * Pin summary (from schematic):
 *   MOSI/IO0  → GPIO1    SCLK  → GPIO5
 *   MISO/IO1  → GPIO2    CS    → GPIO10
 *   DC/IO2    → GPIO3    BL    → GPIO6
 *   RST       → TCA9554 P1 (I2C addr 0x20, shared bus IO7/IO8)
 */

#include "display/display.h"
#include "board/board.h"

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7796.h"
#include "driver/spi_master.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lvgl.h"
#include "board/tca9554.h"

static const char *TAG = "display";

static esp_lcd_panel_handle_t s_panel   = NULL;
static SemaphoreHandle_t      s_flush_sem = NULL;

// ── TCA9554 helpers ───────────────────────────────────────────────────────────
// The I2C driver is installed by touch_gt911_init() on I2C_NUM_0.
// If display_init() runs before touch init, install it here too — but in our
// app_main order, display goes first, so we install it here and touch reuses it.

#define I2C_PORT        I2C_NUM_0
#define I2C_TIMEOUT_MS  30

static esp_err_t i2c_ensure_installed(void)
{
    // Install only if not already done (idempotent)
    i2c_config_t cfg = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = PIN_TOUCH_SDA,
        .scl_io_num       = PIN_TOUCH_SCL,
        .sda_pullup_en    = GPIO_PULLUP_ENABLE,
        .scl_pullup_en    = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 400000,
    };
    i2c_param_config(I2C_PORT, &cfg);
    esp_err_t ret = i2c_driver_install(I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
    if (ret == ESP_ERR_INVALID_STATE) {
        // Already installed — that's fine
        return ESP_OK;
    }
    return ret;
}

/**
 * Assert and deassert LCD reset via TCA9554 P1.
 * P1 is LCD_RST (active LOW on the ST7796).
 *
 * We configure P1 as output, pull it LOW for 10ms, then HIGH.
 * Other expander pins are left as inputs (config reg 0xFF = all inputs,
 * then we only set P1 as output).
 */
static esp_err_t lcd_reset_via_expander(void)
{
    ESP_LOGI(TAG, "Resetting LCD via TCA9554");

    tca9554_set_output(TCA9554_PIN_LCD_RST);

    tca9554_write_pin(TCA9554_PIN_LCD_RST, 0);
    tca9554_commit();
    vTaskDelay(pdMS_TO_TICKS(10));

    tca9554_write_pin(TCA9554_PIN_LCD_RST, 1);
    tca9554_commit();
    vTaskDelay(pdMS_TO_TICKS(120));

    return ESP_OK;
}

// ── DMA done callback ─────────────────────────────────────────────────────────
static bool IRAM_ATTR on_color_trans_done(
    esp_lcd_panel_io_handle_t panel_io,
    esp_lcd_panel_io_event_data_t *edata,
    void *user_ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_flush_sem, &woken);
    return woken == pdTRUE;
}

// ── LVGL flush callback ───────────────────────────────────────────────────────
void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    esp_lcd_panel_draw_bitmap(s_panel,
        area->x1, area->y1, area->x2 + 1, area->y2 + 1, color_map);
    xSemaphoreTake(s_flush_sem, portMAX_DELAY);
    lv_disp_flush_ready(drv);
}

// ── Public init ───────────────────────────────────────────────────────────────
esp_err_t display_init(void)
{
    s_flush_sem = xSemaphoreCreateBinary();
    if (!s_flush_sem) return ESP_ERR_NO_MEM;

    // I2C must be up before we can talk to TCA9554
    ESP_ERROR_CHECK(i2c_ensure_installed());

    tca9554_init();

    // Hardware reset the ST7796 via expander
    ESP_ERROR_CHECK(lcd_reset_via_expander());

    // ── SPI bus ──────────────────────────────────────────────────────────────
    ESP_LOGI(TAG, "Initializing SPI bus (MOSI=IO%d, MISO=IO%d, SCLK=IO%d)",
             PIN_LCD_MOSI, PIN_LCD_MISO, PIN_LCD_SCLK);

    spi_bus_config_t bus_cfg = {
        .mosi_io_num     = PIN_LCD_MOSI,
        .miso_io_num     = PIN_LCD_MISO,
        .sclk_io_num     = PIN_LCD_SCLK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = LCD_H_RES * LVGL_DRAW_BUF_LINES * sizeof(uint16_t) + 8,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    // ── Panel IO ─────────────────────────────────────────────────────────────
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num         = PIN_LCD_CS,
        .dc_gpio_num         = PIN_LCD_DC,
        .pclk_hz             = 40 * 1000 * 1000,
        .lcd_cmd_bits        = 8,
        .lcd_param_bits      = 8,
        .on_color_trans_done = on_color_trans_done,
        .user_ctx            = NULL,
        .trans_queue_depth   = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, &io_handle));

    // ── ST7796 panel ─────────────────────────────────────────────────────────
    // reset_gpio_num = -1 because we did the reset manually via TCA9554 above
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = -1,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = LCD_BIT_DEPTH,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7796(io_handle, &panel_cfg, &s_panel));

    // reset() with gpio=-1 just sends the software reset command (0x01)
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));

    // Portrait orientation — adjust mirror flags if image is flipped
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(s_panel, false));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, true, false));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    // ── Backlight ────────────────────────────────────────────────────────────
    gpio_config_t bl = {
        .pin_bit_mask = (1ULL << PIN_LCD_BL),
        .mode         = GPIO_MODE_OUTPUT,
    };
    gpio_config(&bl);
    gpio_set_level(PIN_LCD_BL, 1);

    ESP_LOGI(TAG, "Display ready (%dx%d)", LCD_H_RES, LCD_V_RES);
    return ESP_OK;
}