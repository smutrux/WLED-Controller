#pragma once
/**
 * ui.h — WLED Controller UI public API
 */

#include <stdint.h>
#include <stdbool.h>
#include "lvgl.h"
#include "wifi/wifi.h"

// ── Widget handles (used by encoder_nav for focus targets) ────────────────────
extern lv_obj_t *ui_status_label;
extern lv_obj_t *ui_brightness_slider;
extern lv_obj_t *ui_brightness_label;
extern lv_obj_t *ui_color_preview;
extern lv_obj_t *ui_power_btn;
extern lv_obj_t *ui_power_btn_label;
extern lv_obj_t *ui_preset_dropdown;   // for encoder nav target

// ── Build ─────────────────────────────────────────────────────────────────────
void ui_build(void);

// ── State update functions (called from poll task under lvgl_lock) ────────────
void ui_set_status(const char *line1, const char *line2);
void ui_set_brightness(uint8_t bri);
void ui_set_color(uint32_t rgb);           // 0x00RRGGBB
void ui_set_power(bool on);
void ui_set_wifi_status(wifi_conn_state_t state);
void ui_set_effect(int fx_index);          // no-op stub kept for poll compat
void ui_set_preset(int display_index, int wled_id);

/** Called after wled_presets_fetch() completes — populates the dropdown. */
void ui_presets_loaded(void);

// ── LVGL thread-safety helpers (defined in main.c) ────────────────────────────
void lvgl_lock(void);
void lvgl_unlock(void);