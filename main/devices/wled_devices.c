/**
 * wled_devices.c — Device registry backed by NVS, seeded from SPIFFS
 *
 * Boot sequence:
 *   1. Try loading from NVS (fast path — normal every boot after first)
 *   2. If NVS is empty, try reading spiffs_data/devices.cfg from SPIFFS
 *   3. Each device parsed from the file is added via wled_devices_add(),
 *      which persists to NVS — so step 2 only runs once per NVS erase.
 *
 * devices.cfg format (in spiffs_data/, committed to the repo):
 *   # comment lines start with #
 *   Name, IP address
 *   Tall,    10.0.0.65
 *
 * To update the device list without reflashing firmware:
 *   1. Edit  spiffs_data/devices.cfg
 *   2. Run   idf.py storage-flash
 *   3. Erase NVS so the file is re-read on next boot:
 *        idf.py -p COMx erase-region 0x9000 0x6000
 *      (or use idf.py erase-flash to erase everything and reflash)
 *
 * NVS layout (namespace "wled_dev"):
 *   "count"       u8    number of active device slots used
 *   "dev_0_name"  str   name of device 0
 *   "dev_0_ip"    str   IP of device 0
 *   "dev_N_name"  str   ... up to WLED_MAX_DEVICES-1
 */

#include "devices/wled_devices.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_spiffs.h"
#include "esp_log.h"
#include "esp_check.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>

static const char *TAG       = "wled_dev";
static const char *NVS_NS    = "wled_dev";
static const char *KEY_COUNT = "count";

#define CFG_PATH  "/spiffs/devices.cfg"

// ── In-memory state ───────────────────────────────────────────────────────────
static wled_device_t s_devices[WLED_MAX_DEVICES];
static int           s_count    = 0;
static int           s_selected = WLED_TARGET_ALL;

// ── String helpers ────────────────────────────────────────────────────────────

static char *str_trim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s) - 1;
    while (end >= s && isspace((unsigned char)*end)) *end-- = '\0';
    return s;
}

// ── NVS helpers ───────────────────────────────────────────────────────────────

static void make_key(char *out, size_t out_len, int idx, const char *field)
{
    snprintf(out, out_len, "dev_%d_%s", idx, field);
}

static esp_err_t nvs_save(void)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NS, NVS_READWRITE, &h), TAG, "nvs_open");

    char key[16];
    esp_err_t ret = nvs_set_u8(h, KEY_COUNT, (uint8_t)s_count);
    if (ret != ESP_OK) goto done;

    for (int i = 0; i < WLED_MAX_DEVICES; i++) {
        make_key(key, sizeof(key), i, "name");
        if (i < s_count && s_devices[i].active) {
            ret = nvs_set_str(h, key, s_devices[i].name);
            if (ret != ESP_OK) goto done;
            make_key(key, sizeof(key), i, "ip");
            ret = nvs_set_str(h, key, s_devices[i].ip);
            if (ret != ESP_OK) goto done;
        } else {
            nvs_erase_key(h, key);
            make_key(key, sizeof(key), i, "ip");
            nvs_erase_key(h, key);
        }
    }
    ret = nvs_commit(h);

done:
    nvs_close(h);
    if (ret != ESP_OK) ESP_LOGE(TAG, "NVS save failed: 0x%x", ret);
    return ret;
}

static esp_err_t nvs_load(void)
{
    nvs_handle_t h;
    esp_err_t ret = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (ret == ESP_ERR_NVS_NOT_FOUND) return ESP_ERR_NOT_FOUND;
    ESP_RETURN_ON_ERROR(ret, TAG, "nvs_open");

    uint8_t count = 0;
    ret = nvs_get_u8(h, KEY_COUNT, &count);
    if (ret == ESP_ERR_NVS_NOT_FOUND || count == 0) {
        nvs_close(h);
        return ESP_ERR_NOT_FOUND;   // namespace exists but no devices saved
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
        ESP_LOGW(TAG, "NVS load error 0x%x — will try SPIFFS", ret);
        memset(s_devices, 0, sizeof(s_devices));
        s_count = 0;
        return ret;
    }
    return ESP_OK;
}

// ── SPIFFS loader ─────────────────────────────────────────────────────────────

static esp_err_t spiffs_init(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path              = "/spiffs",
        .partition_label        = "storage",
        .max_files              = 4,
        .format_if_mount_failed = false,
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SPIFFS mount failed (0x%x) — no devices.cfg available", ret);
    }
    return ret;
}

