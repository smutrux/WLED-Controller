#pragma once
/**
 * ui.h — WLED Controller UI public API
 */

#include <stdint.h>
#include <stdbool.h>
#include "lvgl.h"
#include "wifi/wifi.h"

// ── Widget handles ────────────────────────────────────────────────────────────
extern lv_obj_t *ui_status_label;
extern lv_obj_t *ui_brightness_slider;
extern lv_obj_t *ui_brightness_label;
extern lv_obj_t *ui_color_preview;
extern lv_obj_t *ui_power_btn;
extern lv_obj_t *ui_power_btn_label;
extern lv_obj_t *ui_preset_dropdown;

// ── Picker focus target enum (shared with encoder_nav) ───────────────────────
typedef enum {
    PICKER_TARGET_WHEEL = 0,
    PICKER_TARGET_SAT,
    PICKER_TARGET_APPLY,
    PICKER_TARGET_CANCEL,
    PICKER_TARGET_COUNT
} picker_target_t;

// ── Build ─────────────────────────────────────────────────────────────────────
void ui_build(void);

// ── State update functions (poll task, under lvgl_lock) ───────────────────────
void ui_set_status(const char *line1, const char *line2);
void ui_set_brightness(uint8_t bri);
void ui_set_color(uint32_t rgb);
void ui_set_power(bool on);
void ui_set_wifi_status(wifi_conn_state_t state);
void ui_set_effect(int fx_index);
void ui_set_preset(int display_index, int wled_id);
void ui_presets_loaded(void);

// ── Encoder interaction ───────────────────────────────────────────────────────
void ui_brightness_send_current(void);   // called on encoder exit from edit mode

// ── Color picker state (queried by encoder_nav) ───────────────────────────────
bool             ui_color_picker_is_open(void);
lv_obj_t        *ui_color_picker_wheel(void);
lv_obj_t        *ui_color_picker_sat(void);
lv_obj_t        *ui_color_picker_apply(void);
lv_obj_t        *ui_color_picker_cancel(void);
picker_target_t  ui_color_picker_focus(void);
void             ui_color_picker_set_focus(picker_target_t t);
void             ui_color_picker_close_apply(void);
void             ui_color_picker_close_cancel(void);

// Move picker focus highlight; called from encoder_nav
void ui_picker_set_highlight(picker_target_t t);
void ui_picker_set_editing(picker_target_t t, bool editing);

// ── LVGL thread-safety ────────────────────────────────────────────────────────
void lvgl_lock(void);
void lvgl_unlock(void);
