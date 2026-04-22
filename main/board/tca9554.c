#include "board/tca9554.h"
#include "driver/i2c.h"
#include "esp_log.h"

#define I2C_PORT I2C_NUM_0
#define TIMEOUT  50

static const char *TAG = "tca9554";

// Shadow registers
static uint8_t s_config = 0xFF;
static uint8_t s_output = 0xFF;

static esp_err_t write_reg(uint8_t reg, uint8_t val)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();

    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (TCA9554_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_write_byte(cmd, val, true);
    i2c_master_stop(cmd);

    esp_err_t ret = i2c_master_cmd_begin(I2C_PORT, cmd, pdMS_TO_TICKS(TIMEOUT));
    i2c_cmd_link_delete(cmd);
    return ret;
}

static esp_err_t read_reg(uint8_t reg, uint8_t *val)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();

    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (TCA9554_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_stop(cmd);
    i2c_master_cmd_begin(I2C_PORT, cmd, pdMS_TO_TICKS(TIMEOUT));
    i2c_cmd_link_delete(cmd);

    cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (TCA9554_I2C_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read_byte(cmd, val, I2C_MASTER_NACK);
    i2c_master_stop(cmd);

    esp_err_t ret = i2c_master_cmd_begin(I2C_PORT, cmd, pdMS_TO_TICKS(TIMEOUT));
    i2c_cmd_link_delete(cmd);
    return ret;
}

esp_err_t tca9554_init(void)
{
    ESP_LOGI(TAG, "Init");

    s_config = 0xFF;
    s_output = 0xFF;

    return tca9554_commit();
}

esp_err_t tca9554_set_output(uint8_t pin)
{
    s_config &= ~(1 << pin);
    return ESP_OK;
}

esp_err_t tca9554_set_input(uint8_t pin)
{
    s_config |= (1 << pin);
    return ESP_OK;
}

esp_err_t tca9554_write_pin(uint8_t pin, bool level)
{
    if (level)
        s_output |= (1 << pin);
    else
        s_output &= ~(1 << pin);

    return ESP_OK;
}

esp_err_t tca9554_read_pin(uint8_t pin, bool *level)
{
    uint8_t val;
    esp_err_t ret = read_reg(0x00, &val);
    if (ret != ESP_OK) return ret;

    *level = (val >> pin) & 1;
    return ESP_OK;
}

esp_err_t tca9554_commit(void)
{
    ESP_ERROR_CHECK(write_reg(0x03, s_config));
    ESP_ERROR_CHECK(write_reg(0x01, s_output));
    return ESP_OK;
}