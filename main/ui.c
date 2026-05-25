/**
 * ui.c — WLED Controller UI  (Stage 10)
 *
 * Layout (portrait 320×480):
 *   ┌─────────────────────────────┐
 *   │  ● WLED Controller    [ON]  │  52 px header
 *   ├─────────────────────────────┤
 *   │  ON | Cozy Evening          │  72 px status card
 *   │  Bri: 128  #FF6600          │
 *   ├─────────────────────────────┤
 *   │  Brightness          [128]  │
 *   │  ━━━━━━━━━━━━━━●━━━━━━━━━  │  60 px
 *   ├─────────────────────────────┤
 *   │  Color                      │
 *   │  ████  Tap to change        │  60 px (swatch opens full picker)
 *   ├─────────────────────────────┤
 *   │  Preset                     │
 *   │  ┌──────────────────────▼┐  │  60 px dropdown
 *   │  └────────────────────────┘ │
 *   └─────────────────────────────┘
 *
 * Color picker:
 *   Tap the swatch → full-screen modal with lv_colorwheel + Apply/Cancel.
 *   Encoder nav: wheel is TARGET_TYPE_ACTION (opens modal on press).
 *   Inside modal the encoder is not wired — touch only for colour picking.
 *
 * Theme: dark, blue primary, light-blue secondary.
 */

#include "ui.h"
#include "cmd/wled_cmd.h"
#include "presets/wled_presets.h"
#include "board/board.h"
#include "esp_log.h"
#include "lvgl.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "ui";

// ── Colour constants ──────────────────────────────────────────────────────────
#define COL_BG          0x0D0D0D
#define COL_SURFACE     0x1A1A2E   // deep blue-tinted dark
#define COL_CARD        0x16213E   // card background
#define COL_BORDER      0x0F3460   // subtle blue border
#define COL_TEXT        0xE0E0E0
#define COL_TEXT_DIM    0x7A8194
#define COL_ACCENT      0x1565C0   // primary blue (matches LV_PALETTE_BLUE darken2)
#define COL_ACCENT2     0x42A5F5   // light blue (LV_PALETTE_LIGHT_BLUE)
#define COL_ON          0x1976D2   // power ON blue
#define COL_OFF         0x37474F   // power OFF grey

// ── Widget handles (extern in ui.h) ──────────────────────────────────────────
lv_obj_t *ui_status_label      = NULL;
lv_obj_t *ui_brightness_slider = NULL;
lv_obj_t *ui_brightness_label  = NULL;
lv_obj_t *ui_color_preview     = NULL;
lv_obj_t *ui_power_btn         = NULL;
lv_obj_t *ui_power_btn_label   = NULL;

// ── Private widget handles ────────────────────────────────────────────────────
static lv_obj_t *s_wifi_dot      = NULL;
lv_obj_t *ui_preset_dropdown    = NULL;   // exposed for encoder_nav
static lv_obj_t *s_picker_modal  = NULL;   // color picker overlay
static lv_obj_t *s_colorwheel    = NULL;   // lv_colorwheel inside modal

// ── Local WLED state (kept in sync by poll task) ──────────────────────────────
static bool     wled_on         = true;
static uint8_t  wled_brightness = 128;
static uint32_t wled_color      = 0x1565C0;   // start with blue
static int      wled_preset_idx = -1;         // display index in dropdown

// ── Helper: convert lv_color_t to 0x00RRGGBB uint32 ──────────────────────────
static uint32_t lv_color_to_u32(lv_color_t c)
{
    lv_color32_t c32;
    c32.full = lv_color_to32(c);
    return ((uint32_t)c32.ch.red   << 16)
         | ((uint32_t)c32.ch.green <<  8)
         |  (uint32_t)c32.ch.blue;
}

// ── Color picker modal ────────────────────────────────────────────────────────

static void picker_apply_cb(lv_event_t *e)
{
    lv_color_t picked = lv_colorwheel_get_rgb(s_colorwheel);
    uint32_t rgb = lv_color_to_u32(picked);
    wled_color = rgb;
    lv_obj_set_style_bg_color(ui_color_preview, lv_color_hex(rgb), 0);
    wled_cmd_set_color(rgb);

    lv_obj_del(s_picker_modal);
    s_picker_modal = NULL;
    s_colorwheel   = NULL;
}

