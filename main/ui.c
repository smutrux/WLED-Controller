/**
 * ui.c — WLED Controller UI (Stage 10)
 *
 * Color picker modal internals:
 *   - lv_colorwheel for hue selection
 *   - Saturation slider (0=white, 100=full colour)
 *   - Split preview circle in the wheel centre: left=current, right=new
 *   - Apply / Cancel buttons
 *   - Encoder navigates between all 4 elements via ui_picker_targets[]
 *
 * Poll pause:
 *   Poll is paused while the brightness slider is being touched or the
 *   encoder is in EDIT mode, preventing the poll from snapping the widget
 *   back while the user is interacting with it.
 */

#include "ui.h"
#include "cmd/wled_cmd.h"
#include "poll/wled_poll.h"
#include "presets/wled_presets.h"
#include "board/board.h"
#include "esp_log.h"
#include "lvgl.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "ui";

// ── Colour constants ──────────────────────────────────────────────────────────
#define COL_BG          0x0D0D0D
#define COL_SURFACE     0x1A1A2E
#define COL_CARD        0x16213E
#define COL_BORDER      0x0F3460
#define COL_TEXT        0xE0E0E0
#define COL_TEXT_DIM    0x7A8194
#define COL_ACCENT      0x1565C0
#define COL_ACCENT2     0x42A5F5
#define COL_ON          0x1976D2
#define COL_OFF         0x37474F

// ── Picker preview circle geometry ───────────────────────────────────────────
#define PREVIEW_D       80   // diameter of the split circle in the wheel centre

// ── Widget handles ────────────────────────────────────────────────────────────
lv_obj_t *ui_status_label      = NULL;
lv_obj_t *ui_brightness_slider = NULL;
lv_obj_t *ui_brightness_label  = NULL;
lv_obj_t *ui_color_preview     = NULL;
lv_obj_t *ui_power_btn         = NULL;
lv_obj_t *ui_power_btn_label   = NULL;
lv_obj_t *ui_preset_dropdown   = NULL;

// ── Picker widget handles (exposed to encoder_nav) ────────────────────────────
static lv_obj_t *s_wifi_dot        = NULL;
static lv_obj_t *s_picker_modal    = NULL;
static lv_obj_t *s_colorwheel      = NULL;
static lv_obj_t *s_sat_slider      = NULL;
static lv_obj_t *s_apply_btn       = NULL;
static lv_obj_t *s_cancel_btn      = NULL;
static lv_obj_t *s_preview_new     = NULL;   // right half of split circle
static bool      s_picker_open     = false;

// ── Picker navigation — picker_target_t is defined in ui.h ─────────────────

static picker_target_t s_picker_focus = PICKER_TARGET_WHEEL;

// Expose picker state to encoder_nav
bool      ui_color_picker_is_open(void)   { return s_picker_open; }
lv_obj_t *ui_color_picker_wheel(void)     { return s_colorwheel; }
lv_obj_t *ui_color_picker_sat(void)       { return s_sat_slider; }
lv_obj_t *ui_color_picker_apply(void)     { return s_apply_btn;  }
lv_obj_t *ui_color_picker_cancel(void)    { return s_cancel_btn; }
picker_target_t ui_color_picker_focus(void) { return s_picker_focus; }

void ui_color_picker_set_focus(picker_target_t t)
{
    s_picker_focus = t;
}

// ── Local WLED state ──────────────────────────────────────────────────────────
static bool     wled_on         = true;
static uint8_t  wled_brightness = 128;
static uint32_t wled_color      = 0x1565C0;

// ── Helpers ───────────────────────────────────────────────────────────────────
static uint32_t lv_color_to_u32(lv_color_t c)
{
    lv_color32_t c32;
    c32.full = lv_color_to32(c);
    return ((uint32_t)c32.ch.red   << 16)
         | ((uint32_t)c32.ch.green <<  8)
         |  (uint32_t)c32.ch.blue;
}

static uint32_t picker_get_rgb(void)
{
    if (!s_colorwheel) return wled_color;
    lv_color_hsv_t hsv = lv_colorwheel_get_hsv(s_colorwheel);
    if (s_sat_slider)
        hsv.s = (uint8_t)lv_slider_get_value(s_sat_slider);
    lv_color_t rgb = lv_color_hsv_to_rgb(hsv.h, hsv.s, hsv.v);
    return lv_color_to_u32(rgb);
}

