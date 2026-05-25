/**
 * wled_presets.c — WLED preset registry
 *
 * /json/presets response format:
 * {
 *   "1":  {"n": "Cozy Evening", "on": true, "bri": 128, ...},
 *   "16": {"n": "Party",        "on": true, "bri": 255, ...},
 * }
 * Keys are string representations of the preset ID.
 * We extract "n" (name) and the key itself (ID), sort by ID ascending.
 * WLED injects internal entries at id -1 and 0 — those are skipped.
 */

#include "presets/wled_presets.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "wled_pre";

#define PRESET_BUF_SIZE   4096
#define HTTP_TIMEOUT_MS   5000

static wled_preset_t s_presets[WLED_PRESETS_MAX];
static int           s_count = 0;

// ── HTTP response collector ───────────────────────────────────────────────────

typedef struct {
    char  *buf;
    size_t buf_len;
    size_t written;
} preset_ctx_t;

static esp_err_t preset_http_handler(esp_http_client_event_t *evt)
{
    preset_ctx_t *ctx = (preset_ctx_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && ctx && evt->data_len > 0) {
        size_t space = ctx->buf_len - ctx->written - 1;
        size_t copy  = (size_t)evt->data_len < space
                       ? (size_t)evt->data_len : space;
        if (copy > 0) {
            memcpy(ctx->buf + ctx->written, evt->data, copy);
            ctx->written += copy;
            ctx->buf[ctx->written] = '\0';
        }
    }
    return ESP_OK;
}

// ── Sorting ───────────────────────────────────────────────────────────────────

static int cmp_preset(const void *a, const void *b)
{
    return ((const wled_preset_t *)a)->id - ((const wled_preset_t *)b)->id;
}

// ── Public API ────────────────────────────────────────────────────────────────

esp_err_t wled_presets_fetch(const char *ip)
{
    if (!ip) return ESP_ERR_INVALID_ARG;

    char url[80];
    snprintf(url, sizeof(url), "http://%s/json/presets", ip);

    static char resp[PRESET_BUF_SIZE];
    resp[0] = '\0';

    preset_ctx_t ctx = { resp, sizeof(resp) - 1, 0 };

    esp_http_client_config_t cfg = {
        .url            = url,
        .method         = HTTP_METHOD_GET,
        .timeout_ms     = HTTP_TIMEOUT_MS,
        .event_handler  = preset_http_handler,
        .user_data      = &ctx,
        .keep_alive_enable = false,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_FAIL;

    esp_err_t ret = esp_http_client_perform(client);
    int status    = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (ret != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "[%s] /json/presets failed (ret=0x%x status=%d)", ip, ret, status);
        return ESP_FAIL;
    }

    // Parse JSON object — keys are preset IDs as strings
    memset(s_presets, 0, sizeof(s_presets));
    s_count = 0;

    cJSON *root = cJSON_Parse(resp);
    if (!root) {
        ESP_LOGW(TAG, "cJSON_Parse failed on presets response");
        return ESP_FAIL;
    }

    cJSON *item = NULL;
    cJSON_ArrayForEach(item, root) {
        if (s_count >= WLED_PRESETS_MAX) break;

        int id = atoi(item->string);
        if (id <= 0) continue;  // skip internal WLED entries (id -1, 0)

        cJSON *name_j = cJSON_GetObjectItem(item, "n");
        if (!cJSON_IsString(name_j) || !name_j->valuestring ||
            name_j->valuestring[0] == '\0') continue;

        s_presets[s_count].id = id;
        strlcpy(s_presets[s_count].name, name_j->valuestring, WLED_PRESET_NAME_LEN);
        s_presets[s_count].active = true;
        s_count++;
    }
    cJSON_Delete(root);

    qsort(s_presets, s_count, sizeof(wled_preset_t), cmp_preset);

    ESP_LOGI(TAG, "Loaded %d preset(s) from %s", s_count, ip);
    for (int i = 0; i < s_count; i++) {
        ESP_LOGD(TAG, "  [%d] id=%-3d  \"%s\"", i, s_presets[i].id, s_presets[i].name);
    }

    return s_count > 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

int wled_presets_count(void) { return s_count; }

const wled_preset_t *wled_presets_get(int index)
{
    if (index < 0 || index >= s_count) return NULL;
    return &s_presets[index];
}

int wled_presets_get_id(int index)
{
    if (index < 0 || index >= s_count) return -1;
    return s_presets[index].id;
}

int wled_presets_build_options(char *out, size_t out_len)
{
    if (!out || out_len == 0) return 0;
    out[0] = '\0';
    if (s_count == 0) {
        strlcpy(out, "No presets", out_len);
        return 0;
    }
    int written = 0;
    for (int i = 0; i < s_count; i++) {
        size_t remaining = out_len - strlen(out) - 1;
        if (remaining < 2) break;
        if (i > 0) strlcat(out, "\n", out_len);
        strlcat(out, s_presets[i].name, out_len);
        written++;
    }
    return written;
}

int wled_presets_find_by_id(int wled_id)
{
    for (int i = 0; i < s_count; i++) {
        if (s_presets[i].id == wled_id) return i;
    }
    return -1;
}