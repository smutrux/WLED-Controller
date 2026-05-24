/**
 * wled_http.c — HTTP client for the WLED JSON API
 *
 * Uses esp_http_client (non-streaming).  Each call opens a connection,
 * performs the request, reads the response, and closes.  This is slightly
 * less efficient than keep-alive but is simpler to reason about for a
 * controller that sends commands at human speed.
 *
 * WLED JSON API used here:
 *   GET  /json/state  → {"on":true,"bri":128,"seg":[...],...}
 *   POST /json        → body: {"bri":200}  (partial updates are fine)
 *
 * Response parsing uses a minimal hand-rolled scanner rather than pulling in
 * cJSON, because we only need two or three fields for the status display.
 * cJSON is added in stage 9 when full state sync is implemented.
 */

#include "http/wled_http.h"
#include "devices/wled_devices.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_check.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "wled_http";

// ── Tuning ────────────────────────────────────────────────────────────────────
#define HTTP_TIMEOUT_MS      5000   // per-request timeout
#define RESP_BUF_SIZE        1024   // max response body we'll read

// ── Internal response collector ───────────────────────────────────────────────

typedef struct {
    char  *buf;
    size_t buf_len;
    size_t written;
    int    http_status;
} resp_ctx_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    resp_ctx_t *ctx = (resp_ctx_t *)evt->user_data;

    switch (evt->event_id) {

    case HTTP_EVENT_ON_DATA:
        if (ctx && evt->data_len > 0) {
            size_t space = ctx->buf_len - ctx->written - 1; // reserve for '\0'
            size_t copy  = (size_t)evt->data_len < space
                           ? (size_t)evt->data_len : space;
            if (copy > 0) {
                memcpy(ctx->buf + ctx->written, evt->data, copy);
                ctx->written += copy;
                ctx->buf[ctx->written] = '\0';
            }
        }
        break;

    case HTTP_EVENT_ON_FINISH:
        break;

    case HTTP_EVENT_ERROR:
        ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
        break;

    default:
        break;
    }
    return ESP_OK;
}

// ── URL builder ───────────────────────────────────────────────────────────────

// Writes "http://<ip>/json/state" or "http://<ip>/json" into out.
static void build_url(char *out, size_t out_len, const char *ip, bool state_endpoint)
{
    snprintf(out, out_len, "http://%s/json%s", ip,
             state_endpoint ? "/state" : "");
}

// ── Minimal JSON field extractor ──────────────────────────────────────────────
// Finds "key":value in a flat JSON string.  Returns the value as a
// null-terminated string in out_val (max out_len bytes).
// Works for bool and number values; not for nested objects.
static bool json_get_field(const char *json, const char *key,
                            char *out_val, size_t out_len)
{
    char search[48];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p) return false;

    p += strlen(search);
    while (*p == ' ') p++;     // skip whitespace after colon

    size_t i = 0;
    // Copy until comma, brace, bracket, or end of string
    while (*p && *p != ',' && *p != '}' && *p != ']' && i < out_len - 1) {
        out_val[i++] = *p++;
    }
    out_val[i] = '\0';
    return i > 0;
}

// ── Public API ────────────────────────────────────────────────────────────────

esp_err_t wled_http_get_state(const char *ip, char *buf, size_t buf_len)
{
    if (!ip || !buf || buf_len == 0) return ESP_ERR_INVALID_ARG;

    char url[80];
    build_url(url, sizeof(url), ip, true);
    buf[0] = '\0';

    resp_ctx_t ctx = { .buf = buf, .buf_len = buf_len, .written = 0 };

    esp_http_client_config_t cfg = {
        .url            = url,
        .method         = HTTP_METHOD_GET,
        .timeout_ms     = HTTP_TIMEOUT_MS,
        .event_handler  = http_event_handler,
        .user_data      = &ctx,
        .keep_alive_enable = false,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "[%s] Failed to init HTTP client", ip);
        return ESP_FAIL;
    }

    esp_err_t ret = esp_http_client_perform(client);
    int status    = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "[%s] GET failed: %s", ip, esp_err_to_name(ret));
        return ESP_FAIL;
    }
    if (status != 200) {
        ESP_LOGW(TAG, "[%s] GET returned HTTP %d", ip, status);
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "[%s] GET OK (%zu bytes)", ip, ctx.written);
    return ESP_OK;
}

