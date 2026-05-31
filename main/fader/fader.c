/**
 * fader.c — Motorized fader driver (read + TB6612 motor control)
 *
 * ── Read pipeline ─────────────────────────────────────────────────────────────
 *   ADC (50 ms) → 4-sample moving average → dead-zone filter → brightness 0-255
 *   → UI update immediately → 400 ms settle timer → wled_cmd_set_brightness()
 *
 * ── Motor pipeline ────────────────────────────────────────────────────────────
 *   fader_motor_move_to(target) is called by the poll task whenever the
 *   software brightness changes (e.g. from a web app or another controller).
 *   A separate motor_task runs at 20 ms intervals, reads the current ADC
 *   position, and drives AIN1/AIN2 until the fader is within MOTOR_DEADBAND
 *   of the target, then brakes.
 *
 *   Safety rules — enforced unconditionally before every GPIO write:
 *   1. AIN1 and AIN2 are NEVER both HIGH simultaneously (short circuit).
 *   2. Motor stops immediately when position ≤ MOTOR_END_STOP_LOW or
 *      ≥ MOTOR_END_STOP_HIGH (raw ADC) to prevent stalling at the tracks.
 *   3. If user physically moves the fader (ADC delta > FADER_DEAD_ZONE while
 *      motor is supposedly idle), the motor target is cancelled — user wins.
 *
 * ── TB6612 truth table (STBY=HIGH, PWMA=HIGH = full speed) ───────────────────
 *   AIN1=H, AIN2=L → drive toward top     (brightness up)
 *   AIN1=L, AIN2=H → drive toward bottom  (brightness down)
 *   AIN1=L, AIN2=L → coast
 *   AIN1=H, AIN2=H → brake (holds position)
 */

#include "fader/fader.h"
#include "cmd/wled_cmd.h"
#include "poll/wled_poll.h"
#include "ui.h"
#include "board/board.h"
#include "esp_check.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>

static const char *TAG = "fader";

// ── Tuning ────────────────────────────────────────────────────────────────────
#define FADER_SAMPLE_MS        50    // ADC read interval (ms)
#define FADER_AVG_SAMPLES       4    // moving average window (samples)
#define FADER_DEAD_ZONE        24    // raw ADC counts — ignore wiper jitter
#define FADER_SETTLE_MS       400    // stillness before HTTP POST (ms)

// Calibrate these: slide fader fully down/up, read LOGD "raw=" values.
#define FADER_RAW_MIN          80    // raw ADC at physical bottom
#define FADER_RAW_MAX        3980    // raw ADC at physical top

// Motor end-stops: stop before the fader jams against the track ends.
// Set slightly inside RAW_MIN/MAX so the motor cuts out before stalling.
#define MOTOR_END_STOP_LOW    (FADER_RAW_MIN + 60)
#define MOTOR_END_STOP_HIGH   (FADER_RAW_MAX - 60)

// How close (in raw ADC counts) the motor needs to get before it brakes.
// ~30 counts ≈ ~0.8% of full range — tight but avoids oscillation.
#define MOTOR_DEADBAND         30

// Motor task interval (ms). Faster = more responsive, more ADC reads.
#define MOTOR_TASK_MS          20

// ── State ─────────────────────────────────────────────────────────────────────
static adc_oneshot_unit_handle_t s_adc_handle   = NULL;
static esp_timer_handle_t        s_settle_timer;
static volatile uint8_t          s_last_bri      = 0;
static volatile bool             s_fader_active  = false;

// Motor target: -1 means no active move, 0-255 is a pending target
static volatile int              s_motor_target  = -1;

// Moving average
static int  s_avg_buf[FADER_AVG_SAMPLES] = {0};
static int  s_avg_idx  = 0;
static bool s_avg_full = false;

// ── ADC ───────────────────────────────────────────────────────────────────────
static int adc_read_raw(void)
{
    int raw = 0;
    if (adc_oneshot_read(s_adc_handle, PIN_FADER_ADC_CHANNEL, &raw) != ESP_OK)
        return -1;
    return raw;
}

static int moving_average(int sample)
{
    s_avg_buf[s_avg_idx] = sample;
    s_avg_idx = (s_avg_idx + 1) % FADER_AVG_SAMPLES;
    if (s_avg_idx == 0) s_avg_full = true;
    int count = s_avg_full ? FADER_AVG_SAMPLES : s_avg_idx;
    int sum = 0;
    for (int i = 0; i < count; i++) sum += s_avg_buf[i];
    return sum / count;
}

