/**
 * wled_cmd.c — WLED command dispatch layer
 *
 * Threading model:
 *   UI callbacks (LVGL task, core 1) or encoder callbacks (core 0) call
 *   wled_cmd_set_power() / wled_cmd_set_brightness().
 *
 *   These functions write to a small shared state struct (guarded by a
 *   spinlock) and notify a dedicated wled_cmd_task via a FreeRTOS task
 *   notification.  The cmd_task (core 0) then calls wled_http_broadcast_post()
 *   which blocks on the network — completely off the LVGL render path.
 *
 *   Brightness debounce:
 *     wled_cmd_set_brightness() restarts a one-shot esp_timer each call.
 *     The timer callback sends the task notification after the gesture ends.
 *     Power commands bypass the timer and notify directly.
 *
 *   Why a separate task instead of calling HTTP from the esp_timer callback?
 *     esp_timer callbacks run on a shared task and must not block.
 *     HTTP calls block for up to HTTP_TIMEOUT_MS.  Doing HTTP in the timer
 *     callback would stall all other timers including the LVGL tick.
 */

#include "cmd/wled_cmd.h"
#include "http/wled_http.h"
#include "wifi/wifi.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "wled_cmd";

// ── Tuning ────────────────────────────────────────────────────────────────────
// How long to wait after the last brightness change before sending the POST.
// 200 ms feels instant to a human but collapses dozens of slider events into one.
#define WLED_CMD_BRIGHTNESS_DEBOUNCE_MS  200

// ── Command types ─────────────────────────────────────────────────────────────
typedef enum {
    CMD_POWER      = (1 << 0),
    CMD_BRIGHTNESS = (1 << 1),
} cmd_flag_t;

// ── Shared state (protected by s_lock) ────────────────────────────────────────
static portMUX_TYPE      s_lock      = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t s_pending   = 0;     // bitmask of cmd_flag_t
static volatile bool     s_power     = true;
static volatile uint8_t  s_brightness = 128;

// ── Task handle for notifications ─────────────────────────────────────────────
static TaskHandle_t s_cmd_task_handle = NULL;

// ── Debounce timer ────────────────────────────────────────────────────────────
static esp_timer_handle_t s_bri_timer;

static void bri_debounce_expired(void *arg)
{
    // Timer fired — brightness gesture is done, notify the cmd task.
    // Do NOT call HTTP here: esp_timer callbacks must not block.
    if (s_cmd_task_handle) {
        xTaskNotifyFromISR(s_cmd_task_handle, CMD_BRIGHTNESS,
                           eSetBits, NULL);
    }
}

// ── Command task ──────────────────────────────────────────────────────────────

static void wled_cmd_task(void *arg)
{
    ESP_LOGI(TAG, "Command task started on core %d", xPortGetCoreID());

    for (;;) {
        // Block until at least one command flag arrives
        uint32_t flags = 0;
        xTaskNotifyWait(0, UINT32_MAX, &flags, portMAX_DELAY);

        // Skip if WiFi isn't up — don't queue up stale commands
        if (wifi_get_state() != WIFI_STATE_CONNECTED) {
            ESP_LOGD(TAG, "WiFi not connected, dropping command (flags=0x%lx)",
                     (unsigned long)flags);
            continue;
        }

        // Snapshot the shared state under the spinlock
        bool    power;
        uint8_t brightness;
        portENTER_CRITICAL(&s_lock);
        power      = s_power;
        brightness = s_brightness;
        portEXIT_CRITICAL(&s_lock);

        // Handle power command
        if (flags & CMD_POWER) {
            char payload[32];
            snprintf(payload, sizeof(payload),
                     "{\"on\":%s}", power ? "true" : "false");
            ESP_LOGI(TAG, "POST power -> %s", power ? "ON" : "OFF");
            esp_err_t ret = wled_http_broadcast_post(payload);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Power POST failed (will retry on next command)");
            }
        }

        // Handle brightness command
        if (flags & CMD_BRIGHTNESS) {
            char payload[32];
            snprintf(payload, sizeof(payload), "{\"bri\":%d}", brightness);
            ESP_LOGI(TAG, "POST brightness -> %d", brightness);
            esp_err_t ret = wled_http_broadcast_post(payload);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Brightness POST failed");
            }
        }
    }
}

// ── Public API ────────────────────────────────────────────────────────────────

esp_err_t wled_cmd_init(void)
{
    // Debounce timer for brightness — one-shot, restarted on each slider event
    const esp_timer_create_args_t timer_cfg = {
        .callback              = bri_debounce_expired,
        .arg                   = NULL,
        .name                  = "bri_debounce",
        .skip_unhandled_events = true,
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer_cfg, &s_bri_timer),
                        TAG, "esp_timer_create");

    // Dispatch task — pinned to core 0, lower priority than LVGL (5)
    BaseType_t ok = xTaskCreatePinnedToCore(
        wled_cmd_task,
        "wled_cmd",
        4096,
        NULL,
        3,      // priority 3: below LVGL (5), below encoder (4), above idle
        &s_cmd_task_handle,
        0       // core 0 — same as WiFi driver
    );
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

    // Power commands are immediate — no debounce
    if (s_cmd_task_handle) {
        xTaskNotify(s_cmd_task_handle, CMD_POWER, eSetBits);
    }
}

void wled_cmd_set_brightness(uint8_t bri)
{
    portENTER_CRITICAL(&s_lock);
    s_brightness = bri;
    s_pending   |= CMD_BRIGHTNESS;
    portEXIT_CRITICAL(&s_lock);

    // (Re)start the debounce timer — resets the countdown on every call
    esp_timer_stop(s_bri_timer);   // no-op if not running
    esp_timer_start_once(s_bri_timer,
                         WLED_CMD_BRIGHTNESS_DEBOUNCE_MS * 1000ULL);
}