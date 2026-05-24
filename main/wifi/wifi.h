#pragma once
/**
 * wifi.h — WiFi station driver with exponential backoff reconnection
 *
 * Credentials are stored in Kconfig (menuconfig → WLED Controller WiFi).
 * No SSID or password appears in source code.
 *
 * State transitions:
 *   DISCONNECTED → CONNECTING   : wifi_init() called / reconnect attempt starts
 *   CONNECTING   → CONNECTED    : IP address received (IP_EVENT_STA_GOT_IP)
 *   CONNECTED    → CONNECTING   : connection dropped (WIFI_EVENT_STA_DISCONNECTED)
 *   CONNECTING   → DISCONNECTED : max retries reached (WIFI_MAX_RETRY_COUNT)
 *
 * All UI updates (dot colour) are pushed from the event handler task using
 * lvgl_lock() / lvgl_unlock().
 */

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WIFI_STATE_DISCONNECTED, ///< Not connected, not retrying
    WIFI_STATE_CONNECTING,   ///< Actively trying to connect / waiting to retry
    WIFI_STATE_CONNECTED,    ///< IP address obtained
} wifi_conn_state_t;

/**
 * Initialise NVS, the TCP/IP stack, the default event loop, and WiFi STA mode.
 * Begins connecting immediately.  Safe to call once from app_main.
 */
esp_err_t wifi_init(void);

/** Return the current connection state (thread-safe read of an atomic). */
wifi_conn_state_t wifi_get_state(void);

#ifdef __cplusplus
}
#endif
