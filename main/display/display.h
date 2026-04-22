#pragma once
#include "esp_err.h"
#include "lvgl.h"

esp_err_t display_init(void);

/** LVGL flush callback — registered in main.c during lv_disp_drv setup */
void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map);