static uint8_t raw_to_brightness(int raw)
{
    if (raw <= FADER_RAW_MIN) return 0;
    if (raw >= FADER_RAW_MAX) return 255;
    return (uint8_t)(((raw - FADER_RAW_MIN) * 255) / (FADER_RAW_MAX - FADER_RAW_MIN));
}

static int brightness_to_raw(uint8_t bri)
{
    return FADER_RAW_MIN + ((int)bri * (FADER_RAW_MAX - FADER_RAW_MIN)) / 255;
}

// ── Motor GPIO ────────────────────────────────────────────────────────────────
// ALL motor output changes go through these two functions.
// motor_drive() contains the hard safety check — AIN1 and AIN2 are
// never both HIGH.  motor_brake() and motor_coast() are safe aliases.

static void motor_drive(bool ain1, bool ain2)
{
    // Safety: must not set both HIGH
    if (ain1 && ain2) {
        ESP_LOGE(TAG, "BUG: attempted to set AIN1=AIN2=HIGH — coasting instead");
        ain1 = false;
        ain2 = false;
    }
    gpio_set_level(PIN_FADER_MOTOR_AIN1, ain1 ? 1 : 0);
    gpio_set_level(PIN_FADER_MOTOR_AIN2, ain2 ? 1 : 0);
}

static void motor_brake(void)
{
    // AIN1=L, AIN2=L → TB6612 coast (safe stop, no back-EMF fight)
    // Use coast rather than active brake so the user can still push the fader
    gpio_set_level(PIN_FADER_MOTOR_AIN1, 0);
    gpio_set_level(PIN_FADER_MOTOR_AIN2, 0);
}

// ── Settle timer (fires when fader stops moving) ──────────────────────────────
static void on_settle(void *arg)
{
    uint8_t bri = s_last_bri;
    ESP_LOGI(TAG, "Fader settled → bri=%d (%d%%)", bri, (bri * 100) / 255);
    wled_cmd_set_brightness(bri);
    wled_poll_set_paused(false);
    s_fader_active = false;
}

// ── Motor task ────────────────────────────────────────────────────────────────
static void motor_task(void *arg)
{
    ESP_LOGI(TAG, "Motor task started");

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(MOTOR_TASK_MS));

        int target_bri = s_motor_target;
        if (target_bri < 0) continue;   // no pending move

        int raw = adc_read_raw();
        if (raw < 0) continue;

        // If user is physically moving the fader, cancel motor move
        if (s_fader_active) {
            ESP_LOGD(TAG, "User moving fader — cancelling motor target");
            s_motor_target = -1;
            motor_brake();
            continue;
        }

        int target_raw = brightness_to_raw((uint8_t)target_bri);
        int error      = target_raw - raw;

        // Check end-stops before driving
        if (raw <= MOTOR_END_STOP_LOW && error < 0) {
            ESP_LOGD(TAG, "End-stop LOW hit, stopping");
            s_motor_target = -1;
            motor_brake();
            continue;
        }
        if (raw >= MOTOR_END_STOP_HIGH && error > 0) {
            ESP_LOGD(TAG, "End-stop HIGH hit, stopping");
            s_motor_target = -1;
            motor_brake();
            continue;
        }

        if (abs(error) <= MOTOR_DEADBAND) {
            // Close enough — brake and clear target
            motor_brake();
            s_motor_target = -1;
            ESP_LOGD(TAG, "Motor reached target (raw=%d target_raw=%d)", raw, target_raw);
        } else if (error > 0) {
            // Need to go higher
            motor_drive(true, false);   // AIN1=H, AIN2=L
        } else {
            // Need to go lower
            motor_drive(false, true);   // AIN1=L, AIN2=H
        }
    }
}

