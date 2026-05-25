/**
 * wled_cmd.c — WLED command dispatch layer
 */

#include "cmd/wled_cmd.h"
#include "http/wled_http.h"
#include "wifi/wifi.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "wled_cmd";

#define WLED_CMD_BRIGHTNESS_DEBOUNCE_MS  200

typedef enum {
    CMD_POWER      = (1 << 0),
    CMD_BRIGHTNESS = (1 << 1),
    CMD_COLOR      = (1 << 2),
    CMD_PRESET     = (1 << 3),
} cmd_flag_t;

// ── Shared state ──────────────────────────────────────────────────────────────
static portMUX_TYPE      s_lock        = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t s_pending     = 0;
static volatile bool     s_power       = true;
static volatile uint8_t  s_brightness  = 128;
static volatile uint32_t s_color       = 0xFF6600;
static volatile int      s_preset_id   = -1;

static volatile int64_t  s_last_sent_us = 0;
static TaskHandle_t      s_cmd_task_handle = NULL;
static esp_timer_handle_t s_bri_timer;

// ── Debounce timer ────────────────────────────────────────────────────────────

static void bri_debounce_expired(void *arg)
{
    if (s_cmd_task_handle) {
        xTaskNotifyFromISR(s_cmd_task_handle, CMD_BRIGHTNESS, eSetBits, NULL);
    }
}

// ── Command task ──────────────────────────────────────────────────────────────

static void wled_cmd_task(void *arg)
{
    ESP_LOGI(TAG, "Command task started on core %d", xPortGetCoreID());

    for (;;) {
        uint32_t flags = 0;
        xTaskNotifyWait(0, UINT32_MAX, &flags, portMAX_DELAY);

        if (wifi_get_state() != WIFI_STATE_CONNECTED) {
            ESP_LOGD(TAG, "WiFi not connected, dropping command");
            continue;
        }

        // Snapshot shared state
        bool     power;
        uint8_t  brightness;
        uint32_t color;
        int      preset_id;
        portENTER_CRITICAL(&s_lock);
        power      = s_power;
        brightness = s_brightness;
        color      = s_color;
        preset_id  = s_preset_id;
        portEXIT_CRITICAL(&s_lock);

        s_last_sent_us = esp_timer_get_time();

        // Power
        if (flags & CMD_POWER) {
            char payload[32];
            snprintf(payload, sizeof(payload), "{\"on\":%s}", power ? "true" : "false");
            ESP_LOGI(TAG, "POST power -> %s", power ? "ON" : "OFF");
            esp_err_t ret = wled_http_broadcast_post(payload);
            if (ret != ESP_OK) ESP_LOGW(TAG, "Power POST failed");
        }

        // Brightness
        if (flags & CMD_BRIGHTNESS) {
            char payload[32];
            snprintf(payload, sizeof(payload), "{\"bri\":%d}", brightness);
            ESP_LOGI(TAG, "POST brightness -> %d", brightness);
            wled_http_broadcast_post(payload);
        }

        // Colour — sends to first segment primary colour
        if (flags & CMD_COLOR) {
            char payload[64];
            snprintf(payload, sizeof(payload),
                     "{\"seg\":[{\"col\":[[%lu,%lu,%lu]]}]}",
                     (unsigned long)((color >> 16) & 0xFF),
                     (unsigned long)((color >>  8) & 0xFF),
                     (unsigned long)( color        & 0xFF));
            ESP_LOGI(TAG, "POST color -> #%06lX", (unsigned long)color);
            wled_http_broadcast_post(payload);
        }

        // Preset
        if (flags & CMD_PRESET) {
            char payload[24];
            snprintf(payload, sizeof(payload), "{\"ps\":%d}", preset_id);
            ESP_LOGI(TAG, "POST preset -> id=%d", preset_id);
            wled_http_broadcast_post(payload);
        }
    }
}

// ── Public API ────────────────────────────────────────────────────────────────

esp_err_t wled_cmd_init(void)
{
    const esp_timer_create_args_t timer_cfg = {
        .callback              = bri_debounce_expired,
        .arg                   = NULL,
        .name                  = "bri_debounce",
        .skip_unhandled_events = true,
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer_cfg, &s_bri_timer),
                        TAG, "esp_timer_create");

    BaseType_t ok = xTaskCreatePinnedToCore(
        wled_cmd_task, "wled_cmd", 4096, NULL, 3, &s_cmd_task_handle, 0);
    if (ok != pdPASS || !s_cmd_task_handle) {
        ESP_LOGE(TAG, "Failed to create wled_cmd task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Command layer ready (debounce=%d ms)",
             WLED_CMD_BRIGHTNESS_DEBOUNCE_MS);
    return ESP_OK;
}

void wled_cmd_set_power(bool on)
{
    portENTER_CRITICAL(&s_lock);
    s_power    = on;
    s_pending |= CMD_POWER;
    portEXIT_CRITICAL(&s_lock);
    if (s_cmd_task_handle) xTaskNotify(s_cmd_task_handle, CMD_POWER, eSetBits);
}

void wled_cmd_set_brightness(uint8_t bri)
{
    portENTER_CRITICAL(&s_lock);
    s_brightness = bri;
    s_pending   |= CMD_BRIGHTNESS;
    portEXIT_CRITICAL(&s_lock);
    esp_timer_stop(s_bri_timer);
    esp_timer_start_once(s_bri_timer, WLED_CMD_BRIGHTNESS_DEBOUNCE_MS * 1000ULL);
}

void wled_cmd_set_color(uint32_t rgb)
{
    portENTER_CRITICAL(&s_lock);
    s_color    = rgb;
    s_pending |= CMD_COLOR;
    portEXIT_CRITICAL(&s_lock);
    if (s_cmd_task_handle) xTaskNotify(s_cmd_task_handle, CMD_COLOR, eSetBits);
}

void wled_cmd_apply_preset(int preset_id)
{
    portENTER_CRITICAL(&s_lock);
    s_preset_id = preset_id;
    s_pending  |= CMD_PRESET;
    portEXIT_CRITICAL(&s_lock);
    if (s_cmd_task_handle) xTaskNotify(s_cmd_task_handle, CMD_PRESET, eSetBits);
}

bool wled_cmd_recently_sent(uint32_t within_ms)
{
    if (s_last_sent_us == 0) return false;
    int64_t elapsed_ms = (esp_timer_get_time() - s_last_sent_us) / 1000LL;
    return elapsed_ms < (int64_t)within_ms;
}