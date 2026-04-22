#pragma once
/**
 * encoder.h — Quadrature rotary encoder hardware driver
 *
 * Decodes CLK/DT signals using the ESP32-S3 PCNT peripheral (hardware
 * quadrature decode, no polling/IRQ overhead on the CPU for rotation).
 * Button debounce is handled via a 30 ms esp_timer one-shot.
 *
 * Usage:
 *   encoder_init(my_callback, NULL);
 *   // my_callback fires ENCODER_EVENT_CW / CCW / PRESS from encoder_task context
 */

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ENCODER_EVENT_CW,    ///< One detent clockwise
    ENCODER_EVENT_CCW,   ///< One detent counter-clockwise
    ENCODER_EVENT_PRESS, ///< Button pressed (debounced, fires on release confirmation)
} encoder_event_t;

/**
 * Callback invoked from the encoder polling task (not an ISR).
 * It is safe to take mutexes and call LVGL (with lvgl_lock) inside.
 */
typedef void (*encoder_cb_t)(encoder_event_t event, void *ctx);

/**
 * Initialise PCNT, GPIO button interrupt, and start the polling task.
 * Call once after nvs_flash_init, before the LVGL task starts.
 *
 * @param cb   Callback for encoder events (called from encoder_task context)
 * @param ctx  Opaque pointer passed back to cb
 */
esp_err_t encoder_init(encoder_cb_t cb, void *ctx);

#ifdef __cplusplus
}
#endif
