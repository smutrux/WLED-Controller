/**
 * wifi.c — WiFi station driver with exponential backoff reconnection
 *
 * Backoff schedule (capped at WIFI_MAX_BACKOFF_S):
 *   attempt 0 → 1 s
 *   attempt 1 → 2 s
 *   attempt 2 → 4 s
 *   attempt 3 → 8 s
 *   attempt 4 → 16 s
 *   attempt 5 → 32 s
 *   attempt 6+ → 60 s  (WIFI_MAX_BACKOFF_S)
 *
 * After WIFI_MAX_RETRY_COUNT consecutive failures the driver stops retrying
 * and sets state to DISCONNECTED.  Set WIFI_MAX_RETRY_COUNT to 0 to retry
 * forever (useful for a battery device that may be out of range temporarily).
 */

#include "wifi/wifi.h"
#include "ui.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_timer.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "wled_wifi";  // distinct from the driver's own "wifi" tag

// ── Tuning ────────────────────────────────────────────────────────────────────
// 0 = retry forever (recommended for a portable device)
#define WIFI_MAX_RETRY_COUNT   0
#define WIFI_MAX_BACKOFF_S     60

// ── State ─────────────────────────────────────────────────────────────────────
static volatile wifi_conn_state_t s_state    = WIFI_STATE_DISCONNECTED;
static int                        s_retries  = 0;
static esp_timer_handle_t         s_retry_timer;
static esp_netif_t               *s_netif    = NULL;

// ── Helpers ───────────────────────────────────────────────────────────────────

/** Convert a wifi_conn_state_t enum to a readable string for logging.
 *  Never pass the raw enum to a %s format specifier — it is an integer,
 *  not a pointer, and will cause a LoadProhibited crash in vfprintf. */
static const char *wifi_state_str(wifi_conn_state_t state)
{
    switch (state) {
    case WIFI_STATE_CONNECTED:    return "CONNECTED";
    case WIFI_STATE_CONNECTING:   return "CONNECTING";
    case WIFI_STATE_DISCONNECTED: return "DISCONNECTED";
    default:                      return "UNKNOWN";
    }
}

/** Compute backoff delay in milliseconds for the current retry count. */
static uint32_t backoff_ms(void)
{
    // 1 << n gives 1, 2, 4, 8 … seconds; cap at WIFI_MAX_BACKOFF_S
    uint32_t secs = (1u << s_retries);
    if (secs > WIFI_MAX_BACKOFF_S) secs = WIFI_MAX_BACKOFF_S;
    return secs * 1000;
}

/** Push a connection state change to the WiFi driver and update the UI dot. */
static void set_state(wifi_conn_state_t new_state)
{
    ESP_LOGI(TAG, "WiFi state: %s -> %s",
             wifi_state_str(s_state), wifi_state_str(new_state));
    s_state = new_state;

    // Update UI from whatever task we're on — must hold LVGL mutex
    lvgl_lock();
    ui_set_wifi_status(new_state);
    lvgl_unlock();
}

// ── Retry timer ───────────────────────────────────────────────────────────────

static void retry_connect(void *arg)
{
    // NOTE: Do NOT call set_state() / lvgl_lock() here.
    // esp_timer callbacks share one task with the LVGL tick timer.
    // Blocking on lvgl_lock() here would freeze the LVGL clock entirely,
    // halting all input processing.  State is already CONNECTING — it was
    // set in the DISCONNECTED event handler before this timer was started.
    ESP_LOGI(TAG, "Retrying WiFi connection (attempt %d)…", s_retries + 1);
    esp_wifi_connect();
}

