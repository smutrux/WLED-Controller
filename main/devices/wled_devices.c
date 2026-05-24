/**
 * wled_devices.c — NVS-backed registry of WLED instances
 *
 * NVS layout (namespace "wled_dev"):
 *   "count"       u8    number of active device slots used
 *   "dev_0_name"  str   name of device 0
 *   "dev_0_ip"    str   IP of device 0
 *   "dev_1_name"  str   ... and so on up to WLED_MAX_DEVICES-1
 *
 * Inactive slots are simply absent from NVS — add/remove rewrites all keys.
 * This keeps the NVS logic simple at the cost of O(N) writes on mutation,
 * which is fine for a list that changes rarely.
 */

#include "devices/wled_devices.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_check.h"
#include <string.h>
#include <stdio.h>

static const char *TAG        = "wled_dev";
static const char *NVS_NS     = "wled_dev";   // NVS namespace (max 15 chars)
static const char *KEY_COUNT  = "count";

// ── In-memory state ───────────────────────────────────────────────────────────
static wled_device_t s_devices[WLED_MAX_DEVICES];
static int           s_count    = 0;
static int           s_selected = WLED_TARGET_ALL;

// ── NVS helpers ───────────────────────────────────────────────────────────────

/** Build the NVS key for a given slot and field ("name" or "ip"). */
static void make_key(char *out, size_t out_len, int idx, const char *field)
{
    // Keys must be ≤ 15 chars.  "dev_7_name" = 10 chars — fits comfortably.
    snprintf(out, out_len, "dev_%d_%s", idx, field);
}

/** Persist the entire in-memory device list to NVS. Called after every mutation. */
static esp_err_t nvs_save(void)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NS, NVS_READWRITE, &h), TAG, "nvs_open");

    char key[16];

    // Write count
    esp_err_t ret = nvs_set_u8(h, KEY_COUNT, (uint8_t)s_count);
    if (ret != ESP_OK) goto done;

    // Write all active slots; erase leftover slots beyond current count
    for (int i = 0; i < WLED_MAX_DEVICES; i++) {
        make_key(key, sizeof(key), i, "name");
        if (i < s_count && s_devices[i].active) {
            ret = nvs_set_str(h, key, s_devices[i].name);
            if (ret != ESP_OK) goto done;
            make_key(key, sizeof(key), i, "ip");
            ret = nvs_set_str(h, key, s_devices[i].ip);
            if (ret != ESP_OK) goto done;
        } else {
            // Erase stale slots — ignore "not found" errors
            nvs_erase_key(h, key);
            make_key(key, sizeof(key), i, "ip");
            nvs_erase_key(h, key);
        }
    }

    ret = nvs_commit(h);

done:
    nvs_close(h);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS save failed: 0x%x", ret);
    }
    return ret;
}

/** Load the device list from NVS into s_devices[]. */
static esp_err_t nvs_load(void)
{
    nvs_handle_t h;
    esp_err_t ret = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        // Namespace doesn't exist yet — first boot, no devices stored
        ESP_LOGI(TAG, "No device list in NVS (first boot)");
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "nvs_open readonly");

    uint8_t count = 0;
    ret = nvs_get_u8(h, KEY_COUNT, &count);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        // Key absent — treat as empty
        nvs_close(h);
        return ESP_OK;
    }
    if (ret != ESP_OK) goto done;
    if (count > WLED_MAX_DEVICES) count = WLED_MAX_DEVICES;

    char key[16];
    for (int i = 0; i < (int)count; i++) {
        size_t len;

        make_key(key, sizeof(key), i, "name");
        len = WLED_NAME_MAX_LEN;
        ret = nvs_get_str(h, key, s_devices[i].name, &len);
        if (ret != ESP_OK) goto done;

        make_key(key, sizeof(key), i, "ip");
        len = WLED_IP_MAX_LEN;
        ret = nvs_get_str(h, key, s_devices[i].ip, &len);
        if (ret != ESP_OK) goto done;

        s_devices[i].active = true;
        ESP_LOGI(TAG, "  [%d] \"%s\" @ %s", i, s_devices[i].name, s_devices[i].ip);
    }
    s_count = (int)count;

done:
    nvs_close(h);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "NVS load error 0x%x — starting with empty list", ret);
        memset(s_devices, 0, sizeof(s_devices));
        s_count = 0;
        ret = ESP_OK;   // non-fatal: we just start fresh
    }
    return ret;
}

// ── Public API ────────────────────────────────────────────────────────────────

esp_err_t wled_devices_init(void)
{
    memset(s_devices, 0, sizeof(s_devices));
    s_count    = 0;
    s_selected = WLED_TARGET_ALL;

    esp_err_t ret = nvs_load();
    ESP_LOGI(TAG, "Device registry ready — %d device(s) loaded", s_count);
    return ret;
}

esp_err_t wled_devices_add(const char *name, const char *ip)
{
    if (!name || !ip || name[0] == '\0' || ip[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_count >= WLED_MAX_DEVICES) {
        ESP_LOGW(TAG, "Registry full (%d devices)", WLED_MAX_DEVICES);
        return ESP_ERR_NO_MEM;
    }

    int slot = s_count;
    strlcpy(s_devices[slot].name, name, WLED_NAME_MAX_LEN);
    strlcpy(s_devices[slot].ip,   ip,   WLED_IP_MAX_LEN);
    s_devices[slot].active = true;
    s_count++;

    ESP_LOGI(TAG, "Added [%d] \"%s\" @ %s", slot, name, ip);
    return nvs_save();
}

esp_err_t wled_devices_remove(int index)
{
    if (index < 0 || index >= s_count || !s_devices[index].active) {
        return ESP_ERR_NOT_FOUND;
    }

    // Compact the array: shift everything above index down one slot
    for (int i = index; i < s_count - 1; i++) {
        s_devices[i] = s_devices[i + 1];
    }
    memset(&s_devices[s_count - 1], 0, sizeof(wled_device_t));
    s_count--;

    // If the removed device was selected, fall back to ALL
    if (s_selected == index) {
        s_selected = WLED_TARGET_ALL;
    } else if (s_selected > index) {
        s_selected--;   // shift index to match compacted array
    }

    ESP_LOGI(TAG, "Removed device %d, %d remaining", index, s_count);
    return nvs_save();
}

int wled_devices_count(void)
{
    return s_count;
}

const wled_device_t *wled_devices_get(int index)
{
    if (index < 0 || index >= s_count || !s_devices[index].active) {
        return NULL;
    }
    return &s_devices[index];
}

esp_err_t wled_devices_set_selected(int index)
{
    if (index != WLED_TARGET_ALL && (index < 0 || index >= s_count)) {
        ESP_LOGW(TAG, "set_selected: index %d out of range (count=%d)", index, s_count);
        return ESP_ERR_INVALID_ARG;
    }
    s_selected = index;
    if (index == WLED_TARGET_ALL) {
        ESP_LOGD(TAG, "Selection → ALL");
    } else {
        ESP_LOGD(TAG, "Selection → [%d] \"%s\"", index, s_devices[index].name);
    }
    return ESP_OK;
}

int wled_devices_get_selected(void)
{
    return s_selected;
}

int wled_devices_get_targets(const char **out_ips)
{
    if (!out_ips || s_count == 0) return 0;

    if (s_selected == WLED_TARGET_ALL) {
        for (int i = 0; i < s_count; i++) {
            out_ips[i] = s_devices[i].ip;
        }
        return s_count;
    }

    // Single device selected
    if (s_selected >= 0 && s_selected < s_count) {
        out_ips[0] = s_devices[s_selected].ip;
        return 1;
    }

    return 0;
}
