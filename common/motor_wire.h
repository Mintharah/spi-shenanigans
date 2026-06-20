/*
 * motor_wire.h
 * ----------------------------------------------------------------------------
 * SHARED wire contract: the exact byte layout that crosses the STM32 <-> Pi SPI
 * link. Compiled into BOTH binaries (STM32 firmware and the QNX controller), so
 * this is the one piece of source that must be identical on both sides. Keep it
 * in a shared location (monorepo common/ dir, or a git submodule) -- never two
 * hand-edited copies.
 *
 * Pure data layout only: no atomics, no shared memory, no OS calls. Safe to
 * compile on the bare-metal STM32. Assumes a C11 compiler (for _Static_assert);
 * on a C99 firmware build, swap the _Static_assert lines for an equivalent
 * compile-time-assert macro.
 *
 * NOT here yet: command/config frames (SET_CONFIG, config_payload_t, ACK/NACK).
 * They belong in THIS file when added -- see the project checklist (Config/OTA).
 * ----------------------------------------------------------------------------
 */
#ifndef MOTOR_WIRE_H
#define MOTOR_WIRE_H

#include <stdint.h>
#include <stddef.h>

/* ============================ contract version ============================ */
/* Bump whenever ANY wire/shm struct layout changes (esp. motor_row_t). Stamped
 * into every frame header and into the shm region; the producer and every
 * consumer compare it and refuse to run on a mismatch.                         */
#define MOTOR_CONTRACT_VERSION   1u

/* ============================ wire framing ================================ */
#define MOTOR_FRAME_MAGIC        0x4D4F5452u  /* "MOTR" little-endian resync marker */

/* Generous compile-time maximum -- FIXED. The runtime block size is a config
 * parameter and MUST be <= MOTOR_MAX_ROWS_PER_BLOCK. Buffers are sized to the
 * max so a config change never requires recompilation.                        */
#define MOTOR_MAX_ROWS_PER_BLOCK 512u   /* default runtime block = 200 rows -> ~100 Hz @ 20 kHz */

/* ---- the per-timestep "wide row" -----------------------------------------
 *  >>> PROVISIONAL <<<  This is the ONLY sensor-dependent type in the contract.
 *  Below are your draft columns with conservative raw-count types (uint16 for
 *  ADC-class channels, int16 for signed accelerometer axes). Raw counts go on
 *  the wire; scaling to engineering units is applied downstream from the config
 *  constants. When the sensor list is locked: edit these fields, update the
 *  static_assert, and bump MOTOR_CONTRACT_VERSION.
 */
#pragma pack(push, 1)
typedef struct {
    uint16_t current;   /* analog / ADC, truly sampled per row @ 20 kHz            */
    int16_t  vib_x;     /* MPU6050 over I2C, zero-order-held (~1 kHz native)        */
    int16_t  vib_y;
    int16_t  vib_z;
    int16_t  temp;      /* TBD: interface + rate                                   */
    uint16_t rpm;       /* TBD: acquisition (timer input-capture / pulse count?)   */
    uint16_t voltage;   /* TBD: ADC via divider?                                   */
} motor_row_t;
#pragma pack(pop)

_Static_assert(sizeof(motor_row_t) == 14,
               "motor_row_t layout changed: update this size and bump MOTOR_CONTRACT_VERSION");

/* ---- frame header (prepended to every block on the wire) ------------------ */
#pragma pack(push, 1)
typedef struct {
    uint32_t magic;      /* == MOTOR_FRAME_MAGIC (resync after a desync)          */
    uint32_t seq;        /* monotonic frame counter; gaps => dropped blocks       */
    uint64_t timestamp;  /* STM32 timebase, microseconds (epoch/units TBD in fw)  */
    uint16_t version;    /* == MOTOR_CONTRACT_VERSION                             */
    uint16_t flags;      /* reserved bitfield (e.g. ZOH-stale, overrun)           */
    uint16_t n_rows;     /* valid rows this frame; <= MOTOR_MAX_ROWS_PER_BLOCK    */
    uint16_t _reserved;
} frame_header_t;
#pragma pack(pop)

_Static_assert(sizeof(frame_header_t) == 24, "frame_header_t layout changed");

/* CRC covers exactly [frame_header_t][n_rows * motor_row_t]. Exact polynomial/
 * parameters are still TBD -- they must be pinned to match the STM32 source
 * (hardware CRC unit vs. a software CRC). Both sides MUST agree byte-for-byte. */
typedef uint32_t frame_crc_t;

/* On-wire frame (variable length):
 *     [ frame_header_t ][ motor_row_t rows[n_rows] ][ frame_crc_t ]
 * Worst-case size, for fixed RX buffer allocation:                            */
#define MOTOR_MAX_FRAME_BYTES \
    (sizeof(frame_header_t) + (size_t)MOTOR_MAX_ROWS_PER_BLOCK * sizeof(motor_row_t) + sizeof(frame_crc_t))

#endif /* MOTOR_WIRE_H */