static esp_err_t load_from_cfg_file(void)
{
    FILE *f = fopen(CFG_PATH, "r");
    if (!f) {
        ESP_LOGW(TAG, "devices.cfg not found at %s", CFG_PATH);
        ESP_LOGW(TAG, "Flash it with:  idf.py storage-flash");
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Reading device list from %s", CFG_PATH);

    char line[80];
    int  loaded = 0;

    while (fgets(line, sizeof(line), f)) {
        // Strip newline
        line[strcspn(line, "\r\n")] = '\0';

        char *p = str_trim(line);

        // Skip blanks and comments
        if (*p == '\0' || *p == '#') continue;

        // Split on first comma
        char *comma = strchr(p, ',');
        if (!comma) {
            ESP_LOGW(TAG, "Skipping malformed line (no comma): \"%s\"", p);
            continue;
        }
        *comma = '\0';

        char *name = str_trim(p);
        char *ip   = str_trim(comma + 1);

        if (*name == '\0' || *ip == '\0') {
            ESP_LOGW(TAG, "Skipping line with empty name or IP");
            continue;
        }

        if (loaded >= WLED_MAX_DEVICES) {
            ESP_LOGW(TAG, "Max devices (%d) reached, ignoring \"%s\"",
                     WLED_MAX_DEVICES, name);
            break;
        }

        // Use wled_devices_add so NVS is written as we go
        esp_err_t ret = wled_devices_add(name, ip);
        if (ret == ESP_OK) {
            loaded++;
        }
    }

    fclose(f);
    ESP_LOGI(TAG, "Loaded %d device(s) from devices.cfg", loaded);
    return loaded > 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

// ── Public API ────────────────────────────────────────────────────────────────

esp_err_t wled_devices_init(void)
{
    memset(s_devices, 0, sizeof(s_devices));
    s_count    = 0;
    s_selected = WLED_TARGET_ALL;

    // Fast path: NVS already has the list from a previous boot
    esp_err_t ret = nvs_load();
    if (ret == ESP_OK && s_count > 0) {
        ESP_LOGI(TAG, "Device registry ready — %d device(s) from NVS", s_count);
        return ESP_OK;
    }

    // Slow path: NVS is empty — read devices.cfg from SPIFFS and populate NVS
    ESP_LOGI(TAG, "NVS empty, loading from SPIFFS devices.cfg...");

    if (spiffs_init() == ESP_OK) {
        load_from_cfg_file();
        // SPIFFS stays mounted intentionally. Calling esp_vfs_spiffs_unregister()
        // after reading leaves a dangling entry in the VFS table that corrupts
        // esp_vfs_select() when esp_http_client later calls select() on a socket.
        // The mount overhead is negligible; keeping it avoids the crash on first boot.
    }

    if (s_count == 0) {
        ESP_LOGW(TAG, "No devices loaded. Check devices.cfg and run: idf.py storage-flash");
    }

    ESP_LOGI(TAG, "Device registry ready — %d device(s)", s_count);
    return ESP_OK;
}

esp_err_t wled_devices_add(const char *name, const char *ip)
{
    if (!name || !ip || name[0] == '\0' || ip[0] == '\0')
        return ESP_ERR_INVALID_ARG;
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
    if (index < 0 || index >= s_count || !s_devices[index].active)
        return ESP_ERR_NOT_FOUND;

    for (int i = index; i < s_count - 1; i++)
        s_devices[i] = s_devices[i + 1];
    memset(&s_devices[s_count - 1], 0, sizeof(wled_device_t));
    s_count--;

    if (s_selected == index)       s_selected = WLED_TARGET_ALL;
    else if (s_selected > index)   s_selected--;

    ESP_LOGI(TAG, "Removed device %d, %d remaining", index, s_count);
    return nvs_save();
}

int wled_devices_count(void)          { return s_count; }

const wled_device_t *wled_devices_get(int index)
{
    if (index < 0 || index >= s_count || !s_devices[index].active)
        return NULL;
    return &s_devices[index];
}

esp_err_t wled_devices_set_selected(int index)
{
    if (index != WLED_TARGET_ALL && (index < 0 || index >= s_count)) {
        ESP_LOGW(TAG, "set_selected: %d out of range (count=%d)", index, s_count);
        return ESP_ERR_INVALID_ARG;
    }
    s_selected = index;
    return ESP_OK;
}

int wled_devices_get_selected(void)   { return s_selected; }

int wled_devices_get_targets(const char **out_ips)
{
    if (!out_ips || s_count == 0) return 0;

    if (s_selected == WLED_TARGET_ALL) {
        for (int i = 0; i < s_count; i++)
            out_ips[i] = s_devices[i].ip;
        return s_count;
    }

    if (s_selected >= 0 && s_selected < s_count) {
        out_ips[0] = s_devices[s_selected].ip;
        return 1;
    }
    return 0;
}