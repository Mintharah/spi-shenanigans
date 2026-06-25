/*
 * motor_send.h
 * ----------------------------------------------------------------------------
 * STM32 send side (the consumer): turns each block into a wire frame and ships
 * it to the Pi over SPI. Implements motor_on_block_ready() (from motor_source.h),
 * so linking this satisfies the hook that motor_synth / motor_acquire call.
 * ----------------------------------------------------------------------------
 */
#ifndef MOTOR_SEND_H
#define MOTOR_SEND_H

#include <stdint.h>

/* Configure SPI2 (slave), the data-ready GPIO, and the CRC.
 * block_rows MUST match the source and the Pi controller (default 200). */
void motor_send_init(uint16_t block_rows);

/* Optional diagnostics. */
void motor_send_get_stats(uint32_t *frames_sent, uint32_t *frames_skipped);

#endif /* MOTOR_SEND_H */
