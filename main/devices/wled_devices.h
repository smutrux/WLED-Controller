#pragma once
/**
 * wled_devices.h — NVS-backed registry of WLED instances
 *
 * Stores up to WLED_MAX_DEVICES devices, each with a display name and IP
 * address, persisted in NVS so they survive reboots and don't require a
 * reflash to change.
 *
 * Selection model:
 *   WLED_TARGET_ALL  (-1) — commands broadcast to every registered device
 *   0 … N-1              — commands sent to that specific device only
 *
 * Typical call sequence:
 *   wled_devices_init();                          // load from NVS
 *   wled_devices_add("Living Room", "192.168.1.10");
 *   wled_devices_add("Kitchen",     "192.168.1.11");
 *   wled_devices_set_selected(WLED_TARGET_ALL);   // default: all
 *
 * The HTTP layer (stage 6+) calls wled_devices_get_targets() to get the list
 * of IPs to POST to for a given command.
 */

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ── Limits ────────────────────────────────────────────────────────────────────
#define WLED_MAX_DEVICES    8
#define WLED_NAME_MAX_LEN   32   // including null terminator
#define WLED_IP_MAX_LEN     40   // enough for IPv4 dotted-decimal + null

// ── Target sentinel ───────────────────────────────────────────────────────────
#define WLED_TARGET_ALL    (-1)   ///< Broadcast to all registered devices

// ── Device descriptor ─────────────────────────────────────────────────────────
typedef struct {
    char name[WLED_NAME_MAX_LEN];  ///< Human-readable label, e.g. "Living Room"
    char ip[WLED_IP_MAX_LEN];      ///< IPv4 address, e.g. "192.168.1.10"
    bool active;                   ///< false = slot unused
} wled_device_t;

// ── API ───────────────────────────────────────────────────────────────────────

/**
 * Load the device list from NVS.  Call once after wifi_init().
 * Safe to call on an empty NVS partition — returns ESP_OK with count 0.
 */
esp_err_t wled_devices_init(void);

/**
 * Add a device to the registry and persist it to NVS.
 * Returns ESP_ERR_NO_MEM if WLED_MAX_DEVICES is already reached.
 * Returns ESP_ERR_INVALID_ARG if name or ip is NULL/empty.
 */
esp_err_t wled_devices_add(const char *name, const char *ip);

/**
 * Remove the device at index and persist the change.
 * Returns ESP_ERR_NOT_FOUND if the slot is empty.
 */
esp_err_t wled_devices_remove(int index);

/** Return the number of registered (active) devices. */
int wled_devices_count(void);

/**
 * Get a pointer to the device at index (0-based).
 * Returns NULL if index is out of range or the slot is empty.
 * Pointer is valid until the next add/remove call.
 */
const wled_device_t *wled_devices_get(int index);

/**
 * Set the current target selection.
 * Pass WLED_TARGET_ALL (-1) for broadcast mode.
 * Pass a valid device index for single-device mode.
 * Returns ESP_ERR_INVALID_ARG if index is out of range.
 */
esp_err_t wled_devices_set_selected(int index);

/** Return the current selection (WLED_TARGET_ALL or a device index). */
int wled_devices_get_selected(void);

/**
 * Populate `out_ips` with pointers to the IP strings that should receive the
 * next command, based on the current selection.
 *
 * `out_ips` must point to an array of at least WLED_MAX_DEVICES `const char*`.
 *
 * Returns the number of IPs written (0 if no devices registered).
 *
 * Usage in the HTTP layer:
 *   const char *ips[WLED_MAX_DEVICES];
 *   int n = wled_devices_get_targets(ips);
 *   for (int i = 0; i < n; i++) { http_post(ips[i], payload); }
 */
int wled_devices_get_targets(const char **out_ips);

#ifdef __cplusplus
}
#endif
