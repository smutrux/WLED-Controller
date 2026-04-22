#pragma once

#include <stdbool.h>
#include "esp_err.h"

// Default address for your board
#define TCA9554_I2C_ADDR 0x20

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t tca9554_init(void);

esp_err_t tca9554_set_output(uint8_t pin);
esp_err_t tca9554_set_input(uint8_t pin);

esp_err_t tca9554_write_pin(uint8_t pin, bool level);
esp_err_t tca9554_read_pin(uint8_t pin, bool *level);

esp_err_t tca9554_commit(void);

#ifdef __cplusplus
}
#endif