/*
 * motor_synth.h
 * ----------------------------------------------------------------------------
 * Synthetic 20 kHz source -- drop-in alternative to motor_acquire for sensor-free
 * bring-up. Same TIM2 + DMA + ping-pong path, fed from a sine table instead of
 * the ADC. Drives the same motor_on_block_ready() hook (see motor_acquire.h).
 * Use EITHER this OR motor_acquire.c, never both (both own TIM2).
 * ----------------------------------------------------------------------------
 */
#ifndef MOTOR_SYNTH_H
#define MOTOR_SYNTH_H

#include <stdint.h>

void motor_synth_init(uint16_t block_rows);
void motor_synth_start(void);
void motor_synth_stop(void);

#endif /* MOTOR_SYNTH_H */
