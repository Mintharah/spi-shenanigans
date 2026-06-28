/*
 * motor_synth.c  (timer-interrupt version, instrumented + runtime-reconfig)
 * ----------------------------------------------------------------------------
 * Synthetic data source for sensor-free bring-up. Produces one block of sine
 * samples every block_period_us microseconds (default 10000 us = 100 Hz block
 * cadence) and hands it to motor_on_block_ready().
 *
 * Why a timer INTERRUPT, not timer-triggered DMA: the earlier TIM2_UP -> DMA
 * approach never delivered transfers (the update-DMA request never advanced the
 * stream), so no callback ever fired. A periodic TIM update interrupt is the
 * most reliable STM32 mechanism and exercises the entire downstream path (frame
 * assembly, SPI2-slave DMA, the PB0 handshake, the whole Pi side) identically.
 *
 * A precomputed sine table (2*block_rows) is fed half-at-a-time, alternating
 * each block, so `current` traces a continuous sine across blocks on the
 * monitor.
 *
 * Runtime reconfig: motor_synth_set_block_rows() / set_synth_cycles() rebuild
 * the pattern; set_period_us() reprograms TIM2's prescaler+period. All three
 * must be called with the synth STOPPED (no TIM2 IRQ in flight).
 *
 * g_synth_cb bumps every block (read by the diagnostic main.c).
 * Owns TIM2. Use EITHER this OR motor_acquire.c, never both.
 * ----------------------------------------------------------------------------
 */
#include "stm32f4xx_hal.h"
#include <math.h>
#include "motor_wire.h"
#include "motor_source.h"
#include "motor_synth.h"

extern void Error_Handler(void);

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef MOTOR_DEFAULT_BLOCK_ROWS
#define MOTOR_DEFAULT_BLOCK_ROWS  200u
#endif
#ifndef SYNTH_DEFAULT_CYCLES
#define SYNTH_DEFAULT_CYCLES  4u
#endif
#ifndef SYNTH_DEFAULT_PERIOD_US
#define SYNTH_DEFAULT_PERIOD_US  10000u    /* 100 Hz block cadence */
#endif

/* TIM2 input clock on F401 is 84 MHz (APB1 timers x2). */
#define TIM2_CLOCK_HZ  84000000u

/* ---- module state -------------------------------------------------------- */
static TIM_HandleTypeDef s_htim2;

static uint16_t s_block_rows   = MOTOR_DEFAULT_BLOCK_ROWS;
static uint16_t s_synth_cycles = SYNTH_DEFAULT_CYCLES;
static uint16_t s_pattern[2u * MOTOR_MAX_ROWS_PER_BLOCK];
static volatile uint8_t s_half = 0;

volatile uint32_t g_synth_cb = 0;

/* ---- pattern ------------------------------------------------------------- */
static void build_pattern(uint16_t rows, uint16_t cycles)
{
    uint32_t len = (uint32_t)rows * 2u;
    for (uint32_t i = 0; i < len; ++i) {
        float ph = 2.0f * (float)M_PI * (float)cycles * (float)i / (float)len;
        s_pattern[i] = (uint16_t)(2048.0f + 1800.0f * sinf(ph));
    }
}

/* ---- TIM2 program -------------------------------------------------------- */
/* Pick a (prescaler, period) pair that gives the requested microsecond
 * cadence with a 16-bit period. Strategy: choose the smallest prescaler such
 * that the period counter fits in 16 bits.                                   */
static void tim2_program(uint32_t period_us)
{
    if (period_us == 0) period_us = SYNTH_DEFAULT_PERIOD_US;

    /* total ticks at 84 MHz: */
    uint64_t total = (uint64_t)TIM2_CLOCK_HZ * period_us / 1000000ull;
    if (total == 0) total = 1;

    uint32_t presc = 1;
    while ((total + presc - 1) / presc > 0x10000u) {
        presc <<= 1;
        if (presc > 0x10000u) { presc = 0x10000u; break; }
    }
    uint32_t arr = (uint32_t)((total + presc - 1) / presc);
    if (arr == 0) arr = 1;
    if (arr > 0x10000u) arr = 0x10000u;

    s_htim2.Init.Prescaler         = (uint32_t)(presc - 1u);
    s_htim2.Init.CounterMode       = TIM_COUNTERMODE_UP;
    s_htim2.Init.Period            = (uint32_t)(arr - 1u);
    s_htim2.Init.ClockDivision     = TIM_CLOCKDIVISION_DIV1;
    s_htim2.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
    if (HAL_TIM_Base_Init(&s_htim2) != HAL_OK) Error_Handler();
}

