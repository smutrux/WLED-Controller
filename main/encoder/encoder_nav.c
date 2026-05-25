/**
 * encoder_nav.c — Focus/edit navigation for the rotary encoder
 *
 * Modes:
 *   NAV_MODE_NAVIGATE      CW/CCW cycles focus; press activates target
 *   NAV_MODE_EDIT          CW/CCW adjusts slider with velocity scaling;
 *                          press exits and sends HTTP (deferred brightness)
 *   NAV_MODE_COLOR_PICKER  CW/CCW cycles picker focus; press selects target
 *   NAV_MODE_COLOR_EDIT    CW/CCW adjusts wheel/saturation; press exits edit
 *   NAV_MODE_DROPDOWN      CW/CCW changes preset selection; press accepts it
 *
 * Velocity scaling:
 *   Gap between events < 80 ms  → step 8
 *   Gap                80-200 ms → step 4
 *   Gap                > 200 ms  → step 1
 */

#include "encoder/encoder_nav.h"
#include "encoder/encoder.h"
#include "ui.h"
#include "board/board.h"
#include "poll/wled_poll.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "lvgl.h"

static const char *TAG = "enc_nav";

#define VEL_FAST_US    80000ULL
#define VEL_MED_US    200000ULL

// ── Main-screen targets ───────────────────────────────────────────────────────
typedef enum {
    TARGET_POWER = 0,
    TARGET_BRIGHTNESS,
    TARGET_COLOR,
    TARGET_PRESET,
    TARGET_COUNT
} target_id_t;

typedef enum {
    TARGET_TYPE_BUTTON,
    TARGET_TYPE_SLIDER,
    TARGET_TYPE_ACTION,
    TARGET_TYPE_DROPDOWN,
} target_type_t;

typedef struct {
    lv_obj_t      **widget_ptr;
    target_type_t   type;
    const char     *name;
} nav_target_t;

static const nav_target_t k_targets[TARGET_COUNT] = {
    [TARGET_POWER]      = { &ui_power_btn,         TARGET_TYPE_BUTTON, "Power"      },
    [TARGET_BRIGHTNESS] = { &ui_brightness_slider,  TARGET_TYPE_SLIDER, "Brightness" },
    [TARGET_COLOR]      = { &ui_color_preview,      TARGET_TYPE_ACTION, "Color"      },
    [TARGET_PRESET]     = { &ui_preset_dropdown,    TARGET_TYPE_DROPDOWN, "Preset"   },
};

// ── State ─────────────────────────────────────────────────────────────────────
typedef enum {
    NAV_MODE_NAVIGATE,
    NAV_MODE_EDIT,
    NAV_MODE_COLOR_PICKER,
    NAV_MODE_COLOR_EDIT,
    NAV_MODE_DROPDOWN,
} nav_mode_t;

static int        s_focus_idx   = -1;
static nav_mode_t s_mode        = NAV_MODE_NAVIGATE;
static int64_t    s_last_rot_us = 0;
static uint16_t   s_dropdown_open_sel = 0;

// ── Velocity ──────────────────────────────────────────────────────────────────
static int velocity_step(void)
{
    int64_t now = esp_timer_get_time();
    int64_t gap = (s_last_rot_us > 0) ? (now - s_last_rot_us) : INT64_MAX;
    s_last_rot_us = now;
    if (gap < (int64_t)VEL_FAST_US) return 8;
    if (gap < (int64_t)VEL_MED_US)  return 4;
    return 1;
}

// ── Focus styling (main screen) ───────────────────────────────────────────────
static void apply_focus_style(lv_obj_t *widget, bool is_edit)
{
    lv_color_t color = is_edit
        ? lv_palette_main(LV_PALETTE_LIGHT_BLUE)
        : lv_palette_main(LV_PALETTE_BLUE);
    lv_obj_set_style_outline_color(widget, color, 0);
    lv_obj_set_style_outline_width(widget, 2, 0);
    lv_obj_set_style_outline_pad(widget, 3, 0);
    lv_obj_set_style_outline_opa(widget, LV_OPA_COVER, 0);
    if (lv_obj_check_type(widget, &lv_slider_class))
        lv_obj_set_style_pad_all(widget, is_edit ? 12 : 8, LV_PART_KNOB);
}

