/**
 * ui.c — Initial LVGL UI for WLED Controller
 *
 * Builds a dark controller interface:
 *   - Header bar with connection dot + power toggle
 *   - Status card (updated later from JSON API polling)
 *   - Brightness slider with live value
 *   - Color preview swatch (tap → picker, later stage)
 *   - Effect row placeholder
 */

#include "ui.h"
#include "board/board.h"
#include "esp_log.h"
#include "lvgl.h"
#include <stdio.h>

static const char *TAG = "ui";

// Widget handles exposed for state updates
lv_obj_t *ui_status_label     = NULL;
lv_obj_t *ui_brightness_slider = NULL;
lv_obj_t *ui_brightness_label = NULL;
lv_obj_t *ui_color_preview    = NULL;
lv_obj_t *ui_power_btn        = NULL;
lv_obj_t *ui_power_btn_label  = NULL;
static lv_obj_t *s_wifi_dot = NULL;  // connection status dot in header

// Simulated WLED state — replaced by real poll data in a later stage
static bool    wled_on         = true;
static uint8_t wled_brightness = 128;
static uint32_t wled_color     = 0xFF6600;

// ── Event handlers ───────────────────────────────────────────────────────────

static void on_power_click(lv_event_t *e)
{
    wled_on = !wled_on;
    lv_label_set_text(ui_power_btn_label, wled_on ? "ON" : "OFF");
    lv_obj_set_style_bg_color(ui_power_btn,
        wled_on ? lv_palette_main(LV_PALETTE_ORANGE) : lv_color_hex(0x444444), 0);
    ESP_LOGI(TAG, "Power toggled -> %s", wled_on ? "ON" : "OFF");
    // TODO Stage 7: POST /json/state {"on": true/false}
}

static void on_brightness_change(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    wled_brightness  = (uint8_t)lv_slider_get_value(slider);
    lv_label_set_text_fmt(ui_brightness_label, "%d", wled_brightness);
    ESP_LOGI(TAG, "Brightness -> %d", wled_brightness);
    // TODO Stage 8: debounced POST /json/state {"bri": wled_brightness}
}

static void on_color_tap(lv_event_t *e)
{
    ESP_LOGI(TAG, "Color preview tapped — picker not yet implemented");
    // TODO Stage 9 (Final UI): open lv_colorwheel modal
}

// ── Build ────────────────────────────────────────────────────────────────────

