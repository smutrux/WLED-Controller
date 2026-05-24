#pragma once
/**
 * wled_http.h — HTTP client for the WLED JSON API
 *
 * Provides two primitives:
 *
 *   wled_http_get_state(ip, buf, buf_len)
 *     GET /json/state from one device.  Writes the raw JSON into buf.
 *     Returns ESP_OK on HTTP 200, ESP_FAIL on any error.
 *
 *   wled_http_post(ip, json_body)
 *     POST /json with a JSON payload to one device.
 *     Returns ESP_OK on HTTP 200/204, ESP_FAIL on any error.
 *
 * Both are blocking and should be called from a task, not from an ISR or
 * the LVGL task.  Typical call site is a dedicated wled_task on core 0.
 *
 * The broadcast helpers iterate wled_devices_get_targets() internally so
 * callers don't need to manage the device list themselves:
 *
 *   wled_http_broadcast_post(json_body)
 *     POST to every target returned by the current device selection.
 *
 *   wled_http_test_all()
 *     GET /json/state from every registered device and log the result.
 *     Intended for stage 6 validation only.
 */

#include "esp_err.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * GET /json/state from a single WLED device.
 *
 * @param ip       IPv4 string, e.g. "192.168.1.10"
 * @param buf      Buffer to write the JSON response body into
 * @param buf_len  Size of buf (response is truncated if body exceeds this)
 * @return ESP_OK on success (HTTP 200), ESP_FAIL otherwise
 */
esp_err_t wled_http_get_state(const char *ip, char *buf, size_t buf_len);

/**
 * POST /json to a single WLED device.
 *
 * @param ip        IPv4 string
 * @param json_body Null-terminated JSON string, e.g. "{\"bri\":128}"
 * @return ESP_OK on HTTP 200/204, ESP_FAIL otherwise
 */
esp_err_t wled_http_post(const char *ip, const char *json_body);

/**
 * POST /json to all current target devices (respects WLED_TARGET_ALL vs
 * single-device selection set by wled_devices_set_selected).
 *
 * @param json_body Null-terminated JSON string
 * @return ESP_OK if all targets succeeded, ESP_FAIL if any failed
 */
esp_err_t wled_http_broadcast_post(const char *json_body);

/**
 * Stage 6 diagnostic: GET /json/state from every registered device,
 * log the HTTP status and a summary of the JSON response for each.
 * Call once after WiFi connects, from a task context.
 */
void wled_http_test_all(void);

#ifdef __cplusplus
}
#endif