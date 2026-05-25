/**
 * main.c — WLED Controller: Stage 9
 *
 * ESP-IDF 5.x entry point.
 * Initializes display, touch, LVGL, and builds the initial UI.
 * All LVGL calls are guarded by a mutex — required once WiFi tasks are added.
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "lvgl.h"

#include "board/board.h"
#include "display/display.h"
#include "touch/touch_ft6336.h"
#include "ui.h"
#include "esp_psram.h"
#include "encoder/encoder_nav.h"
#include "wifi/wifi.h"
#include "devices/wled_devices.h"
#include "http/wled_http.h"
#include "cmd/wled_cmd.h"
#include "poll/wled_poll.h"
#include "presets/wled_presets.h"

static const char *TAG = "main";

// ── LVGL mutex ───────────────────────────────────────────────────────────────
// Any task that calls LVGL APIs must take this mutex first.
// In later stages, the HTTP/poll task will use lvgl_lock() / lvgl_unlock().
static SemaphoreHandle_t s_lvgl_mutex = NULL;

void lvgl_lock(void) { xSemaphoreTakeRecursive(s_lvgl_mutex, portMAX_DELAY); }
void lvgl_unlock(void) { xSemaphoreGiveRecursive(s_lvgl_mutex); }

// ── LVGL draw buffers ────────────────────────────────────────────────────────
static lv_disp_draw_buf_t s_disp_draw_buf;
static lv_disp_drv_t s_disp_drv;
static lv_indev_drv_t s_touch_drv;
static lv_color_t *s_buf1 = NULL;
static lv_color_t *s_buf2 = NULL;

// ── LVGL tick (esp_timer ISR-safe) ───────────────────────────────────────────
static void lvgl_tick_cb(void *arg)
{
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

// ── Touch input driver callback ───────────────────────────────────────────────
static void touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    touch_point_t pt[1];
    uint8_t count = touch_ft6336_read(pt, 1);
    if (count > 0)
    {
        data->state = LV_INDEV_STATE_PR;
        data->point.x = pt[0].x;
        data->point.y = pt[0].y;
    }
    else
    {
        data->state = LV_INDEV_STATE_REL;
    }
}

// ── LVGL handler task ────────────────────────────────────────────────────────
static void lvgl_task(void *arg)
{
    ESP_LOGI(TAG, "LVGL task started on core %d", xPortGetCoreID());

    while (true)
    {
        lvgl_lock();
        uint32_t next_ms = lv_timer_handler();
        lvgl_unlock();
        vTaskDelay(pdMS_TO_TICKS(next_ms < 1 ? 1 : (next_ms > 10 ? 10 : next_ms)));
    }
}


// ── Preset fetch task ─────────────────────────────────────────────────────────
// Waits for WiFi, fetches /json/presets from device 0, populates the dropdown.
// Runs once then deletes itself.
static void preset_fetch_task(void *arg)
{
    // Wait for WiFi
    while (wifi_get_state() != WIFI_STATE_CONNECTED) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    vTaskDelay(pdMS_TO_TICKS(1500));  // let poll task run first

    const wled_device_t *dev = wled_devices_get(0);
    if (dev) {
        esp_err_t ret = wled_presets_fetch(dev->ip);
        if (ret == ESP_OK) {
            lvgl_lock();
            ui_presets_loaded();
            lvgl_unlock();
        } else {
            ESP_LOGW("preset", "Failed to load presets from %s", dev->ip);
        }
    }
    vTaskDelete(NULL);
}

// ── app_main ─────────────────────────────────────────────────────────────────
void app_main(void)
{
    ESP_LOGI(TAG, "WLED Controller — Stage 10: Final UI");
    ESP_LOGI(TAG, "IDF version: %s", esp_get_idf_version());

    // ── Display ──────────────────────────────────────────────────────────────
    ESP_ERROR_CHECK(display_init());

    // ── Touch ────────────────────────────────────────────────────────────────
    esp_err_t touch_err = touch_ft6336_init();
    if (touch_err != ESP_OK)
    {
        ESP_LOGW(TAG, "Touch init failed — continuing without touch");
    }

    // ── LVGL ─────────────────────────────────────────────────────────────────
    lv_init();

    // Allocate draw buffers — PSRAM preferred
    size_t buf_bytes = LCD_H_RES * LVGL_DRAW_BUF_LINES * sizeof(lv_color_t);
    bool has_psram = (esp_psram_get_size() > 0);

    if (has_psram)
    {
        s_buf1 = heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s_buf2 = heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        ESP_LOGI(TAG, "Draw buffers in PSRAM (%zu bytes each)", buf_bytes);
    }
    else
    {
        s_buf1 = heap_caps_malloc(buf_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        s_buf2 = heap_caps_malloc(buf_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        ESP_LOGW(TAG, "No PSRAM — draw buffers in internal DMA RAM (%zu bytes each)", buf_bytes);
    }

    if (!s_buf1 || !s_buf2)
    {
        ESP_LOGE(TAG, "Failed to allocate draw buffers! Free heap: %lu",
                 (unsigned long)esp_get_free_heap_size());
        abort();
    }

    lv_disp_draw_buf_init(&s_disp_draw_buf, s_buf1, s_buf2,
                          LCD_H_RES * LVGL_DRAW_BUF_LINES);

    // Register display driver
    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res = LCD_H_RES;
    s_disp_drv.ver_res = LCD_V_RES;
    s_disp_drv.flush_cb = lvgl_flush_cb;
    s_disp_drv.draw_buf = &s_disp_draw_buf;
    s_disp_drv.full_refresh = 0;
    lv_disp_drv_register(&s_disp_drv);

    // Register touch input driver
    lv_indev_drv_init(&s_touch_drv);
    s_touch_drv.type = LV_INDEV_TYPE_POINTER;
    s_touch_drv.read_cb = touch_read_cb;
    lv_indev_drv_register(&s_touch_drv);

    // LVGL tick timer
    const esp_timer_create_args_t tick_args = {
        .callback = lvgl_tick_cb,
        .name = "lvgl_tick",
        .skip_unhandled_events = true,
    };
    esp_timer_handle_t tick_timer;
    ESP_ERROR_CHECK(esp_timer_create(&tick_args, &tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick_timer,
                                             LVGL_TICK_PERIOD_MS * 1000)); // microseconds

    // Mutex for LVGL thread safety
    s_lvgl_mutex = xSemaphoreCreateRecursiveMutex();

    ESP_LOGI(TAG, "LVGL initialized");

    // ── Build UI ─────────────────────────────────────────────────────────────
    lvgl_lock();
    ui_build();
    ui_set_status("Status: Test UI (not functional)", "Segments: --");
    lvgl_unlock();

    // ── Rotary encoder ───────────────────────────────────────────────────────
    // encoder_nav_init() must run after ui_build() so widget handles are valid.
    // It starts encoder_task on core 0 — same core as future WiFi tasks.
    esp_err_t enc_err = encoder_nav_init();
    if (enc_err != ESP_OK) {
        ESP_LOGW(TAG, "Encoder init failed — continuing without encoder (0x%x)", enc_err);
    }

    // ── WiFi ─────────────────────────────────────────────────────────────────
    // wifi_init() starts the STA driver and begins connecting. The event
    // handler updates the UI dot (grey→yellow→green) and schedules retries.
    // NVS init is handled inside wifi_init() — no need to call it here.
    esp_err_t wifi_err = wifi_init();
    if (wifi_err != ESP_OK) {
        ESP_LOGW(TAG, "WiFi init failed (0x%x) — continuing offline", wifi_err);
    }

    // ── WLED device registry ──────────────────────────────────────────────────
    // On first boot: reads spiffs_data/devices.cfg from the storage partition
    // and persists to NVS.  On all subsequent boots: loads from NVS directly.
    // To update devices without reflashing firmware:
    //   1. Edit  spiffs_data/devices.cfg
    //   2. Run   idf.py storage-flash
    //   3. Erase NVS: idf.py -p COMx erase-region 0x9000 0x6000
    wled_devices_init();

    // Default: broadcast to all devices
    wled_devices_set_selected(WLED_TARGET_ALL);

    // ── Command layer ─────────────────────────────────────────────────────────
    // Owns the brightness debounce timer and the wled_cmd_task (core 0).
    // Must be called after wifi_init() but before any UI events can fire.
    esp_err_t cmd_err = wled_cmd_init();
    if (cmd_err != ESP_OK) {
        ESP_LOGW(TAG, "Command layer init failed (0x%x)", cmd_err);
    }

    // ── State polling task ────────────────────────────────────────────────────
    // GETs /json/state every CONFIG_WLED_POLL_INTERVAL_MS and pushes updates
    // to the UI. Skips UI updates for WLED_POLL_CMD_HOLD_MS after any command.
    esp_err_t poll_err = wled_poll_init();
    if (poll_err != ESP_OK) {
        ESP_LOGW(TAG, "Poll init failed (0x%x)", poll_err);
    }

    // ── Preset fetch task ─────────────────────────────────────────────────────
    xTaskCreatePinnedToCore(preset_fetch_task, "preset_fetch", 6144, NULL, 2, NULL, 0);

    // ── Start LVGL handler task ───────────────────────────────────────────────
    // Pinned to core 1; leave core 0 for WiFi/network tasks in later stages
    xTaskCreatePinnedToCore(
        lvgl_task,
        "lvgl",
        8192, // stack bytes
        NULL,
        5, // priority
        NULL,
        1 // core 1
    );

    ESP_LOGI(TAG, "Boot complete — LVGL running on core 1");

    // app_main can return; the lvgl_task keeps everything alive
}