// Update the right half (new-colour preview) of the split circle
static void update_preview_new(void)
{
    if (!s_preview_new) return;
    uint32_t rgb = picker_get_rgb();
    lv_obj_set_style_bg_color(s_preview_new, lv_color_hex(rgb), 0);
}

// Apply a focus highlight to a picker widget
static void picker_set_highlight(lv_obj_t *w, bool on)
{
    if (!w) return;
    lv_color_t c = lv_palette_main(LV_PALETTE_LIGHT_BLUE);
    lv_obj_set_style_outline_color(w, c, 0);
    lv_obj_set_style_outline_width(w, on ? 2 : 0, 0);
    lv_obj_set_style_outline_pad(w, 3, 0);
    lv_obj_set_style_outline_opa(w, on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
}

// ── Color picker modal ────────────────────────────────────────────────────────

static void picker_close(bool apply)
{
    if (apply) {
        uint32_t rgb = picker_get_rgb();
        wled_color = rgb;
        if (ui_color_preview)
            lv_obj_set_style_bg_color(ui_color_preview, lv_color_hex(rgb), 0);
        wled_cmd_set_color(rgb);
    }
    lv_obj_del(s_picker_modal);
    s_picker_modal  = NULL;
    s_colorwheel    = NULL;
    s_sat_slider    = NULL;
    s_apply_btn     = NULL;
    s_cancel_btn    = NULL;
    s_preview_new   = NULL;
    s_picker_open   = false;
    s_picker_focus  = PICKER_TARGET_WHEEL;
    wled_poll_set_paused(false);
}

static void picker_apply_cb(lv_event_t *e)  { picker_close(true);  }
static void picker_cancel_cb(lv_event_t *e) { picker_close(false); }

void ui_color_picker_close_apply(void)  { if (s_picker_open) picker_close(true);  }
void ui_color_picker_close_cancel(void) { if (s_picker_open) picker_close(false); }

// Update preview whenever colorwheel or sat slider changes
static void on_picker_value_change(lv_event_t *e) { update_preview_new(); }

static void open_color_picker(void)
{
    if (s_picker_open) return;
    s_picker_open  = true;
    s_picker_focus = PICKER_TARGET_WHEEL;
    wled_poll_set_paused(true);

    s_picker_modal = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_picker_modal, LCD_H_RES, LCD_V_RES);
    lv_obj_align(s_picker_modal, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(s_picker_modal, lv_color_hex(0x0A0A1A), 0);
    lv_obj_set_style_bg_opa(s_picker_modal, LV_OPA_90, 0);
    lv_obj_set_style_border_width(s_picker_modal, 0, 0);
    lv_obj_set_style_radius(s_picker_modal, 0, 0);
    lv_obj_clear_flag(s_picker_modal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_picker_modal);
    lv_label_set_text(title, "Choose Color");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COL_TEXT), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    // ── Colorwheel ────────────────────────────────────────────────────────────
    s_colorwheel = lv_colorwheel_create(s_picker_modal, true);
    lv_obj_set_size(s_colorwheel, 220, 220);
    lv_obj_align(s_colorwheel, LV_ALIGN_TOP_MID, 0, 36);
    lv_colorwheel_set_rgb(s_colorwheel, lv_color_hex(wled_color));
    // Hide the default inner knob — we'll draw our own split circle on top
    lv_obj_set_style_bg_opa(s_colorwheel, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_border_opa(s_colorwheel, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_add_event_cb(s_colorwheel, on_picker_value_change,
                        LV_EVENT_VALUE_CHANGED, NULL);

    // ── Split preview circle in the wheel centre ──────────────────────────────
    // Container — clips children to a circle
    lv_obj_t *preview_ring = lv_obj_create(s_picker_modal);
    lv_obj_set_size(preview_ring, PREVIEW_D, PREVIEW_D);
    lv_obj_align_to(preview_ring, s_colorwheel, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(preview_ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_clip_corner(preview_ring, true, 0);
    lv_obj_set_style_border_width(preview_ring, 1, 0);
    lv_obj_set_style_border_color(preview_ring, lv_color_hex(COL_BORDER), 0);
    lv_obj_set_style_pad_all(preview_ring, 0, 0);
    lv_obj_clear_flag(preview_ring, LV_OBJ_FLAG_SCROLLABLE);

    // Left half — current colour (static until Apply)
    lv_obj_t *preview_old = lv_obj_create(preview_ring);
    lv_obj_set_size(preview_old, PREVIEW_D / 2, PREVIEW_D);
    lv_obj_align(preview_old, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(preview_old, lv_color_hex(wled_color), 0);
    lv_obj_set_style_border_width(preview_old, 0, 0);
    lv_obj_set_style_radius(preview_old, 0, 0);

    // Right half — new colour (updates live as wheel/sat change)
    s_preview_new = lv_obj_create(preview_ring);
    lv_obj_set_size(s_preview_new, PREVIEW_D / 2, PREVIEW_D);
    lv_obj_align(s_preview_new, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(s_preview_new, lv_color_hex(wled_color), 0);
    lv_obj_set_style_border_width(s_preview_new, 0, 0);
    lv_obj_set_style_radius(s_preview_new, 0, 0);

    // ── Saturation label + slider ─────────────────────────────────────────────
    lv_obj_t *sat_lbl = lv_label_create(s_picker_modal);
    lv_label_set_text(sat_lbl, "Saturation  (left = white)");
    lv_obj_set_style_text_font(sat_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(sat_lbl, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_align(sat_lbl, LV_ALIGN_TOP_LEFT, 14, 266);

    s_sat_slider = lv_slider_create(s_picker_modal);
    lv_slider_set_range(s_sat_slider, 0, 100);
    lv_color_hsv_t hsv = lv_colorwheel_get_hsv(s_colorwheel);
    lv_slider_set_value(s_sat_slider, hsv.s, LV_ANIM_OFF);
    lv_obj_set_size(s_sat_slider, LCD_H_RES - 28, 8);
    lv_obj_align(s_sat_slider, LV_ALIGN_TOP_MID, 0, 286);
    lv_obj_set_style_bg_color(s_sat_slider, lv_color_hex(0x2A2A3E), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_sat_slider, lv_color_hex(COL_ACCENT2), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_sat_slider, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_sat_slider, 8, LV_PART_KNOB);
    lv_obj_add_event_cb(s_sat_slider, on_picker_value_change,
                        LV_EVENT_VALUE_CHANGED, NULL);

    // ── Apply / Cancel buttons ────────────────────────────────────────────────
    s_apply_btn = lv_btn_create(s_picker_modal);
    lv_obj_set_size(s_apply_btn, 130, 44);
    lv_obj_align(s_apply_btn, LV_ALIGN_BOTTOM_LEFT, 12, -12);
    lv_obj_set_style_bg_color(s_apply_btn, lv_color_hex(COL_ON), 0);
    lv_obj_set_style_radius(s_apply_btn, 8, 0);
    lv_obj_add_event_cb(s_apply_btn, picker_apply_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *apply_lbl = lv_label_create(s_apply_btn);
    lv_label_set_text(apply_lbl, LV_SYMBOL_OK "  Apply");
    lv_obj_set_style_text_font(apply_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(apply_lbl);

    s_cancel_btn = lv_btn_create(s_picker_modal);
    lv_obj_set_size(s_cancel_btn, 130, 44);
    lv_obj_align(s_cancel_btn, LV_ALIGN_BOTTOM_RIGHT, -12, -12);
    lv_obj_set_style_bg_color(s_cancel_btn, lv_color_hex(COL_OFF), 0);
    lv_obj_set_style_radius(s_cancel_btn, 8, 0);
    lv_obj_add_event_cb(s_cancel_btn, picker_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cancel_lbl = lv_label_create(s_cancel_btn);
    lv_label_set_text(cancel_lbl, LV_SYMBOL_CLOSE "  Cancel");
    lv_obj_set_style_text_font(cancel_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(cancel_lbl);

    // Initial highlight on the colorwheel
    picker_set_highlight(s_colorwheel, true);
}

// ── Event handlers ────────────────────────────────────────────────────────────

static void on_power_click(lv_event_t *e)
{
    wled_on = !wled_on;
    lv_label_set_text(ui_power_btn_label, wled_on ? "ON" : "OFF");
    lv_obj_set_style_bg_color(ui_power_btn,
        wled_on ? lv_color_hex(COL_ON) : lv_color_hex(COL_OFF), 0);
    wled_cmd_set_power(wled_on);
}

static void on_brightness_pressed(lv_event_t *e)
{
    wled_poll_set_paused(true);
}

static void on_brightness_label_update(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    wled_brightness  = (uint8_t)lv_slider_get_value(slider);
    lv_label_set_text_fmt(ui_brightness_label, "%.0f", wled_brightness/2.55);
}

static void on_brightness_released(lv_event_t *e)
{
    wled_cmd_set_brightness(wled_brightness);
    wled_poll_set_paused(false);
}

static void on_color_tap(lv_event_t *e) { open_color_picker(); }

static void on_preset_change(lv_event_t *e)
{
    lv_obj_t *dd = lv_event_get_target(e);
    int idx = (int)lv_dropdown_get_selected(dd);
    int wled_id = wled_presets_get_id(idx);
    if (wled_id > 0) {
        ESP_LOGI(TAG, "Preset selected: idx=%d id=%d", idx, wled_id);
        wled_cmd_apply_preset(wled_id);
    }
}

// ── Build ─────────────────────────────────────────────────────────────────────
void ui_build(void)
{
    lv_theme_t *theme = lv_theme_default_init(
        lv_disp_get_default(),
        lv_palette_main(LV_PALETTE_BLUE),
        lv_palette_main(LV_PALETTE_LIGHT_BLUE),
        true, &lv_font_montserrat_16
    );
    lv_disp_set_theme(lv_disp_get_default(), theme);

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // ── Header ────────────────────────────────────────────────────────────────
    lv_obj_t *header = lv_obj_create(scr);
    lv_obj_set_size(header, LCD_H_RES, 52);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(header, lv_color_hex(COL_SURFACE), 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_set_style_pad_hor(header, 12, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    s_wifi_dot = lv_obj_create(header);
    lv_obj_set_size(s_wifi_dot, 10, 10);
    lv_obj_align(s_wifi_dot, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_radius(s_wifi_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_wifi_dot, lv_color_hex(0x555555), 0);
    lv_obj_set_style_border_width(s_wifi_dot, 0, 0);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, "WLED Controller");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COL_TEXT), 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 18, 0);

    ui_power_btn = lv_btn_create(header);
    lv_obj_set_size(ui_power_btn, 56, 32);
    lv_obj_align(ui_power_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(ui_power_btn,
        wled_on ? lv_color_hex(COL_ON) : lv_color_hex(COL_OFF), 0);
    lv_obj_set_style_radius(ui_power_btn, 6, 0);
    lv_obj_add_event_cb(ui_power_btn, on_power_click, LV_EVENT_CLICKED, NULL);

    ui_power_btn_label = lv_label_create(ui_power_btn);
    lv_label_set_text(ui_power_btn_label, wled_on ? "ON" : "OFF");
    lv_obj_set_style_text_font(ui_power_btn_label, &lv_font_montserrat_14, 0);
    lv_obj_center(ui_power_btn_label);

    // ── Status card ───────────────────────────────────────────────────────────
    lv_obj_t *card = lv_obj_create(scr);
    lv_obj_set_size(card, LCD_H_RES - 24, 72);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 58);
    lv_obj_set_style_bg_color(card, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_border_color(card, lv_color_hex(COL_BORDER), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    ui_status_label = lv_label_create(card);
    lv_label_set_text(ui_status_label, "Connecting...\n—");
    lv_obj_set_style_text_font(ui_status_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(ui_status_label, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_align(ui_status_label, LV_ALIGN_LEFT_MID, 0, 0);

    // ── Brightness ────────────────────────────────────────────────────────────
    lv_obj_t *bri_lbl = lv_label_create(scr);
    lv_label_set_text(bri_lbl, "Brightness");
    lv_obj_set_style_text_font(bri_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(bri_lbl, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_align(bri_lbl, LV_ALIGN_TOP_LEFT, 14, 142);

    ui_brightness_slider = lv_slider_create(scr);
    lv_slider_set_range(ui_brightness_slider, 0, 255);
    lv_slider_set_value(ui_brightness_slider, wled_brightness, LV_ANIM_OFF);
    lv_obj_set_size(ui_brightness_slider, LCD_H_RES - 80, 8);
    lv_obj_align(ui_brightness_slider, LV_ALIGN_TOP_LEFT, 14, 168);
    lv_obj_set_style_bg_color(ui_brightness_slider, lv_color_hex(0x2A2A3E), LV_PART_MAIN);
    lv_obj_set_style_bg_color(ui_brightness_slider, lv_color_hex(COL_ACCENT2), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(ui_brightness_slider, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
    lv_obj_set_style_pad_all(ui_brightness_slider, 8, LV_PART_KNOB);

    lv_obj_add_event_cb(ui_brightness_slider, on_brightness_pressed,
                        LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(ui_brightness_slider, on_brightness_label_update,
                        LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(ui_brightness_slider, on_brightness_released,
                        LV_EVENT_RELEASED, NULL);

    ui_brightness_label = lv_label_create(scr);
    lv_obj_set_style_text_font(ui_brightness_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(ui_brightness_label, lv_color_hex(COL_ACCENT2), 0);
    lv_obj_align_to(ui_brightness_label, ui_brightness_slider,
                    LV_ALIGN_OUT_RIGHT_MID, 20, 0);
    lv_label_set_text_fmt(ui_brightness_label, "%.0f", wled_brightness/2.55);

    // ── Color preview ─────────────────────────────────────────────────────────
    lv_obj_t *col_heading = lv_label_create(scr);
    lv_label_set_text(col_heading, "Color");
    lv_obj_set_style_text_font(col_heading, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(col_heading, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_align(col_heading, LV_ALIGN_TOP_LEFT, 14, 206);

    ui_color_preview = lv_obj_create(scr);
    lv_obj_set_size(ui_color_preview, 64, 36);
    lv_obj_align(ui_color_preview, LV_ALIGN_TOP_LEFT, 14, 228);
    lv_obj_set_style_bg_color(ui_color_preview, lv_color_hex(wled_color), 0);
    lv_obj_set_style_border_color(ui_color_preview, lv_color_hex(COL_BORDER), 0);
    lv_obj_set_style_border_width(ui_color_preview, 1, 0);
    lv_obj_set_style_radius(ui_color_preview, 6, 0);
    lv_obj_add_flag(ui_color_preview, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ui_color_preview, on_color_tap, LV_EVENT_CLICKED, NULL);

    lv_obj_t *col_hint = lv_label_create(scr);
    lv_label_set_text(col_hint, "Tap to change");
    lv_obj_set_style_text_font(col_hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(col_hint, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_align_to(col_hint, ui_color_preview, LV_ALIGN_OUT_RIGHT_MID, 12, 0);

    // ── Preset dropdown ───────────────────────────────────────────────────────
    lv_obj_t *pre_heading = lv_label_create(scr);
    lv_label_set_text(pre_heading, "Preset");
    lv_obj_set_style_text_font(pre_heading, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(pre_heading, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_align(pre_heading, LV_ALIGN_TOP_LEFT, 14, 282);

    ui_preset_dropdown = lv_dropdown_create(scr);
    lv_dropdown_set_options(ui_preset_dropdown, "Loading presets...");
    lv_obj_set_size(ui_preset_dropdown, LCD_H_RES - 28, 40);
    lv_obj_align(ui_preset_dropdown, LV_ALIGN_TOP_MID, 0, 304);
    lv_obj_set_style_bg_color(ui_preset_dropdown, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_border_color(ui_preset_dropdown, lv_color_hex(COL_BORDER), 0);
    lv_obj_set_style_border_width(ui_preset_dropdown, 1, 0);
    lv_obj_set_style_text_color(ui_preset_dropdown, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_font(ui_preset_dropdown, &lv_font_montserrat_14, 0);
    lv_obj_set_style_radius(ui_preset_dropdown, 8, 0);
    lv_obj_set_style_pad_hor(ui_preset_dropdown, 12, 0);

    lv_obj_t *list = lv_dropdown_get_list(ui_preset_dropdown);
    lv_obj_set_style_bg_color(list, lv_color_hex(COL_SURFACE), 0);
    lv_obj_set_style_border_color(list, lv_color_hex(COL_BORDER), 0);
    lv_obj_set_style_text_color(list, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_font(list, &lv_font_montserrat_14, 0);
    lv_obj_set_style_bg_color(list, lv_color_hex(COL_ACCENT),
                               LV_PART_SELECTED | LV_STATE_CHECKED);

    lv_obj_add_event_cb(ui_preset_dropdown, on_preset_change,
                        LV_EVENT_VALUE_CHANGED, NULL);

    ESP_LOGI(TAG, "UI built");
}

// ── Public update functions ───────────────────────────────────────────────────

void ui_set_status(const char *line1, const char *line2)
{
    if (!ui_status_label) return;
    char buf[96];
    snprintf(buf, sizeof(buf), "%s\n%s", line1, line2);
    lv_label_set_text(ui_status_label, buf);
}

void ui_set_brightness(uint8_t bri)
{
    wled_brightness = bri;
    if (ui_brightness_slider)
        lv_slider_set_value(ui_brightness_slider, bri, LV_ANIM_ON);
    if (ui_brightness_label)
        lv_label_set_text_fmt(ui_brightness_label, "%.0f", bri/2.55);
}

void ui_set_color(uint32_t rgb)
{
    wled_color = rgb;
    if (ui_color_preview)
        lv_obj_set_style_bg_color(ui_color_preview, lv_color_hex(rgb), 0);
    if (s_colorwheel)
        lv_colorwheel_set_rgb(s_colorwheel, lv_color_hex(rgb));
}

void ui_set_power(bool on)
{
    wled_on = on;
    if (ui_power_btn)
        lv_obj_set_style_bg_color(ui_power_btn,
            on ? lv_color_hex(COL_ON) : lv_color_hex(COL_OFF), 0);
    if (ui_power_btn_label)
        lv_label_set_text(ui_power_btn_label, on ? "ON" : "OFF");
}

void ui_set_wifi_status(wifi_conn_state_t state)
{
    if (!s_wifi_dot) return;
    lv_color_t color;
    switch (state) {
    case WIFI_STATE_CONNECTED:    color = lv_palette_main(LV_PALETTE_GREEN);  break;
    case WIFI_STATE_CONNECTING:   color = lv_palette_main(LV_PALETTE_YELLOW); break;
    default:                      color = lv_color_hex(0x555555);             break;
    }
    lv_obj_set_style_bg_color(s_wifi_dot, color, 0);
}

void ui_set_effect(int fx_index)   { (void)fx_index; }

void ui_set_preset(int display_index, int wled_id)
{
    if (!ui_preset_dropdown || display_index < 0) return;
    lv_obj_remove_event_cb(ui_preset_dropdown, on_preset_change);
    lv_dropdown_set_selected(ui_preset_dropdown, (uint16_t)display_index);
    lv_obj_add_event_cb(ui_preset_dropdown, on_preset_change,
                        LV_EVENT_VALUE_CHANGED, NULL);
}

void ui_presets_loaded(void)
{
    if (!ui_preset_dropdown) return;
    static char opts[WLED_PRESETS_MAX * (WLED_PRESET_NAME_LEN + 1)];
    int n = wled_presets_build_options(opts, sizeof(opts));
    lv_dropdown_set_options(ui_preset_dropdown,
                            n > 0 ? opts : "No presets found");
    if (n > 0) ESP_LOGI(TAG, "Preset dropdown populated (%d entries)", n);
}

void ui_brightness_send_current(void)
{
    wled_cmd_set_brightness(wled_brightness);
    wled_poll_set_paused(false);
}

// ── Picker focus highlight helper (called from encoder_nav) ───────────────────
void ui_picker_set_highlight(picker_target_t t)
{
    // Clear all highlights first
    picker_set_highlight(s_colorwheel,  false);
    picker_set_highlight(s_sat_slider,  false);
    picker_set_highlight(s_apply_btn,   false);
    picker_set_highlight(s_cancel_btn,  false);

    // Apply to the new target
    lv_obj_t *targets[PICKER_TARGET_COUNT] = {
        s_colorwheel, s_sat_slider, s_apply_btn, s_cancel_btn
    };
    if (t < PICKER_TARGET_COUNT && targets[t]) {
        picker_set_highlight(targets[t], true);
    }
    s_picker_focus = t;
}