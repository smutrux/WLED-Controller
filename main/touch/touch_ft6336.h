#pragma once
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    uint16_t x;
    uint16_t y;
} touch_point_t;

esp_err_t touch_ft6336_init(void);
uint8_t   touch_ft6336_read(touch_point_t *points, uint8_t max_points);