static void clear_focus_style(lv_obj_t *widget)
{
    lv_obj_set_style_outline_opa(widget, LV_OPA_TRANSP, 0);
    lv_obj_set_style_outline_width(widget, 0, 0);
    if (lv_obj_check_type(widget, &lv_slider_class))
        lv_obj_set_style_pad_all(widget, 8, LV_PART_KNOB);
}

static lv_obj_t *focused_widget(void)
{
    if (s_focus_idx < 0 || s_focus_idx >= TARGET_COUNT) return NULL;
    return *k_targets[s_focus_idx].widget_ptr;
}

// ── Main-screen navigation ────────────────────────────────────────────────────
static void set_focus(int new_idx)
{
    lv_obj_t *old = focused_widget();
    if (old) clear_focus_style(old);
    s_focus_idx = new_idx;
    s_mode = NAV_MODE_NAVIGATE;
    lv_obj_t *nw = focused_widget();
    if (nw) apply_focus_style(nw, false);
}

static void cycle_focus(int dir)
{
    int next = (s_focus_idx < 0)
        ? (dir > 0 ? 0 : TARGET_COUNT - 1)
        : (s_focus_idx + dir + TARGET_COUNT) % TARGET_COUNT;
    set_focus(next);
}

static void activate_focused(void)
{
    if (s_focus_idx < 0) { set_focus(0); return; }
    lv_obj_t *widget = focused_widget();
    if (!widget) return;

    switch (k_targets[s_focus_idx].type) {
    case TARGET_TYPE_BUTTON:
        lv_event_send(widget, LV_EVENT_CLICKED, NULL);
        break;

    case TARGET_TYPE_SLIDER:
        if (s_mode == NAV_MODE_NAVIGATE) {
            s_mode = NAV_MODE_EDIT;
            apply_focus_style(widget, true);
            wled_poll_set_paused(true);
        } else {
            // Exit edit → send deferred HTTP, unpause poll
            ui_brightness_send_current();   // also calls wled_poll_set_paused(false)
            s_mode = NAV_MODE_NAVIGATE;
            apply_focus_style(widget, false);
        }
        break;

    case TARGET_TYPE_ACTION:
        lv_event_send(widget, LV_EVENT_CLICKED, NULL);
        // If this opened the picker, enter picker mode
        if (s_focus_idx == TARGET_COLOR && ui_color_picker_is_open()) {
            s_mode = NAV_MODE_COLOR_PICKER;
            // Highlight defaults to WHEEL (set inside open_color_picker already)
        }
        break;

    case TARGET_TYPE_DROPDOWN:
        s_dropdown_open_sel = lv_dropdown_get_selected(widget);
        lv_dropdown_open(widget);
        s_mode = NAV_MODE_DROPDOWN;
        apply_focus_style(widget, true);
        wled_poll_set_paused(true);   // prevent poll from resetting the selection
        break;
    }
}

static void preset_rotate(int dir)
{
    lv_obj_t *dd = ui_preset_dropdown;
    if (!dd) return;

    uint16_t count = lv_dropdown_get_option_cnt(dd);
    if (count == 0) return;

    int next = (int)lv_dropdown_get_selected(dd) + dir;
    if (next < 0) next = count - 1;
    if (next >= count) next = 0;
    lv_dropdown_set_selected(dd, (uint16_t)next);
}

