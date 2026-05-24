#pragma once
/**
 * wled_poll.h — Periodic WLED state polling task
 *
 * Spawns a task on core 0 that GETs /json/state every
 * CONFIG_WLED_POLL_INTERVAL_MS milliseconds and pushes the parsed result
 * to the UI via lvgl_lock() / lvgl_unlock().
 *
 * Polled fields:
 *   on       → ui_set_power()
 *   bri      → ui_set_brightness()
 *   seg[0].col[0]  → ui_set_color()  (first segment, primary colour)
 *   fx (seg[0])    → ui_set_effect()
 *
 * Which device is polled:
 *   WLED_TARGET_ALL → device 0 (reference device for broadcast state)
 *   specific index  → that device
 *
 * Command conflict avoidance:
 *   After wled_cmd sends a command, the poll task skips UI updates for
 *   WLED_POLL_CMD_HOLD_MS to avoid flickering the UI back to stale state
 *   before the device has processed the command.  HTTP reachability errors
 *   are logged but do not stop the poll loop.
 */

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialise and start the poll task.
 * Call after wifi_init() and wled_cmd_init() from app_main.
 */
esp_err_t wled_poll_init(void);

/**
 * Pause or resume UI updates from the poll task.
 * Pausing is useful while the user is actively dragging a slider —
 * the poll task keeps running (maintaining reachability stats) but
 * won't overwrite widgets the user is currently interacting with.
 * Not currently called automatically; available for future use.
 */
void wled_poll_set_paused(bool paused);

#ifdef __cplusplus
}
#endif