// ── Fader read task ───────────────────────────────────────────────────────────
static void fader_task(void *arg)
{
    ESP_LOGI(TAG, "Fader read task started (sample=%d ms, settle=%d ms)",
             FADER_SAMPLE_MS, FADER_SETTLE_MS);

    int last_averaged = -1;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(FADER_SAMPLE_MS));

        int raw = adc_read_raw();
        if (raw < 0) continue;

        int averaged = moving_average(raw);
        ESP_LOGD(TAG, "raw=%d avg=%d", raw, averaged);

        if (last_averaged >= 0 &&
            abs(averaged - last_averaged) < FADER_DEAD_ZONE) {
            continue;
        }

        last_averaged     = averaged;
        uint8_t bri       = raw_to_brightness(averaged);
        s_last_bri        = bri;

        if (!s_fader_active) {
            wled_poll_set_paused(true);
            s_fader_active = true;
            // Cancel any motor move — user is touching the fader
            s_motor_target = -1;
        }

        lvgl_lock();
        ui_set_brightness(bri);
        lvgl_unlock();

        esp_timer_stop(s_settle_timer);
        esp_timer_start_once(s_settle_timer, FADER_SETTLE_MS * 1000ULL);
    }
}

// ── Public API ────────────────────────────────────────────────────────────────

esp_err_t fader_init(void)
{
    // ── ADC ───────────────────────────────────────────────────────────────────
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id  = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&unit_cfg, &s_adc_handle),
                        TAG, "adc_oneshot_new_unit");

    adc_oneshot_chan_cfg_t chan_cfg = {
        .bitwidth = ADC_BITWIDTH_12,
        .atten    = ADC_ATTEN_DB_12,
    };
    ESP_RETURN_ON_ERROR(
        adc_oneshot_config_channel(s_adc_handle, PIN_FADER_ADC_CHANNEL, &chan_cfg),
        TAG, "adc_config_channel");

    // ── Motor GPIO ────────────────────────────────────────────────────────────
    // Configure all four TB6612 control pins as push-pull outputs
    gpio_config_t io_cfg = {
        .pin_bit_mask = (1ULL << PIN_FADER_MOTOR_PWMA) |
                        (1ULL << PIN_FADER_MOTOR_AIN1) |
                        (1ULL << PIN_FADER_MOTOR_AIN2) |
                        (1ULL << PIN_FADER_MOTOR_STBY),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io_cfg), TAG, "gpio_config motor");

    // Safe initial state: coast, then enable STBY and PWMA
    gpio_set_level(PIN_FADER_MOTOR_AIN1, 0);
    gpio_set_level(PIN_FADER_MOTOR_AIN2, 0);
    gpio_set_level(PIN_FADER_MOTOR_PWMA, 1);   // full speed (PWM later)
    gpio_set_level(PIN_FADER_MOTOR_STBY, 1);   // chip active (not standby)

    // ── Settle timer ──────────────────────────────────────────────────────────
    const esp_timer_create_args_t timer_cfg = {
        .callback              = on_settle,
        .arg                   = NULL,
        .name                  = "fader_settle",
        .skip_unhandled_events = true,
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer_cfg, &s_settle_timer),
                        TAG, "esp_timer_create");

    // ── Tasks ─────────────────────────────────────────────────────────────────
    BaseType_t ok;

    ok = xTaskCreatePinnedToCore(fader_task, "fader_read", 3072, NULL, 2, NULL, 0);
    if (ok != pdPASS) { ESP_LOGE(TAG, "fader_read task failed"); return ESP_FAIL; }

    ok = xTaskCreatePinnedToCore(motor_task, "fader_motor", 2048, NULL, 3, NULL, 0);
    if (ok != pdPASS) { ESP_LOGE(TAG, "fader_motor task failed"); return ESP_FAIL; }

    ESP_LOGI(TAG, "Fader init OK — ADC GPIO%d  PWMA=IO%d  AIN1=IO%d  AIN2=IO%d  STBY=IO%d",
             PIN_FADER_WIPER,
             PIN_FADER_MOTOR_PWMA, PIN_FADER_MOTOR_AIN1,
             PIN_FADER_MOTOR_AIN2, PIN_FADER_MOTOR_STBY);
    return ESP_OK;
}

uint8_t fader_get_brightness(void) { return s_last_bri; }

void fader_motor_move_to(uint8_t target_brightness)
{
    // Don't interrupt a user touch
    if (s_fader_active) return;
    s_motor_target = target_brightness;
    ESP_LOGD(TAG, "Motor target set: %d", target_brightness);
}

void fader_motor_stop(void)
{
    s_motor_target = -1;
    motor_brake();
}