static void picker_cancel_cb(lv_event_t *e)
{
    lv_obj_del(s_picker_modal);
    s_picker_modal = NULL;
    s_colorwheel   = NULL;
}

static void open_color_picker(void)
{
    if (s_picker_modal) return;  // already open

    // Full-screen dark overlay
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
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 14);

    // Colorwheel — centred, size leaves room for buttons
    s_colorwheel = lv_colorwheel_create(s_picker_modal, true);
    lv_obj_set_size(s_colorwheel, 240, 240);
    lv_obj_align(s_colorwheel, LV_ALIGN_TOP_MID, 0, 46);
    lv_colorwheel_set_rgb(s_colorwheel, lv_color_hex(wled_color));
    // Style the inner circle (current colour swatch)
    lv_obj_set_style_bg_color(s_colorwheel, lv_color_hex(wled_color),
                               LV_PART_KNOB);

    // Apply button
    lv_obj_t *apply_btn = lv_btn_create(s_picker_modal);
    lv_obj_set_size(apply_btn, 120, 44);
    lv_obj_align(apply_btn, LV_ALIGN_BOTTOM_LEFT, 16, -16);
    lv_obj_set_style_bg_color(apply_btn, lv_color_hex(COL_ON), 0);
    lv_obj_set_style_radius(apply_btn, 8, 0);
    lv_obj_add_event_cb(apply_btn, picker_apply_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *apply_lbl = lv_label_create(apply_btn);
    lv_label_set_text(apply_lbl, "Apply");
    lv_obj_set_style_text_font(apply_lbl, &lv_font_montserrat_16, 0);
    lv_obj_center(apply_lbl);

    // Cancel button
    lv_obj_t *cancel_btn = lv_btn_create(s_picker_modal);
    lv_obj_set_size(cancel_btn, 120, 44);
    lv_obj_align(cancel_btn, LV_ALIGN_BOTTOM_RIGHT, -16, -16);
    lv_obj_set_style_bg_color(cancel_btn, lv_color_hex(COL_OFF), 0);
    lv_obj_set_style_radius(cancel_btn, 8, 0);
    lv_obj_add_event_cb(cancel_btn, picker_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cancel_lbl = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_lbl, "Cancel");
    lv_obj_set_style_text_font(cancel_lbl, &lv_font_montserrat_16, 0);
    lv_obj_center(cancel_lbl);
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

static void on_brightness_change(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    wled_brightness  = (uint8_t)lv_slider_get_value(slider);
    lv_label_set_text_fmt(ui_brightness_label, "%d", wled_brightness);
    wled_cmd_set_brightness(wled_brightness);
}

static void on_color_tap(lv_event_t *e)
{
    open_color_picker();
}

static void on_preset_change(lv_event_t *e)
{
    lv_obj_t *dd = lv_event_get_target(e);
    int idx = (int)lv_dropdown_get_selected(dd);
    int wled_id = wled_presets_get_id(idx);
    if (wled_id > 0) {
        wled_preset_idx = idx;
        ESP_LOGI(TAG, "Preset selected: idx=%d id=%d", idx, wled_id);
        wled_cmd_apply_preset(wled_id);
    }
}

// ── Build ─────────────────────────────────────────────────────────────────────

void ui_build(void)
{
    // Dark theme, blue primary, light-blue secondary
    lv_theme_t *theme = lv_theme_default_init(
        lv_disp_get_default(),
        lv_palette_main(LV_PALETTE_BLUE),
        lv_palette_main(LV_PALETTE_LIGHT_BLUE),
        true,
        &lv_font_montserrat_16
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
    lv_obj_set_style_bg_color(ui_brightness_slider,
        lv_color_hex(COL_ACCENT2), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(ui_brightness_slider,
        lv_color_hex(0xFFFFFF), LV_PART_KNOB);
    lv_obj_set_style_pad_all(ui_brightness_slider, 8, LV_PART_KNOB);
    lv_obj_add_event_cb(ui_brightness_slider, on_brightness_change,
        LV_EVENT_VALUE_CHANGED, NULL);

    ui_brightness_label = lv_label_create(scr);
    lv_obj_set_style_text_font(ui_brightness_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(ui_brightness_label, lv_color_hex(COL_ACCENT2), 0);
    lv_obj_align_to(ui_brightness_label, ui_brightness_slider,
        LV_ALIGN_OUT_RIGHT_MID, 10, 0);
    lv_label_set_text_fmt(ui_brightness_label, "%d", wled_brightness);

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

    // Style the dropdown to match the dark theme
    lv_obj_set_style_bg_color(ui_preset_dropdown, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_border_color(ui_preset_dropdown, lv_color_hex(COL_BORDER), 0);
    lv_obj_set_style_border_width(ui_preset_dropdown, 1, 0);
    lv_obj_set_style_text_color(ui_preset_dropdown, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_font(ui_preset_dropdown, &lv_font_montserrat_14, 0);
    lv_obj_set_style_radius(ui_preset_dropdown, 8, 0);
    lv_obj_set_style_pad_hor(ui_preset_dropdown, 12, 0);

    // Style the dropdown list (the list that appears when opened)
    lv_obj_t *list = lv_dropdown_get_list(ui_preset_dropdown);
    lv_obj_set_style_bg_color(list, lv_color_hex(COL_SURFACE), 0);
    lv_obj_set_style_border_color(list, lv_color_hex(COL_BORDER), 0);
    lv_obj_set_style_text_color(list, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_font(list, &lv_font_montserrat_14, 0);
    lv_obj_set_style_bg_color(list, lv_color_hex(COL_ACCENT),
                               LV_PART_SELECTED | LV_STATE_CHECKED);

    lv_obj_add_event_cb(ui_preset_dropdown, on_preset_change, LV_EVENT_VALUE_CHANGED, NULL);

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
        lv_label_set_text_fmt(ui_brightness_label, "%d", bri);
}

void ui_set_color(uint32_t rgb)
{
    wled_color = rgb;
    if (ui_color_preview)
        lv_obj_set_style_bg_color(ui_color_preview, lv_color_hex(rgb), 0);
    // Keep colorwheel in sync if picker is open
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
    case WIFI_STATE_CONNECTED:
        color = lv_palette_main(LV_PALETTE_GREEN);   break;
    case WIFI_STATE_CONNECTING:
        color = lv_palette_main(LV_PALETTE_YELLOW);  break;
    case WIFI_STATE_DISCONNECTED:
    default:
        color = lv_color_hex(0x555555);              break;
    }
    lv_obj_set_style_bg_color(s_wifi_dot, color, 0);
}

void ui_set_effect(int fx_index)
{
    // Effect index is kept for poll compatibility.
    // The fx_label was removed in Stage 10 — the preset dropdown
    // replaced the effect row entirely.  This is a no-op stub.
    (void)fx_index;
}

void ui_set_preset(int display_index, int wled_id)
{
    if (!ui_preset_dropdown) return;
    if (display_index < 0) return;  // preset not in list, leave dropdown alone

    // Suppress the VALUE_CHANGED event while we set it programmatically
    // to avoid firing on_preset_change and re-posting the same command.
    lv_obj_remove_event_cb(ui_preset_dropdown, on_preset_change);
    lv_dropdown_set_selected(ui_preset_dropdown, (uint16_t)display_index);
    lv_obj_add_event_cb(ui_preset_dropdown, on_preset_change, LV_EVENT_VALUE_CHANGED, NULL);

    wled_preset_idx = display_index;
}

void ui_presets_loaded(void)
{
    if (!ui_preset_dropdown) return;

    // Build the options string from the preset registry
    static char opts[WLED_PRESETS_MAX * (WLED_PRESET_NAME_LEN + 1)];
    int n = wled_presets_build_options(opts, sizeof(opts));

    if (n == 0) {
        lv_dropdown_set_options(ui_preset_dropdown, "No presets found");
        return;
    }

    lv_dropdown_set_options(ui_preset_dropdown, opts);
    ESP_LOGI(TAG, "Preset dropdown populated (%d entries)", n);
}