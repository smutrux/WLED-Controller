/**
 * wled_poll.c — Periodic WLED state polling task
 *
 * Uses cJSON (bundled with ESP-IDF) to parse the /json/state response.
 * All UI pushes go through lvgl_lock() / lvgl_unlock() so the LVGL task
 * never races with the poll task on widget state.
 */

#include "poll/wled_poll.h"
#include "http/wled_http.h"
#include "cmd/wled_cmd.h"
#include "devices/wled_devices.h"
#include "wifi/wifi.h"
#include "ui.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "presets/wled_presets.h"
#include <string.h>

static const char *TAG = "wled_poll";

// ── Tuning ─────────────────────────────────────────────────────────────────────
// How long to suppress UI updates after a command is sent (ms).
// Prevents the poll from flickering the UI back to old state while the WLED
// device is still processing the command.
#define WLED_POLL_CMD_HOLD_MS    1500

// Response buffer — /json/state is typically 300–600 bytes for a simple setup
#define POLL_BUF_SIZE            2048

// ── State ──────────────────────────────────────────────────────────────────────
static volatile bool s_paused = false;

// ── JSON parsing ───────────────────────────────────────────────────────────────

typedef struct {
    bool     on;
    uint8_t  bri;
    uint8_t  r, g, b;      // seg[0].col[0]
    int      fx;           // effect index
    int      ps;           // active preset id (-1 if none)
    bool     valid;        // false if parsing failed
} wled_state_t;

static wled_state_t parse_state(const char *json)
{
    wled_state_t result = { .valid = false };

    cJSON *root = cJSON_Parse(json);
    if (!root) {
        ESP_LOGW(TAG, "cJSON_Parse failed");
        return result;
    }

    // Top-level: "on" (bool), "bri" (int)
    cJSON *on_item  = cJSON_GetObjectItem(root, "on");
    cJSON *bri_item = cJSON_GetObjectItem(root, "bri");

    if (!cJSON_IsBool(on_item) || !cJSON_IsNumber(bri_item)) {
        ESP_LOGW(TAG, "Missing required fields in /json/state");
        cJSON_Delete(root);
        return result;
    }

    result.on  = cJSON_IsTrue(on_item);
    result.bri = (uint8_t)bri_item->valueint;

    // Default colour/fx in case segment parsing fails
    result.r = 255; result.g = 102; result.b = 0;
    result.fx = 0;
    result.ps = -1;

    // Top-level preset id
    cJSON *ps_item = cJSON_GetObjectItem(root, "ps");
    if (cJSON_IsNumber(ps_item)) {
        result.ps = ps_item->valueint;
    }

    // seg[0] → col[0] → [R, G, B], fx
    cJSON *seg_arr = cJSON_GetObjectItem(root, "seg");
    if (cJSON_IsArray(seg_arr) && cJSON_GetArraySize(seg_arr) > 0) {
        cJSON *seg0 = cJSON_GetArrayItem(seg_arr, 0);

        // Colour: col is [[R,G,B], [R,G,B], [R,G,B]]
        cJSON *col_arr = cJSON_GetObjectItem(seg0, "col");
        if (cJSON_IsArray(col_arr) && cJSON_GetArraySize(col_arr) > 0) {
            cJSON *col0 = cJSON_GetArrayItem(col_arr, 0);
            if (cJSON_IsArray(col0) && cJSON_GetArraySize(col0) >= 3) {
                result.r = (uint8_t)cJSON_GetArrayItem(col0, 0)->valueint;
                result.g = (uint8_t)cJSON_GetArrayItem(col0, 1)->valueint;
                result.b = (uint8_t)cJSON_GetArrayItem(col0, 2)->valueint;
            }
        }

        // Effect index
        cJSON *fx_item = cJSON_GetObjectItem(seg0, "fx");
        if (cJSON_IsNumber(fx_item)) {
            result.fx = fx_item->valueint;
        }
    }

    result.valid = true;
    cJSON_Delete(root);
    return result;
}

// ── UI push ────────────────────────────────────────────────────────────────────

