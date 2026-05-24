#pragma once
/**
 * wled_cmd.h — WLED command dispatch layer
 *
 * Sits between the UI event handlers and wled_http_broadcast_post().
 * Responsibilities:
 *   - Build the correct JSON payload for each command type
 *   - Debounce slider/encoder changes so rapid movement sends one
 *     HTTP request at the end of the gesture, not one per step
 *   - Dispatch from any task context (safe to call from LVGL task,
 *     encoder task, or future poll task)
 *
 * Commands that are fire-and-forget (power toggle) send immediately.
 * Commands that can arrive in bursts (brightness) are debounced with
 * a one-shot esp_timer (WLED_CMD_BRIGHTNESS_DEBOUNCE_MS).
 *
 * All HTTP calls happen on a dedicated wled_cmd_task (core 0) so the
 * LVGL task never blocks on network I/O.
 */

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialise the command layer.
 * Creates the debounce timer and the dispatch task.
 * Call once from app_main after wifi_init().
 */
esp_err_t wled_cmd_init(void);

/**
 * Send a power on/off command immediately (no debounce).
 * Safe to call from any task context.
 */
void wled_cmd_set_power(bool on);

/**
 * Schedule a brightness command.
 * The actual POST is debounced — repeated calls within
 * WLED_CMD_BRIGHTNESS_DEBOUNCE_MS reset the timer.
 * Safe to call from any task context.
 */
void wled_cmd_set_brightness(uint8_t bri);

#ifdef __cplusplus
}
#endif

/**
 * Returns true if a command was sent within the last `within_ms` milliseconds.
 * Used by the poll task to suppress UI updates immediately after a user action,
 * preventing the old device state from flickering back onto the screen before
 * the device has processed the command.
 */
bool wled_cmd_recently_sent(uint32_t within_ms);