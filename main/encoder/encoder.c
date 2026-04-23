/**
 * encoder.c — Quadrature rotary encoder hardware driver
 *
 * Hardware:  5-pin rotary encoder  (GND, +, SW, DT, CLK)
 * Pins used: see board.h (PIN_ENC_CLK, PIN_ENC_DT, PIN_ENC_SW)
 *
 * PCNT quadrature decode (X4 — counts both edges of both signals):
 *   Channel A: edge on CLK, level from DT
 *   Channel B: edge on DT,  level from CLK
 * This gives 4 PCNT counts per physical detent on a typical encoder.
 * COUNTS_PER_DETENT can be halved to 2 if events fire twice per click.
 *
 * Button:
 *   GPIO interrupt (falling edge) → 30 ms debounce timer → confirm low → press event.
 *   Active-low with internal pull-up.  No external resistor needed.
 */

#include "encoder/encoder.h"
#include "board/board.h"

#include "driver/pulse_cnt.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"

static const char *TAG = "encoder";

// ── Tuning ────────────────────────────────────────────────────────────────────
// Most cheap 20-detent encoders produce 4 PCNT counts per detent in X4 mode.
// If your encoder fires two events per click, change this to 2.
// If it fires four events per click, change this to 1.
#define COUNTS_PER_DETENT   4

// Glitch filter: ignore signals shorter than 1 µs (prevents false counts from
// contact bounce on the CLK/DT pins — different from the SW button debounce).
#define PCNT_GLITCH_NS      1000

// Button debounce window in milliseconds.
#define DEBOUNCE_MS         30

// ── State ─────────────────────────────────────────────────────────────────────
static pcnt_unit_handle_t    s_pcnt_unit   = NULL;
static pcnt_channel_handle_t s_pcnt_chan_a = NULL;
static pcnt_channel_handle_t s_pcnt_chan_b = NULL;
static esp_timer_handle_t    s_debounce_timer;

static encoder_cb_t s_cb  = NULL;
static void        *s_ctx = NULL;

// ── Button ISR + debounce timer ───────────────────────────────────────────────

static void debounce_expired(void *arg)
{
    // Confirm pin is still low (held) — avoids spurious fires on quick glitches
    if (gpio_get_level(PIN_ENC_SW) == 0) {
        if (s_cb) {
            s_cb(ENCODER_EVENT_PRESS, s_ctx);
        }
    }
}

static void IRAM_ATTR btn_isr(void *arg)
{
    // (Re)start the one-shot debounce timer.  stop() on an already-stopped
    // timer returns ESP_ERR_INVALID_STATE which we safely ignore here.
    esp_timer_stop(s_debounce_timer);
    esp_timer_start_once(s_debounce_timer, DEBOUNCE_MS * 1000ULL);
}

// ── PCNT init ─────────────────────────────────────────────────────────────────

static esp_err_t pcnt_encoder_init(void)
{
    // Unit with wide accumulation range — we never expect to overflow
    pcnt_unit_config_t unit_cfg = {
        .high_limit = 32000,
        .low_limit  = -32000,
    };
    ESP_RETURN_ON_ERROR(pcnt_new_unit(&unit_cfg, &s_pcnt_unit),
                        TAG, "pcnt_new_unit");

    // Hardware glitch filter (rejects bounce on CLK/DT lines)
    pcnt_glitch_filter_config_t glitch_cfg = {
        .max_glitch_ns = PCNT_GLITCH_NS,
    };
    ESP_RETURN_ON_ERROR(pcnt_unit_set_glitch_filter(s_pcnt_unit, &glitch_cfg),
                        TAG, "set_glitch_filter");

    // ── Channel A: detect edges on CLK, read level from DT ───────────────────
    pcnt_chan_config_t chan_a_cfg = {
        .edge_gpio_num  = PIN_ENC_CLK,
        .level_gpio_num = PIN_ENC_DT,
    };
    ESP_RETURN_ON_ERROR(pcnt_new_channel(s_pcnt_unit, &chan_a_cfg, &s_pcnt_chan_a),
                        TAG, "pcnt_new_channel A");

    // CLK rising  + DT high  →  -1 (CCW)
    // CLK rising  + DT low   →  +1 (CW)   (inverse of above)
    // CLK falling + DT high  →  +1 (CW)
    // CLK falling + DT low   →  -1 (CCW)  (inverse of above)
    pcnt_channel_set_edge_action(s_pcnt_chan_a,
        PCNT_CHANNEL_EDGE_ACTION_INCREASE,   // CLK rising → count down
        PCNT_CHANNEL_EDGE_ACTION_DECREASE);  // CLK falling → count up
    pcnt_channel_set_level_action(s_pcnt_chan_a,
        PCNT_CHANNEL_LEVEL_ACTION_KEEP,      // DT high → keep direction as-is
        PCNT_CHANNEL_LEVEL_ACTION_INVERSE);  // DT low  → invert direction

    // ── Channel B: detect edges on DT, read level from CLK ───────────────────
    pcnt_chan_config_t chan_b_cfg = {
        .edge_gpio_num  = PIN_ENC_DT,
        .level_gpio_num = PIN_ENC_CLK,
    };
    ESP_RETURN_ON_ERROR(pcnt_new_channel(s_pcnt_unit, &chan_b_cfg, &s_pcnt_chan_b),
                        TAG, "pcnt_new_channel B");

    pcnt_channel_set_edge_action(s_pcnt_chan_b,
        PCNT_CHANNEL_EDGE_ACTION_DECREASE,   // DT rising → count up
        PCNT_CHANNEL_EDGE_ACTION_INCREASE);  // DT falling → count down
    pcnt_channel_set_level_action(s_pcnt_chan_b,
        PCNT_CHANNEL_LEVEL_ACTION_KEEP,      // CLK high → keep direction
        PCNT_CHANNEL_LEVEL_ACTION_INVERSE);  // CLK low  → invert direction

    ESP_RETURN_ON_ERROR(pcnt_unit_enable(s_pcnt_unit),    TAG, "pcnt_enable");
    ESP_RETURN_ON_ERROR(pcnt_unit_clear_count(s_pcnt_unit), TAG, "pcnt_clear");
    ESP_RETURN_ON_ERROR(pcnt_unit_start(s_pcnt_unit),     TAG, "pcnt_start");

    ESP_LOGI(TAG, "PCNT ready (CLK=IO%d DT=IO%d, %d counts/detent)",
             PIN_ENC_CLK, PIN_ENC_DT, COUNTS_PER_DETENT);
    return ESP_OK;
}

