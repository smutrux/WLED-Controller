#pragma once
/**
 * fader.h — Motorized fader driver (read + TB6612 motor control)
 *
 * Wiring summary (see board.h for GPIO numbers):
 *   Fader pot:    pin3=GND  pin4=VCC3V3  pin5=IO9(ADC)
 *   Fader motor:  pin1=AO1  pin2=AO2
 *   TB6612 ctrl:  PWMA=IO15  AIN1=IO16  AIN2=IO17  STBY=IO21
 *   TB6612 power: VM=12V boost  MGND=common GND  VCC=3.3V
 */

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialise ADC, motor GPIOs, settle timer, and both tasks.
 * Call from app_main after wled_cmd_init() and ui_build().
 */
esp_err_t fader_init(void);

/** Last brightness value read from the fader (0-255). Thread-safe. */
uint8_t fader_get_brightness(void);

/**
 * Command the motor to move the fader to a target brightness position.
 * No-op if the user is currently touching the fader (user always wins).
 * Called by the poll task when software brightness changes externally.
 */
void fader_motor_move_to(uint8_t target_brightness);

/** Immediately stop the motor and clear any pending move target. */
void fader_motor_stop(void);

#ifdef __cplusplus
}
#endif