static void tim2_init(uint32_t period_us)
{
    __HAL_RCC_TIM2_CLK_ENABLE();
    s_htim2.Instance = TIM2;
    tim2_program(period_us);

    /* Producer runs in this ISR; keep it at the same priority as the SPI/DMA
     * completion ISRs so they never preempt each other and corrupt the
     * s_tx_idx / s_pending handshake state in motor_send.c. */
    HAL_NVIC_SetPriority(TIM2_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(TIM2_IRQn);
}

/* ---- public ------------------------------------------------------------- */
void motor_synth_init(uint16_t block_rows)
{
    if (block_rows == 0 || block_rows > MOTOR_MAX_ROWS_PER_BLOCK)
        block_rows = MOTOR_DEFAULT_BLOCK_ROWS;
    s_block_rows   = block_rows;
    s_synth_cycles = SYNTH_DEFAULT_CYCLES;

    /* Build once for MAX rows so runtime block_rows changes never require a
     * rebuild -- any block_rows up to MOTOR_MAX_ROWS_PER_BLOCK reads from
     * initialized memory. Pattern frequency content depends on synth_cycles
     * relative to the pattern length (2*MAX), not on the runtime block_rows. */
    build_pattern(MOTOR_MAX_ROWS_PER_BLOCK, s_synth_cycles);
    tim2_init(SYNTH_DEFAULT_PERIOD_US);
}

void motor_synth_start(void)
{
    s_half = 0;
    if (HAL_TIM_Base_Start_IT(&s_htim2) != HAL_OK)
        Error_Handler();
}

void motor_synth_stop(void)
{
    HAL_TIM_Base_Stop_IT(&s_htim2);
}

/* Deferred work flags set by the apply path (which runs inside the TIM2 ISR)
 * and serviced by motor_synth_service() from the main loop. volatile because
 * they cross ISR <-> main-loop. s_pending_period_us == 0 means "no change". */
static volatile uint8_t  s_rebuild_pending   = 0;
static volatile uint32_t s_pending_period_us = 0;

void motor_synth_set_block_rows(uint16_t block_rows)
{
    if (block_rows == 0 || block_rows > MOTOR_MAX_ROWS_PER_BLOCK) return;
    /* The pattern table is sized to 2*MAX and pre-built at init, so any
     * block_rows up to MAX reads from initialized memory. No rebuild needed,
     * no ISR-time cost beyond a 16-bit store.                                */
    s_block_rows = block_rows;
    s_half = 0;
}

void motor_synth_set_synth_cycles(uint16_t synth_cycles)
{
    if (synth_cycles == 0 || synth_cycles > 64) return;
    /* Cycles changes need a pattern rebuild (~500 us at MAX); too slow for
     * the ISR. Just record the new value and flag a deferred rebuild --
     * motor_synth_service() will pick it up. Until then, the pattern stays
     * coherent at the previous cycle count.                                  */
    s_synth_cycles = synth_cycles;
    s_rebuild_pending = 1;
}

void motor_synth_set_period_us(uint32_t period_us)
{
    if (period_us == 0) return;       /* "leave alone" sentinel */
    /* tim2_program() writes EGR=UG which fires the TIM2 update IRQ; doing
     * that from INSIDE the TIM2 ISR is the cause of the apply-path stalls
     * on tight clocks. Defer to motor_synth_service() instead.              */
    s_pending_period_us = period_us;
}

void motor_synth_service(void)
{
    uint32_t p = s_pending_period_us;
    if (p != 0u) {
        s_pending_period_us = 0u;
        tim2_program(p);
    }
    if (s_rebuild_pending) {
        s_rebuild_pending = 0;
        build_pattern(MOTOR_MAX_ROWS_PER_BLOCK, s_synth_cycles);
    }
}

/* ---- timer update callback (block cadence) ------------------------------- */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance != TIM2) return;

    g_synth_cb++;
    const uint16_t *block = &s_pattern[(uint32_t)s_half * s_block_rows];
    s_half ^= 1u;
    motor_on_block_ready(block, s_block_rows);
}

void TIM2_IRQHandler(void)
{
    HAL_TIM_IRQHandler(&s_htim2);
}