esp_err_t wled_http_post(const char *ip, const char *json_body)
{
    if (!ip || !json_body) return ESP_ERR_INVALID_ARG;

    char url[80];
    build_url(url, sizeof(url), ip, false);

    // Scratch buffer — POST responses are tiny ("OK" or empty)
    char resp_buf[64] = {0};
    resp_ctx_t ctx = { .buf = resp_buf, .buf_len = sizeof(resp_buf), .written = 0 };

    esp_http_client_config_t cfg = {
        .url            = url,
        .method         = HTTP_METHOD_POST,
        .timeout_ms     = HTTP_TIMEOUT_MS,
        .event_handler  = http_event_handler,
        .user_data      = &ctx,
        .keep_alive_enable = false,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "[%s] Failed to init HTTP client", ip);
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, json_body, (int)strlen(json_body));

    esp_err_t ret = esp_http_client_perform(client);
    int status    = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "[%s] POST failed: %s", ip, esp_err_to_name(ret));
        return ESP_FAIL;
    }
    if (status != 200 && status != 204) {
        ESP_LOGW(TAG, "[%s] POST returned HTTP %d", ip, status);
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "[%s] POST OK", ip);
    return ESP_OK;
}

esp_err_t wled_http_broadcast_post(const char *json_body)
{
    const char *ips[WLED_MAX_DEVICES];
    int n = wled_devices_get_targets(ips);

    if (n == 0) {
        ESP_LOGW(TAG, "broadcast_post: no target devices");
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t overall = ESP_OK;
    for (int i = 0; i < n; i++) {
        esp_err_t ret = wled_http_post(ips[i], json_body);
        if (ret != ESP_OK) {
            overall = ESP_FAIL;
            // Continue — try remaining devices even if one fails
        }
    }
    return overall;
}

void wled_http_test_all(void)
{
    int count = wled_devices_count();
    ESP_LOGI(TAG, "=== Stage 6 HTTP reachability test (%d devices) ===", count);

    if (count == 0) {
        ESP_LOGW(TAG, "No devices registered — add devices in main.c first");
        return;
    }

    char resp[RESP_BUF_SIZE];
    char val[32];
    int  pass = 0, fail = 0;

    for (int i = 0; i < count; i++) {
        const wled_device_t *dev = wled_devices_get(i);
        if (!dev) continue;

        ESP_LOGI(TAG, "--- [%d] \"%s\" @ %s ---", i, dev->name, dev->ip);

        esp_err_t ret = wled_http_get_state(dev->ip, resp, sizeof(resp));

        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "  FAIL — could not reach device");
            ESP_LOGW(TAG, "  Check: Is %s reachable on your network?", dev->ip);
            ESP_LOGW(TAG, "  Check: Is WLED running on that device?");
            fail++;
            continue;
        }

        // Parse a handful of fields for a human-readable summary
        bool got_on  = json_get_field(resp, "on",  val, sizeof(val));
        bool got_bri = json_get_field(resp, "bri", val, sizeof(val));

        char on_str[8]  = "?";
        char bri_str[8] = "?";

        if (got_on)  json_get_field(resp, "on",  on_str,  sizeof(on_str));
        if (got_bri) json_get_field(resp, "bri", bri_str, sizeof(bri_str));

        // Re-fetch cleanly for display
        json_get_field(resp, "on",  on_str,  sizeof(on_str));
        json_get_field(resp, "bri", bri_str, sizeof(bri_str));

        char mainseg[8] = "?";
        json_get_field(resp, "mainseg", mainseg, sizeof(mainseg));

        ESP_LOGI(TAG, "  OK  on=%s  bri=%s  mainseg=%s", on_str, bri_str, mainseg);
        ESP_LOGD(TAG, "  Raw JSON: %.200s%s", resp,
                 strlen(resp) > 200 ? "... (truncated)" : "");
        pass++;
    }

    ESP_LOGI(TAG, "=== Test complete: %d/%d reachable ===", pass, count);

    if (fail > 0) {
        ESP_LOGW(TAG, "%d device(s) unreachable. Common causes:", fail);
        ESP_LOGW(TAG, "  - Wrong IP address in the device registry");
        ESP_LOGW(TAG, "  - WLED device is off or on a different network segment");
        ESP_LOGW(TAG, "  - Controller not yet fully connected to WiFi");
    }
}