void ui_build(void)
{
    // Apply dark theme with orange accent
    lv_theme_t *theme = lv_theme_default_init(
        lv_disp_get_default(),
        lv_palette_main(LV_PALETTE_ORANGE),
        lv_palette_main(LV_PALETTE_CYAN),
        true,   // dark
        &lv_font_montserrat_16
    );
    lv_disp_set_theme(lv_disp_get_default(), theme);

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0D0D0D), 0);

    // ── Header ───────────────────────────────────────────────────────────────
    lv_obj_t *header = lv_obj_create(scr);
    lv_obj_set_size(header, LCD_H_RES, 52);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_set_style_pad_hor(header, 10, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    s_wifi_dot = lv_obj_create(header);
    lv_obj_set_size(s_wifi_dot, 10, 10);
    lv_obj_align(s_wifi_dot, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_radius(s_wifi_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_wifi_dot, lv_color_hex(0x555555), 0); // grey until WiFi connects
    lv_obj_set_style_border_width(s_wifi_dot, 0, 0);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, "WLED Controller");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xEEEEEE), 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 18, 0);

    ui_power_btn = lv_btn_create(header);
    lv_obj_set_size(ui_power_btn, 56, 32);
    lv_obj_align(ui_power_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(ui_power_btn,
        wled_on ? lv_palette_main(LV_PALETTE_ORANGE) : lv_color_hex(0x444444), 0);
    lv_obj_set_style_radius(ui_power_btn, 6, 0);
    lv_obj_add_event_cb(ui_power_btn, on_power_click, LV_EVENT_CLICKED, NULL);

    ui_power_btn_label = lv_label_create(ui_power_btn);
    lv_label_set_text(ui_power_btn_label, wled_on ? "ON" : "OFF");
    lv_obj_set_style_text_font(ui_power_btn_label, &lv_font_montserrat_14, 0);
    lv_obj_center(ui_power_btn_label);

    // ── Status card ──────────────────────────────────────────────────────────
    lv_obj_t *card = lv_obj_create(scr);
    lv_obj_set_size(card, LCD_H_RES - 24, 72);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1E1E1E), 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x333333), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    ui_status_label = lv_label_create(card);
    lv_label_set_text(ui_status_label, "Status: Initializing...\nSegments: --");
    lv_obj_set_style_text_font(ui_status_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(ui_status_label, lv_color_hex(0xAAAAAA), 0);
    lv_obj_align(ui_status_label, LV_ALIGN_LEFT_MID, 0, 0);

    // ── Brightness ───────────────────────────────────────────────────────────
    lv_obj_t *bri_lbl = lv_label_create(scr);
    lv_label_set_text(bri_lbl, "Brightness");
    lv_obj_set_style_text_font(bri_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(bri_lbl, lv_color_hex(0xCCCCCC), 0);
    lv_obj_align(bri_lbl, LV_ALIGN_TOP_LEFT, 12, 150);

    ui_brightness_slider = lv_slider_create(scr);
    lv_slider_set_range(ui_brightness_slider, 0, 255);
    lv_slider_set_value(ui_brightness_slider, wled_brightness, LV_ANIM_OFF);
    lv_obj_set_size(ui_brightness_slider, LCD_H_RES - 80, 8);
    lv_obj_align(ui_brightness_slider, LV_ALIGN_TOP_LEFT, 12, 182);
    lv_obj_set_style_bg_color(ui_brightness_slider, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_bg_color(ui_brightness_slider,
        lv_palette_main(LV_PALETTE_ORANGE), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(ui_brightness_slider,
        lv_color_hex(0xFFFFFF), LV_PART_KNOB);
    lv_obj_set_style_pad_all(ui_brightness_slider, 8, LV_PART_KNOB);
    lv_obj_add_event_cb(ui_brightness_slider, on_brightness_change,
        LV_EVENT_VALUE_CHANGED, NULL);

    ui_brightness_label = lv_label_create(scr);
    lv_obj_set_style_text_font(ui_brightness_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(ui_brightness_label,
        lv_palette_main(LV_PALETTE_ORANGE), 0);
    lv_obj_align_to(ui_brightness_label, ui_brightness_slider,
        LV_ALIGN_OUT_RIGHT_MID, 10, 0);
    lv_label_set_text_fmt(ui_brightness_label, "%d", wled_brightness);

    // ── Color preview ────────────────────────────────────────────────────────
    lv_obj_t *col_heading = lv_label_create(scr);
    lv_label_set_text(col_heading, "Color");
    lv_obj_set_style_text_font(col_heading, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(col_heading, lv_color_hex(0xCCCCCC), 0);
    lv_obj_align(col_heading, LV_ALIGN_TOP_LEFT, 12, 222);

    ui_color_preview = lv_obj_create(scr);
    lv_obj_set_size(ui_color_preview, 64, 36);
    lv_obj_align(ui_color_preview, LV_ALIGN_TOP_LEFT, 12, 250);
    lv_obj_set_style_bg_color(ui_color_preview, lv_color_hex(wled_color), 0);
    lv_obj_set_style_border_color(ui_color_preview, lv_color_hex(0x555555), 0);
    lv_obj_set_style_border_width(ui_color_preview, 1, 0);
    lv_obj_set_style_radius(ui_color_preview, 6, 0);
    lv_obj_add_flag(ui_color_preview, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ui_color_preview, on_color_tap, LV_EVENT_CLICKED, NULL);

    lv_obj_t *col_hint = lv_label_create(scr);
    lv_label_set_text(col_hint, "Tap to change");
    lv_obj_set_style_text_font(col_hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(col_hint, lv_color_hex(0x666666), 0);
    lv_obj_align_to(col_hint, ui_color_preview, LV_ALIGN_OUT_RIGHT_MID, 10, 0);

    // ── Effect row ───────────────────────────────────────────────────────────
    lv_obj_t *fx_card = lv_obj_create(scr);
    lv_obj_set_size(fx_card, LCD_H_RES - 24, 52);
    lv_obj_align(fx_card, LV_ALIGN_TOP_MID, 0, 310);
    lv_obj_set_style_bg_color(fx_card, lv_color_hex(0x1E1E1E), 0);
    lv_obj_set_style_border_color(fx_card, lv_color_hex(0x333333), 0);
    lv_obj_set_style_border_width(fx_card, 1, 0);
    lv_obj_set_style_radius(fx_card, 10, 0);
    lv_obj_set_style_pad_hor(fx_card, 12, 0);
    lv_obj_clear_flag(fx_card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *fx_lbl = lv_label_create(fx_card);
    lv_label_set_text(fx_lbl, "Effect: Solid");
    lv_obj_set_style_text_font(fx_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(fx_lbl, lv_color_hex(0x888888), 0);
    lv_obj_align(fx_lbl, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *fx_arrow = lv_label_create(fx_card);
    lv_label_set_text(fx_arrow, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(fx_arrow, lv_color_hex(0x555555), 0);
    lv_obj_align(fx_arrow, LV_ALIGN_RIGHT_MID, 0, 0);

    ESP_LOGI(TAG, "UI built");
}

void ui_set_status(const char *line1, const char *line2)
{
    if (!ui_status_label) return;
    // lv_label supports \n for two lines
    char buf[80];
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
}

void ui_set_power(bool on)
{
    wled_on = on;
    if (ui_power_btn)
        lv_obj_set_style_bg_color(ui_power_btn,
            on ? lv_palette_main(LV_PALETTE_ORANGE) : lv_color_hex(0x444444), 0);
    if (ui_power_btn_label)
        lv_label_set_text(ui_power_btn_label, on ? "ON" : "OFF");
}

void ui_set_wifi_status(wifi_conn_state_t state)
{
    if (!s_wifi_dot) return;

    lv_color_t color;
    switch (state) {
    case WIFI_STATE_CONNECTED:
        // color = lv_palette_main(LV_PALETTE_GREEN);
        color = lv_color_hex(0x1010A0);
        break;
        case WIFI_STATE_CONNECTING:
        // color = lv_palette_main(LV_PALETTE_YELLOW);
        color = lv_color_hex(0xA010A0);
        break;
    case WIFI_STATE_DISCONNECTED:
    default:
        color = lv_color_hex(0x555555);
        break;
    }
    lv_obj_set_style_bg_color(s_wifi_dot, color, 0);
}
