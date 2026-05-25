#pragma once
/**
 * wled_presets.h — WLED preset registry
 *
 * Fetches /json/presets from the primary device once on WiFi connect,
 * stores them in a compact array, and builds the options string that
 * LVGL's lv_dropdown widget expects ("Name 1\nName 2\nName 3").
 *
 * Preset IDs in WLED are not sequential (user can assign any number 1-250).
 * The registry maps display index → {name, id} so the dropdown can translate
 * a selected row back to the correct ID for the POST payload.
 */

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WLED_PRESETS_MAX       32
#define WLED_PRESET_NAME_LEN   40

typedef struct {
    char    name[WLED_PRESET_NAME_LEN];
    int     id;       // WLED preset ID (1-250)
    bool    active;
} wled_preset_t;

/**
 * Fetch /json/presets from `ip`, parse, and populate the registry.
 * Blocking — call from a task, not from LVGL or an ISR.
 * Safe to call again to refresh.
 */
esp_err_t wled_presets_fetch(const char *ip);

/** Number of presets currently loaded. */
int wled_presets_count(void);

/** Get preset at display index (0-based). Returns NULL if out of range. */
const wled_preset_t *wled_presets_get(int index);

/**
 * Get the WLED preset ID for a display index.
 * Returns -1 if index is out of range.
 */
int wled_presets_get_id(int index);

/**
 * Build the newline-separated options string for lv_dropdown_set_options().
 * Writes into `out` (size `out_len`). Truncates gracefully if buffer is small.
 * Returns the number of presets written.
 */
int wled_presets_build_options(char *out, size_t out_len);

/**
 * Find the display index for a given WLED preset ID.
 * Returns -1 if not found.
 */
int wled_presets_find_by_id(int wled_id);

#ifdef __cplusplus
}
#endif