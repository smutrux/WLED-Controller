#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "lvgl.h"
#include "wifi/wifi.h"

// Widget handles — use the ui_set_* helpers rather than touching these directly
extern lv_obj_t *ui_status_label;
extern lv_obj_t *ui_brightness_slider;
extern lv_obj_t *ui_brightness_label;
extern lv_obj_t *ui_color_preview;
extern lv_obj_t *ui_power_btn;

/** Build the full initial UI on the active LVGL screen */
void ui_build(void);

/** Update the status card text (called from WiFi/poll stage) */
void ui_set_status(const char *line1, const char *line2);

/** Sync brightness slider + label to polled value */
void ui_set_brightness(uint8_t bri);

/** Update color preview swatch (0xRRGGBB) */
void ui_set_color(uint32_t rgb);

/** Sync power button state */
void ui_set_power(bool on);

// ── WiFi status (called from wifi.c event handler) ───────────────────────────
// wifi_conn_state_t is defined in wifi/wifi.h
void ui_set_wifi_status(wifi_conn_state_t state);

// ── LVGL thread-safety helpers (defined in main.c) ───────────────────────────
// Any task other than the LVGL task must bracket LVGL calls with these.
// They use a recursive mutex so re-entrant calls within the same task are safe.
void lvgl_lock(void);
void lvgl_unlock(void);
