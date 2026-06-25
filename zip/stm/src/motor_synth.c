/*
 * motor_synth.c  (timer-interrupt version, instrumented)
 * ----------------------------------------------------------------------------
 * Synthetic data source for sensor-free bring-up. Produces one block of sine
 * samples every ~10 ms (100 Hz block cadence -- the same rate the real 20 kHz /
 * 200-row path yields) and hands it to motor_on_block_ready().
 *
 * Why a timer INTERRUPT, not timer-triggered DMA: the earlier TIM2_UP -> DMA
 * approach never delivered transfers (the update-DMA request never advanced the
 * stream), so no callback ever fired. A periodic TIM update interrupt is the
 * most reliable STM32 mechanism and exercises the entire downstream path (frame
 * assembly, SPI2-slave DMA, the PB0 handshake, the whole Pi side) identically --
 * the only thing it doesn't model is per-sample ADC DMA, which is motor_acquire's
 * job later. For pushing frames to the Pi, the block cadence is what matters.
 *
 * A precomputed sine table (2*block_rows) is fed half-at-a-time, alternating each
 * block, so `current` traces a continuous sine across blocks on the monitor.
 *
 * g_synth_cb bumps every block (read by the diagnostic main.c): if it never
 * changes, the timer interrupt isn't firing.
 *
 * Owns TIM2. Use EITHER this OR motor_acquire.c, never both.
 * ----------------------------------------------------------------------------
 */
#include "stm32f4xx_hal.h"
#include <math.h>
#include "motor_wire.h"
#include "motor_source.h"

extern void Error_Handler(void);

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef MOTOR_DEFAULT_BLOCK_ROWS
#define MOTOR_DEFAULT_BLOCK_ROWS  200u
#endif
#ifndef SYNTH_CYCLES
#define SYNTH_CYCLES  4u
#endif

/* ---- module state -------------------------------------------------------- */
static TIM_HandleTypeDef s_htim2;

static uint16_t s_block_rows = MOTOR_DEFAULT_BLOCK_ROWS;
static uint16_t s_pattern[2u * MOTOR_MAX_ROWS_PER_BLOCK];   /* sine table */
static volatile uint8_t s_half = 0;                          /* which half to send */

volatile uint32_t g_synth_cb = 0;   /* diagnostic: blocks produced */

/* ---- pattern ------------------------------------------------------------- */
static void build_pattern(uint16_t rows)
{
    uint32_t len = (uint32_t)rows * 2u;
    for (uint32_t i = 0; i < len; ++i) {
        float ph = 2.0f * (float)M_PI * (float)SYNTH_CYCLES * (float)i / (float)len;
        s_pattern[i] = (uint16_t)(2048.0f + 1800.0f * sinf(ph));
    }
}

/* ---- init ---------------------------------------------------------------- */
static void tim2_init(void)
{
    __HAL_RCC_TIM2_CLK_ENABLE();
    s_htim2.Instance               = TIM2;
    /* TIM2 clock = 84 MHz. 84e6 / (8400 * 100) = 100 Hz block cadence. */
    s_htim2.Init.Prescaler         = 8399;
    s_htim2.Init.CounterMode       = TIM_COUNTERMODE_UP;
    s_htim2.Init.Period            = 99;
    s_htim2.Init.ClockDivision     = TIM_CLOCKDIVISION_DIV1;
    s_htim2.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
    if (HAL_TIM_Base_Init(&s_htim2) != HAL_OK) Error_Handler();

    /* Producer runs in this ISR; keep it at the same priority as the SPI/DMA
     * completion ISRs so they never preempt each other and corrupt the
     * s_tx_idx / s_pending handshake state in motor_send.c. */
    HAL_NVIC_SetPriority(TIM2_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(TIM2_IRQn);
}

void motor_synth_init(uint16_t block_rows)
{
    if (block_rows == 0 || block_rows > MOTOR_MAX_ROWS_PER_BLOCK)
        block_rows = MOTOR_DEFAULT_BLOCK_ROWS;
    s_block_rows = block_rows;

    build_pattern(s_block_rows);
    tim2_init();
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

/* ---- timer update callback (block cadence) ------------------------------- */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance != TIM2) return;

    g_synth_cb++;
    const uint16_t *block = &s_pattern[(uint32_t)s_half * s_block_rows];
    s_half ^= 1u;                              /* alternate halves -> moving sine */
    motor_on_block_ready(block, s_block_rows); /* runs the send side */
}

/* ---- ISR plumbing -------------------------------------------------------- */
void TIM2_IRQHandler(void)
{
    HAL_TIM_IRQHandler(&s_htim2);
}