static void push_to_ui(const wled_state_t *s)
{
    uint32_t rgb = ((uint32_t)s->r << 16) | ((uint32_t)s->g << 8) | s->b;

    // Find display index for the active preset (-1 if not in list)
    int preset_idx = wled_presets_find_by_id(s->ps);

    lvgl_lock();
    ui_set_power(s->on);
    ui_set_brightness(s->bri);
    ui_set_color(rgb);
    ui_set_preset(preset_idx, s->ps);
    lvgl_unlock();
}

// ── Poll task ──────────────────────────────────────────────────────────────────

static void poll_task(void *arg)
{
    ESP_LOGI(TAG, "Poll task started (interval=%d ms, hold=%d ms)",
             CONFIG_WLED_POLL_INTERVAL_MS, WLED_POLL_CMD_HOLD_MS);

    static char resp_buf[POLL_BUF_SIZE];

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(CONFIG_WLED_POLL_INTERVAL_MS));

        // Skip if WiFi is down
        if (wifi_get_state() != WIFI_STATE_CONNECTED) {
            ESP_LOGD(TAG, "WiFi not connected, skipping poll");
            continue;
        }

        // Skip if paused (e.g. user actively dragging a slider)
        if (s_paused) {
            ESP_LOGD(TAG, "Paused, skipping poll");
            continue;
        }

        // Skip UI update if a command was sent recently (avoid flicker)
        bool hold = wled_cmd_recently_sent(WLED_POLL_CMD_HOLD_MS);

        // Determine which IP to poll
        int sel = wled_devices_get_selected();
        int poll_idx = (sel == WLED_TARGET_ALL) ? 0 : sel;

        const wled_device_t *dev = wled_devices_get(poll_idx);
        if (!dev) {
            ESP_LOGW(TAG, "No device at index %d, skipping poll", poll_idx);
            continue;
        }

        // Fetch state
        esp_err_t ret = wled_http_get_state(dev->ip, resp_buf, sizeof(resp_buf));
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "[%s] Poll failed", dev->ip);
            // Update status label to show reachability issue
            lvgl_lock();
            ui_set_status("Status: Unreachable", dev->name);
            lvgl_unlock();
            continue;
        }

        // Parse
        wled_state_t state = parse_state(resp_buf);
        if (!state.valid) {
            ESP_LOGW(TAG, "[%s] Failed to parse /json/state", dev->ip);
            continue;
        }

        ESP_LOGD(TAG, "[%s] on=%d bri=%d rgb=#%02X%02X%02X fx=%d",
                 dev->ip, state.on, state.bri,
                 state.r, state.g, state.b, state.fx);

        // Push to UI unless we're in the command hold window
        if (!hold) {
            push_to_ui(&state);

            // Update the status card with preset name, colour, brightness
            const char *preset_name = "No Preset Loaded";
            int pidx = wled_presets_find_by_id(state.ps);
            if (pidx >= 0) {
                const wled_preset_t *p = wled_presets_get(pidx);
                if (p) preset_name = p->name;
            }
            char line1[48], line2[48];
            snprintf(line1, sizeof(line1), "%s  |  %s",
                     state.on ? "ON" : "OFF", preset_name);
            snprintf(line2, sizeof(line2), "Bri: %-3.0f%%  #%02X%02X%02X",
                     state.bri/2.55, state.r, state.g, state.b);
            lvgl_lock();
            ui_set_status(line1, line2);
            lvgl_unlock();
        } else {
            ESP_LOGD(TAG, "In command hold window, skipping UI update");
        }
    }
}

// ── Public API ─────────────────────────────────────────────────────────────────

esp_err_t wled_poll_init(void)
{
    if (wled_devices_count() == 0) {
        ESP_LOGW(TAG, "No devices registered — poll task will wait");
    }

    BaseType_t ok = xTaskCreatePinnedToCore(
        poll_task,
        "wled_poll",
        8192,   // cJSON parsing needs stack headroom
        NULL,
        2,      // priority 2: below cmd (3), encoder (4), LVGL (5)
        NULL,
        0       // core 0 — same as WiFi and cmd task
    );

    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create poll task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Poll task started (%d ms interval)", CONFIG_WLED_POLL_INTERVAL_MS);
    return ESP_OK;
}

void wled_poll_set_paused(bool paused)
{
    s_paused = paused;
}