// ── Button GPIO init ──────────────────────────────────────────────────────────

static esp_err_t btn_gpio_init(void)
{
    gpio_config_t io_cfg = {
        .pin_bit_mask = 1ULL << PIN_ENC_SW,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,    // active-low switch, no ext resistor
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,     // trigger on press (falling edge)
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io_cfg), TAG, "gpio_config SW");

    // Debounce timer — one-shot, not auto-reload
    const esp_timer_create_args_t timer_cfg = {
        .callback = debounce_expired,
        .arg      = NULL,
        .name     = "enc_debounce",
        .skip_unhandled_events = true,
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer_cfg, &s_debounce_timer),
                        TAG, "esp_timer_create");

    // gpio_install_isr_service returns ESP_ERR_INVALID_STATE if already installed
    // (e.g. by another driver).  That is fine — just reuse the existing service.
    esp_err_t isr_ret = gpio_install_isr_service(0);
    if (isr_ret != ESP_OK && isr_ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "gpio_install_isr_service failed: 0x%x", isr_ret);
        return isr_ret;
    }
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(PIN_ENC_SW, btn_isr, NULL),
                        TAG, "isr_handler_add");

    ESP_LOGI(TAG, "Button ready (SW=IO%d, debounce=%dms)", PIN_ENC_SW, DEBOUNCE_MS);
    return ESP_OK;
}

// ── Polling task ──────────────────────────────────────────────────────────────

static void encoder_task(void *arg)
{
    int last_raw  = 0;
    int remainder = 0;     // sub-detent accumulator

    for (;;) {
        int raw = 0;
        pcnt_unit_get_count(s_pcnt_unit, &raw);

        int delta = raw - last_raw;
        last_raw  = raw;
        remainder += delta;

        // Fire one event per detent (accumulate fractional counts across ticks)
        while (remainder >= COUNTS_PER_DETENT) {
            remainder -= COUNTS_PER_DETENT;
            if (s_cb) s_cb(ENCODER_EVENT_CW, s_ctx);
        }
        while (remainder <= -COUNTS_PER_DETENT) {
            remainder += COUNTS_PER_DETENT;
            if (s_cb) s_cb(ENCODER_EVENT_CCW, s_ctx);
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ── Public API ────────────────────────────────────────────────────────────────

esp_err_t encoder_init(encoder_cb_t cb, void *ctx)
{
    s_cb  = cb;
    s_ctx = ctx;

    ESP_RETURN_ON_ERROR(pcnt_encoder_init(), TAG, "pcnt_init");
    ESP_RETURN_ON_ERROR(btn_gpio_init(),     TAG, "btn_init");

    xTaskCreatePinnedToCore(
        encoder_task,
        "encoder",
        2048,   // stack: rotation polling is lightweight
        NULL,
        4,      // priority: below LVGL (5), above idle
        NULL,
        0       // core 0 — keeps LVGL core 1 unaffected
    );

    ESP_LOGI(TAG, "Encoder driver started");
    return ESP_OK;
}
