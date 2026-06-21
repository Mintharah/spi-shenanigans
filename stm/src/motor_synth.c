/*
 * motor_synth.c
 * ----------------------------------------------------------------------------
 * Synthetic 20 kHz data source for sensor-free bring-up. A drop-in alternative
 * to motor_acquire.c: it drives the SAME hook (motor_on_block_ready) at the SAME
 * real cadence, so everything downstream (frame assembly, SPI, the whole Pi side)
 * runs identically -- the only difference is the samples come from a sine table
 * instead of the ADC.
 *
 *   TIM2 update @ 20 kHz  ->  DMA request (TIM2_UP)  ->  DMA copies one sample
 *   from s_pattern[] into the circular ping-pong buffer s_buf[2*block_rows]:
 *       half-transfer IRQ  -> first half  is stable -> motor_on_block_ready()
 *       transfer-complete  -> second half is stable -> motor_on_block_ready()
 *
 * This is a true 20 kHz path: the timer, the DMA, and the block cadence are the
 * production ones, so it tests timing/load, not just data shape. The CPU is out
 * of the sample loop exactly as with the real ADC.
 *
 * USE EITHER motor_acquire.c OR motor_synth.c -- NOT BOTH. Both own TIM2 (and a
 * DMA stream); link only one. Swap by changing which .c you compile + which init
 * you call from main.
 * ----------------------------------------------------------------------------
 */
#include "stm32f4xx_hal.h"
#include <math.h>
#include "motor_wire.h"      /* MOTOR_MAX_ROWS_PER_BLOCK */
#include "motor_source.h"    /* motor_on_block_ready() -- source/consumer hook */

extern void Error_Handler(void);

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef MOTOR_DEFAULT_BLOCK_ROWS
#define MOTOR_DEFAULT_BLOCK_ROWS  200u
#endif

/* Synthetic signal: number of sine periods across the 2*block_rows table.
 * Signal freq = SYNTH_CYCLES * (20000 / (2*block_rows)). For 200 rows that's
 * SYNTH_CYCLES * 50 Hz -> 4 cycles = 200 Hz. */
#ifndef SYNTH_CYCLES
#define SYNTH_CYCLES  4u
#endif

/* ---- module state -------------------------------------------------------- */
static TIM_HandleTypeDef s_htim2;
static DMA_HandleTypeDef s_hdma_tim2_up;

static uint16_t s_block_rows = MOTOR_DEFAULT_BLOCK_ROWS;
static uint16_t s_pattern[2u * MOTOR_MAX_ROWS_PER_BLOCK];   /* DMA source (sine)  */
static uint16_t s_buf[2u * MOTOR_MAX_ROWS_PER_BLOCK];       /* DMA dest (ping-pong) */

/* ---- pattern ------------------------------------------------------------- */
static void build_pattern(uint16_t rows)
{
    uint32_t len = (uint32_t)rows * 2u;
    for (uint32_t i = 0; i < len; ++i) {
        float ph = 2.0f * (float)M_PI * (float)SYNTH_CYCLES * (float)i / (float)len;
        /* centered in the 12-bit ADC range, like an AC current reading */
        s_pattern[i] = (uint16_t)(2048.0f + 1800.0f * sinf(ph));
    }
    /* For exact-value transport checks instead of a waveform, replace the line
     * above with: s_pattern[i] = (uint16_t)i;  (a ramp you can assert on the Pi). */
}

/* ---- init ---------------------------------------------------------------- */
static void dma_init(void)
{
    __HAL_RCC_DMA1_CLK_ENABLE();
    s_hdma_tim2_up.Instance                 = DMA1_Stream1;
    s_hdma_tim2_up.Init.Channel             = DMA_CHANNEL_3;   /* TIM2_UP -> DMA1 S1 Ch3 */
    s_hdma_tim2_up.Init.Direction           = DMA_PERIPH_TO_MEMORY; /* "periph" = RAM table */
    s_hdma_tim2_up.Init.PeriphInc           = DMA_PINC_ENABLE; /* walk the pattern table */
    s_hdma_tim2_up.Init.MemInc              = DMA_MINC_ENABLE; /* walk the dest buffer   */
    s_hdma_tim2_up.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD;
    s_hdma_tim2_up.Init.MemDataAlignment    = DMA_MDATAALIGN_HALFWORD;
    s_hdma_tim2_up.Init.Mode                = DMA_CIRCULAR;
    s_hdma_tim2_up.Init.Priority            = DMA_PRIORITY_HIGH;
    s_hdma_tim2_up.Init.FIFOMode            = DMA_FIFOMODE_DISABLE;
    if (HAL_DMA_Init(&s_hdma_tim2_up) != HAL_OK) Error_Handler();

    s_hdma_tim2_up.XferHalfCpltCallback = NULL;  /* set in start (after we know which) */
    HAL_NVIC_SetPriority(DMA1_Stream1_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(DMA1_Stream1_IRQn);
}

static void tim2_init(void)
{
    __HAL_RCC_TIM2_CLK_ENABLE();
    s_htim2.Instance               = TIM2;
    s_htim2.Init.Prescaler         = 0;
    s_htim2.Init.CounterMode       = TIM_COUNTERMODE_UP;
    s_htim2.Init.Period            = 4199;      /* 84 MHz / 4200 = 20 kHz */
    s_htim2.Init.ClockDivision     = TIM_CLOCKDIVISION_DIV1;
    s_htim2.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
    if (HAL_TIM_Base_Init(&s_htim2) != HAL_OK) Error_Handler();
}

void motor_synth_init(uint16_t block_rows)
{
    if (block_rows == 0 || block_rows > MOTOR_MAX_ROWS_PER_BLOCK)
        block_rows = MOTOR_DEFAULT_BLOCK_ROWS;
    s_block_rows = block_rows;

    build_pattern(s_block_rows);
    dma_init();
    tim2_init();
}

/* ---- callbacks ----------------------------------------------------------- */
static void synth_half_cb(DMA_HandleTypeDef *h)
{
    (void)h;
    motor_on_block_ready(&s_buf[0], s_block_rows);            /* first half */
}

static void synth_full_cb(DMA_HandleTypeDef *h)
{
    (void)h;
    motor_on_block_ready(&s_buf[s_block_rows], s_block_rows); /* second half */
}

void motor_synth_start(void)
{
    s_hdma_tim2_up.XferHalfCpltCallback = synth_half_cb;
    s_hdma_tim2_up.XferCpltCallback     = synth_full_cb;

    if (HAL_DMA_Start_IT(&s_hdma_tim2_up, (uint32_t)s_pattern, (uint32_t)s_buf,
                         (uint32_t)s_block_rows * 2u) != HAL_OK)
        Error_Handler();

    __HAL_TIM_ENABLE_DMA(&s_htim2, TIM_DMA_UPDATE);  /* update event -> DMA request */
    if (HAL_TIM_Base_Start(&s_htim2) != HAL_OK)
        Error_Handler();
}

void motor_synth_stop(void)
{
    HAL_TIM_Base_Stop(&s_htim2);
    __HAL_TIM_DISABLE_DMA(&s_htim2, TIM_DMA_UPDATE);
    HAL_DMA_Abort_IT(&s_hdma_tim2_up);
}

/* ---- ISR plumbing -------------------------------------------------------- */
void DMA1_Stream1_IRQHandler(void)
{
    HAL_DMA_IRQHandler(&s_hdma_tim2_up);
}
