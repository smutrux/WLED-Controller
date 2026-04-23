#pragma once
/**
 * encoder_nav.h — Focus/edit navigation layer for the rotary encoder
 *
 * Sits between the raw encoder events (CW / CCW / PRESS) and the LVGL UI.
 * Manages a list of "targets" (focusable widgets) and two interaction modes:
 *
 *   NAVIGATE mode  (no widget selected)
 *     • CW / CCW  — move focus highlight to next / previous target
 *     • PRESS     — activate the focused target:
 *                     button  → toggle it immediately
 *                     slider  → enter EDIT mode
 *
 *   EDIT mode  (slider selected)
 *     • CW / CCW  — increment / decrement the slider value
 *     • PRESS     — exit back to NAVIGATE mode (keep focus highlight)
 *
 * Visual feedback:
 *   Focus (navigate)  — orange outline (2 px) around the widget
 *   Edit  (slider)    — cyan   outline (2 px) + enlarged knob
 *   No focus          — no outline
 */

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialise the navigation layer.
 * Must be called after ui_build() so that all widget handles are valid,
 * and after LVGL has been initialised.
 */
esp_err_t encoder_nav_init(void);

#ifdef __cplusplus
}
#endif