static void preset_press(void)
{
    lv_obj_t *dd = ui_preset_dropdown;
    if (!dd) {
        s_mode = NAV_MODE_NAVIGATE;
        return;
    }

    bool changed = lv_dropdown_get_selected(dd) != s_dropdown_open_sel;
    if (lv_dropdown_is_open(dd))
        lv_dropdown_close(dd);

    s_mode = NAV_MODE_NAVIGATE;
    apply_focus_style(dd, false);
    wled_poll_set_paused(false);   // re-enable poll now selection is confirmed

    if (changed)
        lv_event_send(dd, LV_EVENT_VALUE_CHANGED, NULL);
}

// ── Color picker sub-navigation ───────────────────────────────────────────────
// Rotating traverses picker tab stops. Pressing selects the focused item.

static void picker_focus_move(int dir)
{
    picker_target_t focus = ui_color_picker_focus();
    int next = ((int)focus + dir + PICKER_TARGET_COUNT) % PICKER_TARGET_COUNT;
    ui_picker_set_highlight((picker_target_t)next);
}

static void picker_adjust(int dir)
{
    picker_target_t focus = ui_color_picker_focus();

    switch (focus) {
    case PICKER_TARGET_WHEEL: {
        // Rotate hue ring in 5° fixed steps (velocity on hue feels wrong)
        lv_obj_t *wheel = ui_color_picker_wheel();
        if (!wheel) break;
        lv_color_hsv_t hsv = lv_colorwheel_get_hsv(wheel);
        int hue = (int)hsv.h + dir * 5;
        if (hue < 0)    hue += 360;
        if (hue >= 360) hue -= 360;
        hsv.h = (uint16_t)hue;
        lv_colorwheel_set_hsv(wheel, hsv);
        // Trigger VALUE_CHANGED so the preview circle updates
        lv_event_send(wheel, LV_EVENT_VALUE_CHANGED, NULL);
        break;
    }
    case PICKER_TARGET_SAT: {
        // Adjust saturation with velocity scaling
        lv_obj_t *sat = ui_color_picker_sat();
        if (!sat) break;
        int step = velocity_step();
        int val  = lv_slider_get_value(sat) + dir * step;
        if (val < 0)   val = 0;
        if (val > 100) val = 100;
        lv_slider_set_value(sat, val, LV_ANIM_OFF);
        lv_event_send(sat, LV_EVENT_VALUE_CHANGED, NULL);
        break;
    }
    case PICKER_TARGET_APPLY:
    case PICKER_TARGET_CANCEL:
        break;
    default:
        break;
    }
}

static void picker_select(void)
{
    picker_target_t focus = ui_color_picker_focus();

    switch (focus) {
    case PICKER_TARGET_WHEEL:
    case PICKER_TARGET_SAT:
        s_mode = NAV_MODE_COLOR_EDIT;
        ui_picker_set_editing(focus, true);
        return;
    case PICKER_TARGET_APPLY:
        ui_color_picker_close_apply();
        s_mode = NAV_MODE_NAVIGATE;
        return;
    case PICKER_TARGET_CANCEL:
        ui_color_picker_close_cancel();
        s_mode = NAV_MODE_NAVIGATE;
        return;
    default:
        break;
    }
}

static void picker_exit_edit(void)
{
    picker_target_t focus = ui_color_picker_focus();
    ui_picker_set_editing(focus, false);
    s_mode = NAV_MODE_COLOR_PICKER;
}

