/**
 * touch_ft6336.c — FT6336 capacitive touch driver
 *
 * The Waveshare ESP32-S3 Touch LCD 3.5" factory demo uses esp_lcd_touch_ft6336
 * (addr 0x38), confirming the touch IC is FT6336, not GT911.
 *
 * Reset / INT are routed through the TCA9554 IO expander:
 *   TCA9554 P1 = LCD_RST (shared with LCD reset — already done by display_init)
 *   TCA9554 P2 = TP_INT  (interrupt, currently polled, not interrupt-driven)
 *
 * The Waveshare demo sets EXAMPLE_PIN_TP_RST and EXAMPLE_PIN_TP_INT both to
 * GPIO_NUM_NC, meaning hardware reset and interrupt are not used for touch init.
 * We follow the same approach: no explicit reset here (display_init already
 * toggled the shared RST line through TCA9554), just probe and go.
 *
 * FT6336 register map (8-bit registers, single-byte I2C addresses):
 *   0x02  TD_STATUS   lower nibble = number of touch points
 *   0x03  P1_XH       [7:6]=event flag, [3:0]=X high bits
 *   0x04  P1_XL       X low bits
 *   0x05  P1_YH       [7:4]=touch ID, [3:0]=Y high bits
 *   0x06  P1_YL       Y low bits
 *   0x09  P2_XH       (same layout, second finger)
 *   0x0A  P2_XL
 *   0x0B  P2_YH
 *   0x0C  P2_YL
 */

#include "touch/touch_ft6336.h"
#include "board/board.h"

#include "driver/i2c.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ft6336";

#define I2C_PORT         I2C_NUM_0
#define I2C_TIMEOUT_MS   30
#define FT6336_ADDR      0x38

// ── Register addresses ────────────────────────────────────────────
#define REG_TD_STATUS    0x02   // touch point count
#define REG_P1_XH        0x03   // first finger X high + event flags
#define REG_CHIP_ID      0xA3   // should read 0x36 for FT6336

// ── Low-level I2C helpers ─────────────────────────────────────────

static esp_err_t ft6336_write(uint8_t reg, uint8_t val)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (FT6336_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_write_byte(cmd, val, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(I2C_PORT, cmd, pdMS_TO_TICKS(I2C_TIMEOUT_MS));
    i2c_cmd_link_delete(cmd);
    return ret;
}

static esp_err_t ft6336_read(uint8_t reg, uint8_t *buf, size_t len)
{
    // Set register pointer
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (FT6336_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(I2C_PORT, cmd, pdMS_TO_TICKS(I2C_TIMEOUT_MS));
    i2c_cmd_link_delete(cmd);
    if (ret != ESP_OK)
        return ret;

    // Read data
    cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (FT6336_ADDR << 1) | I2C_MASTER_READ, true);
    if (len > 1)
        i2c_master_read(cmd, buf, len - 1, I2C_MASTER_ACK);
    i2c_master_read_byte(cmd, buf + len - 1, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    ret = i2c_master_cmd_begin(I2C_PORT, cmd, pdMS_TO_TICKS(I2C_TIMEOUT_MS));
    i2c_cmd_link_delete(cmd);
    return ret;
}

// ── Public API ────────────────────────────────────────────────────

esp_err_t touch_ft6336_init(void)
{
    // I2C bus is already installed by display_init().
    // The shared RST line (TCA9554 P1) was already toggled during display_init,
    // which also resets the touch controller. No separate reset needed here.

    // Probe: read chip ID register
    uint8_t chip_id = 0;
    esp_err_t ret = ft6336_read(REG_CHIP_ID, &chip_id, 1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "FT6336 not responding (I2C addr 0x38). Check wiring. err=0x%x", ret);
        return ret;
    }
    ESP_LOGI(TAG, "FT6336 chip ID: 0x%02X (expect 0x36 or 0x64)", chip_id);

    // Set to normal operating mode (register 0xA5 = 0x00)
    ft6336_write(0xA5, 0x00);

    // Optional: set interrupt mode to polling (0x00) rather than trigger (0x01)
    ft6336_write(0xA4, 0x00);

    ESP_LOGI(TAG, "FT6336 init OK");
    return ESP_OK;
}

uint8_t touch_ft6336_read(touch_point_t *points, uint8_t max_points)
{
    uint8_t status = 0;
    if (ft6336_read(REG_TD_STATUS, &status, 1) != ESP_OK)
        return 0;

    uint8_t count = status & 0x0F;
    if (count == 0 || count > 2)
        return 0;

    if (count > max_points)
        count = max_points;

    // Read all 4 bytes per point starting at REG_P1_XH (0x03)
    // Layout: [XH, XL, YH, YL] repeated, with 0x09 gap between fingers
    // Offsets from base: P1=0x03, P2=0x09
    static const uint8_t point_reg[2] = {0x03, 0x09};

    for (uint8_t i = 0; i < count; i++) {
        uint8_t buf[4] = {0};
        if (ft6336_read(point_reg[i], buf, 4) != ESP_OK)
            continue;

        points[i].x = (uint16_t)((buf[0] & 0x0F) << 8) | buf[1];
        points[i].y = (uint16_t)((buf[2] & 0x0F) << 8) | buf[3];
    }

    return count;
}
