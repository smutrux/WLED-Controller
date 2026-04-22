/**
 * encoder_nav.c — Focus/edit navigation layer for the rotary encoder
 *
 * Consumes raw encoder events from encoder.c and translates them into
 * UI interactions: cycling widget focus, toggling buttons, adjusting sliders.
 *
 * All LVGL calls are wrapped with lvgl_lock() / lvgl_unlock() because this
 * callback runs in the encoder_task context (core 0), not the LVGL task.
 */

#include "encoder/encoder_nav.h"
#include "encoder/encoder.h"
#include "ui.h"
#include "board/board.h"
#include "esp_log.h"
#include "lvgl.h"
#include "esp_check.h"

static const char *TAG = "enc_nav";

// ── Brightness step ───────────────────────────────────────────────────────────
// How many LVGL units (0-255) to move per detent while editing brightness.
// 4 ≈ 64 steps across the full range, snappy but not jumpy.
#define BRIGHTNESS_STEP 4

// ── Target descriptors ────────────────────────────────────────────────────────

typedef enum {
    TARGET_POWER = 0,
    TARGET_BRIGHTNESS,
    TARGET_COLOR,        // placeholder — invokes on_color_tap when pressed
    TARGET_COUNT
} target_id_t;

typedef enum {
    TARGET_TYPE_BUTTON,  // press = toggle
    TARGET_TYPE_SLIDER,  // press = enter edit; rotate = change value
    TARGET_TYPE_ACTION,  // press = fire click event (same as touch)
} target_type_t;

typedef struct {
    lv_obj_t      **widget_ptr;   // pointer to the widget handle (extern from ui.h)
    target_type_t   type;
    const char     *name;         // for logging
} nav_target_t;

static const nav_target_t k_targets[TARGET_COUNT] = {
    [TARGET_POWER]      = { &ui_power_btn,         TARGET_TYPE_BUTTON, "Power"      },
    [TARGET_BRIGHTNESS] = { &ui_brightness_slider,  TARGET_TYPE_SLIDER, "Brightness" },
    [TARGET_COLOR]      = { &ui_color_preview,      TARGET_TYPE_ACTION, "Color"      },
};

// ── Navigation state ──────────────────────────────────────────────────────────

typedef enum {
    NAV_MODE_NAVIGATE,   // rotating cycles focus between targets
    NAV_MODE_EDIT,       // rotating changes the focused slider's value
} nav_mode_t;

static int         s_focus_idx = -1;          // -1 = nothing focused yet
static nav_mode_t  s_mode      = NAV_MODE_NAVIGATE;

// ── LVGL focus styling ────────────────────────────────────────────────────────

// Apply an outline to a widget to show it is focused/active.
// is_edit=true → cyan (editing); is_edit=false → orange (navigating).
static void apply_focus_style(lv_obj_t *widget, bool is_edit)
{
    lv_color_t color = is_edit
        ? lv_palette_main(LV_PALETTE_CYAN)
        : lv_palette_main(LV_PALETTE_ORANGE);

    lv_obj_set_style_outline_color(widget, color, 0);
    lv_obj_set_style_outline_width(widget, 2, 0);
    lv_obj_set_style_outline_pad(widget, 3, 0);
    lv_obj_set_style_outline_opa(widget, LV_OPA_COVER, 0);

    // Extra feedback for sliders: enlarge the knob when in edit mode
    if (lv_obj_check_type(widget, &lv_slider_class)) {
        int pad = is_edit ? 12 : 8;
        lv_obj_set_style_pad_all(widget, pad, LV_PART_KNOB);
    }
}

// Remove all focus styling from a widget.
static void clear_focus_style(lv_obj_t *widget)
{
    lv_obj_set_style_outline_opa(widget, LV_OPA_TRANSP, 0);
    lv_obj_set_style_outline_width(widget, 0, 0);

    // Reset slider knob padding to default
    if (lv_obj_check_type(widget, &lv_slider_class)) {
        lv_obj_set_style_pad_all(widget, 8, LV_PART_KNOB);
    }
}

// Convenience: get the actual lv_obj_t* for the current focus index.
static lv_obj_t *focused_widget(void)
{
    if (s_focus_idx < 0 || s_focus_idx >= TARGET_COUNT) return NULL;
    return *k_targets[s_focus_idx].widget_ptr;
}

// ── Navigation actions ────────────────────────────────────────────────────────

