/*
 * motor_source.h
 * ----------------------------------------------------------------------------
 * The contract between a data SOURCE and its CONSUMER on the STM32.
 *
 *   A source -- motor_acquire (real ADC) or motor_synth (dummy data) -- produces
 *   one block of current samples at a time and calls motor_on_block_ready().
 *   The consumer (the frame-assembly / send side) implements that function: it
 *   wraps the block into a wire frame and pushes it out over SPI.
 *
 * Keeping the hook here makes the two sources independent of each other --
 * synth does not depend on acquire, and vice versa.
 * ----------------------------------------------------------------------------
 */
#ifndef MOTOR_SOURCE_H
#define MOTOR_SOURCE_H

#include <stdint.h>

/* Called from DMA-ISR context whenever one block of `n_rows` contiguous uint16
 * current samples is full and stable. Implemented by the send side.
 * KEEP IT SHORT -- it runs in interrupt context. */
void motor_on_block_ready(const uint16_t *samples, uint16_t n_rows);

#endif /* MOTOR_SOURCE_H */
