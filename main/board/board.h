#pragma once
 
/**
 * board.h — Pin assignments for Waveshare ESP32-S3 Touch LCD 3.5"
 *
 * Verify against the schematic PDF for your board revision before flashing.
 * Waveshare occasionally revises GPIO assignments between hardware versions.
 */
 
// ── LCD SPI bus ───────────────────────────────────────────────────
// Source: schematic net table (bottom-left), LCD connector J1
//   IO1  = LCD_SPI_MOSI / LCD_QSPI_IO0
//   IO2  = LCD_SPI_MISO / LCD_QSPI_IO1
//   IO3  = LCD_DC       / LCD_QSPI_IO2   ← DC in standard SPI mode
//   IO5  = LCD_SPI_SCLK / LCD_QSPI_SCLK
//   IO10 = LCD_CS
//   IO6  = LCD_BL  (via S8050 transistor T1, active HIGH)
//   RST  → TCA9554 IO expander P1 (not a direct GPIO)
#define PIN_LCD_MOSI    GPIO_NUM_1
#define PIN_LCD_MISO    GPIO_NUM_2
#define PIN_LCD_SCLK    GPIO_NUM_5
#define PIN_LCD_CS      GPIO_NUM_10
#define PIN_LCD_DC      GPIO_NUM_3
 
// RST is via TCA9554 IO expander — handled separately, not a direct GPIO
#define PIN_LCD_RST     -1
 
// Backlight via transistor, active HIGH
#define PIN_LCD_BL      GPIO_NUM_6
 
// ── I2C bus (FT6336 touch + IO expander TCA9554) ─────────────────
// IO7 = ESP_SCL, IO8 = ESP_SDA  (shared bus for touch, expander, RTC, IMU)
#define PIN_TOUCH_SDA   GPIO_NUM_8
#define PIN_TOUCH_SCL   GPIO_NUM_7
 
// INT and RST for FT6336 are routed through TCA9554 (expander pins P2, P1)
// They are NOT used — Waveshare demo confirms both are GPIO_NUM_NC
#define TCA9554_PIN_TOUCH_RST 2
#define TCA9554_PIN_TOUCH_INT 3

// FT6336 I2C address (fixed, no reset sequence needed to set it)
#define TOUCH_I2C_ADDR  0x38    // FT6336
 
// ── TCA9554 IO expander (U5, address 0x20) ───────────────────────
// Controls: LCD_RST (P1), TP_INT (P2), and other board functions
// You'll need to write to this over I2C to assert LCD reset properly.
#define TCA9554_I2C_ADDR    0x20
#define TCA9554_REG_OUTPUT  0x01
#define TCA9554_REG_CONFIG  0x03
#define TCA9554_PIN_LCD_RST (1 << 1)   // P1
#define TCA9554_PIN_TP_INT  (1 << 2)   // P2
 
// ── Display geometry ─────────────────────────────────────────────
#define LCD_H_RES       320
#define LCD_V_RES       480
#define LCD_BIT_DEPTH   16
 
#define LVGL_DRAW_BUF_LINES   40
#define LVGL_TICK_PERIOD_MS   5