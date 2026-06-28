/*
 * motor_synth.h
 * ----------------------------------------------------------------------------
 * Synthetic 20 kHz source -- drop-in alternative to motor_acquire for sensor-free
 * bring-up. Same TIM2 + DMA + ping-pong path, fed from a sine table instead of
 * the ADC. Drives the same motor_on_block_ready() hook (see motor_source.h).
 * Use EITHER this OR motor_acquire.c, never both (both own TIM2).
 *
 * Runtime reconfig: motor_synth_set_block_rows() and motor_synth_set_period_us()
 * are safe to call ONLY when the synth is stopped (between motor_synth_stop()
 * and motor_synth_start()). The expected call order from the SET_CONFIG
 * handler in motor_send.c is:
 *
 *     motor_synth_stop();
 *     motor_synth_set_block_rows(new_rows);
 *     motor_synth_set_period_us(new_period);   // optional
 *     motor_synth_start();
 * ----------------------------------------------------------------------------
 */
#ifndef MOTOR_SYNTH_H
#define MOTOR_SYNTH_H

#include <stdint.h>

void motor_synth_init(uint16_t block_rows);
void motor_synth_start(void);
void motor_synth_stop(void);

/* Call periodically from main() (NOT from an ISR). Services deferred work
 * the apply path queued up -- currently the synth_cycles pattern rebuild.
 * Cheap when there's nothing to do (single volatile read).                  */
void motor_synth_service(void);

/* Runtime reconfig. set_block_rows is now cheap (variable update only); the
 * pattern table is pre-built for MAX rows at init. set_synth_cycles flags a
 * deferred rebuild that motor_synth_service() picks up from main().         */
void motor_synth_set_block_rows(uint16_t block_rows);
void motor_synth_set_synth_cycles(uint16_t synth_cycles);
void motor_synth_set_period_us(uint32_t period_us);

#endif /* MOTOR_SYNTH_H */