// Move focus to a new target index, updating outline styles.
static void set_focus(int new_idx)
{
    // Remove highlight from old target
    lv_obj_t *old = focused_widget();
    if (old) clear_focus_style(old);

    s_focus_idx = new_idx;
    s_mode      = NAV_MODE_NAVIGATE;

    lv_obj_t *nw = focused_widget();
    if (nw) {
        apply_focus_style(nw, false);
        ESP_LOGI(TAG, "Focus → %s", k_targets[new_idx].name);
    }
}

// Cycle focus forward (+1) or backward (-1), wrapping around.
static void cycle_focus(int direction)
{
    int next;
    if (s_focus_idx < 0) {
        // First encoder interaction: land on the first target
        next = (direction > 0) ? 0 : TARGET_COUNT - 1;
    } else {
        next = (s_focus_idx + direction + TARGET_COUNT) % TARGET_COUNT;
    }
    set_focus(next);
}

// Activate (press) the currently focused target.
static void activate_focused(void)
{
    if (s_focus_idx < 0) {
        // Nothing focused yet — pressing enters focus on first target
        set_focus(0);
        return;
    }

    lv_obj_t *widget = focused_widget();
    if (!widget) return;

    const nav_target_t *t = &k_targets[s_focus_idx];

    switch (t->type) {
    case TARGET_TYPE_BUTTON:
        // Fire a click event — the button's on_power_click handler does the rest
        ESP_LOGI(TAG, "Encoder press → toggle %s", t->name);
        lv_event_send(widget, LV_EVENT_CLICKED, NULL);
        break;

    case TARGET_TYPE_SLIDER:
        if (s_mode == NAV_MODE_NAVIGATE) {
            // Enter edit mode
            s_mode = NAV_MODE_EDIT;
            apply_focus_style(widget, true);   // switch to cyan outline
            ESP_LOGI(TAG, "Encoder press → edit %s", t->name);
        } else {
            // Exit edit mode, return to navigate
            s_mode = NAV_MODE_NAVIGATE;
            apply_focus_style(widget, false);  // back to orange outline
            ESP_LOGI(TAG, "Encoder press → deselect %s", t->name);
        }
        break;

    case TARGET_TYPE_ACTION:
        // Fire a click event (color picker, etc.)
        ESP_LOGI(TAG, "Encoder press → action %s", t->name);
        lv_event_send(widget, LV_EVENT_CLICKED, NULL);
        break;
    }
}

// Handle a rotation event given the current mode.
static void handle_rotation(int direction)  // direction: +1=CW, -1=CCW
{
    if (s_focus_idx < 0 || s_mode == NAV_MODE_NAVIGATE) {
        // In navigate mode: cycle between targets
        cycle_focus(direction);
        return;
    }

    // In edit mode: adjust the focused slider
    lv_obj_t *widget = focused_widget();
    if (!widget) return;

    const nav_target_t *t = &k_targets[s_focus_idx];
    if (t->type != TARGET_TYPE_SLIDER) return;

    int current = lv_slider_get_value(widget);
    int min     = lv_slider_get_min_value(widget);
    int max     = lv_slider_get_max_value(widget);
    int next    = current + direction * BRIGHTNESS_STEP;

    // Clamp to slider range
    if (next < min) next = min;
    if (next > max) next = max;

    if (next != current) {
        lv_slider_set_value(widget, next, LV_ANIM_OFF);
        // Fire VALUE_CHANGED so on_brightness_change() updates the label
        lv_event_send(widget, LV_EVENT_VALUE_CHANGED, NULL);
    }
}

// ── Encoder event callback ────────────────────────────────────────────────────

static void on_encoder_event(encoder_event_t event, void *ctx)
{
    lvgl_lock();

    switch (event) {
    case ENCODER_EVENT_CW:
        handle_rotation(+1);
        break;
    case ENCODER_EVENT_CCW:
        handle_rotation(-1);
        break;
    case ENCODER_EVENT_PRESS:
        activate_focused();
        break;
    }

    lvgl_unlock();
}

// ── Public API ────────────────────────────────────────────────────────────────

esp_err_t encoder_nav_init(void)
{
    // Validate widget handles so a missing ui_build() call fails loudly
    for (int i = 0; i < TARGET_COUNT; i++) {
        if (!*k_targets[i].widget_ptr) {
            ESP_LOGE(TAG, "Target widget '%s' is NULL — call ui_build() first",
                     k_targets[i].name);
            return ESP_ERR_INVALID_STATE;
        }
    }

    ESP_RETURN_ON_ERROR(encoder_init(on_encoder_event, NULL),
                        TAG, "encoder_init");

    ESP_LOGI(TAG, "Navigation ready (%d targets)", TARGET_COUNT);
    return ESP_OK;
}