// ── Main event callback ───────────────────────────────────────────────────────
static void on_encoder_event(encoder_event_t event, void *ctx)
{
    lvgl_lock();

    if (ui_color_picker_is_open() &&
        s_mode != NAV_MODE_COLOR_PICKER &&
        s_mode != NAV_MODE_COLOR_EDIT) {
        s_mode = NAV_MODE_COLOR_PICKER;
        ui_picker_set_highlight(ui_color_picker_focus());
    }

    // Detect if picker was closed externally (touch Cancel)
    if ((s_mode == NAV_MODE_COLOR_PICKER || s_mode == NAV_MODE_COLOR_EDIT) &&
        !ui_color_picker_is_open()) {
        s_mode = NAV_MODE_NAVIGATE;
        // Restore the main-screen focus highlight so the user can see where
        // the encoder is and doesn't accidentally re-open the picker on next press
        lv_obj_t *fw = focused_widget();
        if (fw) apply_focus_style(fw, false);
    }

    // Detect if the dropdown was closed externally.
    if (s_mode == NAV_MODE_DROPDOWN &&
        (!ui_preset_dropdown || !lv_dropdown_is_open(ui_preset_dropdown))) {
        s_mode = NAV_MODE_NAVIGATE;
        wled_poll_set_paused(false);
        if (ui_preset_dropdown)
            apply_focus_style(ui_preset_dropdown, false);
    }

    switch (event) {
    case ENCODER_EVENT_CW:
        if (s_mode == NAV_MODE_COLOR_PICKER) {
            picker_focus_move(+1);
        } else if (s_mode == NAV_MODE_COLOR_EDIT) {
            picker_adjust(+1);
        } else if (s_mode == NAV_MODE_DROPDOWN) {
            preset_rotate(+1);
        } else if (s_focus_idx < 0 || s_mode == NAV_MODE_NAVIGATE) {
            cycle_focus(+1);
        } else if (s_mode == NAV_MODE_EDIT) {
            // Edit mode — velocity-scaled slider
            lv_obj_t *w = focused_widget();
            if (w && k_targets[s_focus_idx].type == TARGET_TYPE_SLIDER) {
                int step = velocity_step();
                int v = lv_slider_get_value(w) + step;
                int mx = lv_slider_get_max_value(w);
                if (v > mx) v = mx;
                lv_slider_set_value(w, v, LV_ANIM_OFF);
                lv_event_send(w, LV_EVENT_VALUE_CHANGED, NULL);
            }
        }
        break;

    case ENCODER_EVENT_CCW:
        if (s_mode == NAV_MODE_COLOR_PICKER) {
            picker_focus_move(-1);
        } else if (s_mode == NAV_MODE_COLOR_EDIT) {
            picker_adjust(-1);
        } else if (s_mode == NAV_MODE_DROPDOWN) {
            preset_rotate(-1);
        } else if (s_focus_idx < 0 || s_mode == NAV_MODE_NAVIGATE) {
            cycle_focus(-1);
        } else if (s_mode == NAV_MODE_EDIT) {
            lv_obj_t *w = focused_widget();
            if (w && k_targets[s_focus_idx].type == TARGET_TYPE_SLIDER) {
                int step = velocity_step();
                int v = lv_slider_get_value(w) - step;
                int mn = lv_slider_get_min_value(w);
                if (v < mn) v = mn;
                lv_slider_set_value(w, v, LV_ANIM_OFF);
                lv_event_send(w, LV_EVENT_VALUE_CHANGED, NULL);
            }
        }
        break;

    case ENCODER_EVENT_PRESS:
        if (s_mode == NAV_MODE_COLOR_PICKER) {
            picker_select();
        } else if (s_mode == NAV_MODE_COLOR_EDIT) {
            picker_exit_edit();
        } else if (s_mode == NAV_MODE_DROPDOWN) {
            preset_press();
        } else {
            activate_focused();
        }
        break;
    }

    lvgl_unlock();
}

// ── Public API ────────────────────────────────────────────────────────────────
esp_err_t encoder_nav_init(void)
{
    for (int i = 0; i < TARGET_COUNT; i++) {
        if (!*k_targets[i].widget_ptr) {
            ESP_LOGE(TAG, "Target '%s' is NULL — call ui_build() first",
                     k_targets[i].name);
            return ESP_ERR_INVALID_STATE;
        }
    }
    ESP_RETURN_ON_ERROR(encoder_init(on_encoder_event, NULL), TAG, "encoder_init");
    ESP_LOGI(TAG, "Navigation ready (%d targets)", TARGET_COUNT);
    return ESP_OK;
}