// ── Event handler ─────────────────────────────────────────────────────────────

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {

        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "STA started — connecting to \"%s\"",
                     CONFIG_WLED_WIFI_SSID);
            set_state(WIFI_STATE_CONNECTING);
            esp_wifi_connect();
            break;

        case WIFI_EVENT_STA_CONNECTED:
            // Connected at L2 — wait for IP before calling it done
            ESP_LOGI(TAG, "Associated with AP, waiting for IP…");
            break;

        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *ev =
                (wifi_event_sta_disconnected_t *)data;
            ESP_LOGW(TAG, "Disconnected (reason %d)", ev->reason);

            // Stop any pending retry timer so we don't double-fire
            esp_timer_stop(s_retry_timer);

            if (WIFI_MAX_RETRY_COUNT > 0 &&
                s_retries >= WIFI_MAX_RETRY_COUNT) {
                ESP_LOGE(TAG, "Max retries reached — giving up");
                set_state(WIFI_STATE_DISCONNECTED);
                break;
            }

            uint32_t delay = backoff_ms();
            ESP_LOGI(TAG, "Will retry in %lu ms (backoff attempt %d)",
                     (unsigned long)delay, s_retries);
            set_state(WIFI_STATE_CONNECTING);
            esp_timer_start_once(s_retry_timer, (uint64_t)delay * 1000ULL);
            s_retries++;
            break;
        }

        default:
            break;
        }

    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        s_retries = 0;   // reset backoff counter on successful connect
        set_state(WIFI_STATE_CONNECTED);

        // Disable WiFi power-save mode. The default WIFI_PS_MIN_MODEM sleeps
        // the radio between beacon intervals, which causes some routers to send
        // a deauth and reassociate — appearing as a spurious yellow flash on the
        // status dot. WIFI_PS_NONE keeps the radio on continuously. Current draw
        // increases ~20 mA but the connection stays stable and the dot stays green.
        esp_wifi_set_ps(WIFI_PS_NONE);
    }
}

// ── Public API ────────────────────────────────────────────────────────────────

esp_err_t wifi_init(void)
{
    // ── NVS (required by WiFi driver to store calibration data) ──────────────
    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // Partition was truncated or version changed — erase and retry once
        ESP_LOGW(TAG, "NVS partition issue (%d) — erasing", nvs_ret);
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs_flash_erase");
        nvs_ret = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(nvs_ret, TAG, "nvs_flash_init");

    // ── TCP/IP stack + default event loop ────────────────────────────────────
    ESP_RETURN_ON_ERROR(esp_netif_init(),               TAG, "netif_init");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event_loop");

    s_netif = esp_netif_create_default_wifi_sta();
    if (!s_netif) {
        ESP_LOGE(TAG, "Failed to create default WiFi STA netif");
        return ESP_FAIL;
    }

    // ── WiFi driver ───────────────────────────────────────────────────────────
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "esp_wifi_init");

    // Register for both WiFi and IP events
    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                            wifi_event_handler, NULL, NULL),
        TAG, "register WIFI_EVENT");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                            wifi_event_handler, NULL, NULL),
        TAG, "register IP_EVENT");

    // ── STA configuration (credentials from Kconfig) ─────────────────────────
    wifi_config_t wifi_cfg = {
        .sta = {
            .ssid     = CONFIG_WLED_WIFI_SSID,
            .password = CONFIG_WLED_WIFI_PASSWORD,
            // pmf_cfg: protected management frames — prefer if AP supports it
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable  = true,
                .required = false,
            },
        },
    };
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA),    TAG, "set_mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg),
                        TAG, "set_config");

    // ── Retry timer (one-shot, restarted on each disconnect) ─────────────────
    const esp_timer_create_args_t timer_cfg = {
        .callback              = retry_connect,
        .arg                   = NULL,
        .name                  = "wifi_retry",
        .skip_unhandled_events = true,
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer_cfg, &s_retry_timer),
                        TAG, "esp_timer_create");

    // ── Start — WIFI_EVENT_STA_START will trigger the first connect ───────────
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start");

    // Silence the WiFi driver's verbose internal logging. The driver uses several
    // tag names; all need to be suppressed or they flood the console during
    // reconnect loops and can starve other tasks (especially on USB CDC console).
    esp_log_level_set("wifi",          ESP_LOG_WARN);
    esp_log_level_set("wifi_init",     ESP_LOG_WARN);
    esp_log_level_set("phy_init",      ESP_LOG_WARN);
    esp_log_level_set("phy",           ESP_LOG_WARN);
    esp_log_level_set("esp_netif_lwip",ESP_LOG_WARN);

    ESP_LOGI(TAG, "WiFi init done — connecting to \"%s\"",
             CONFIG_WLED_WIFI_SSID);
    return ESP_OK;
}

wifi_conn_state_t wifi_get_state(void)
{
    return s_state;
}