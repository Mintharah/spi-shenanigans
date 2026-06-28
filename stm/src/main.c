/*
 * main.c -- STM32F401CC DIAGNOSTIC bring-up build
 * ----------------------------------------------------------------------------
 * This build reports, on the onboard LED (PC13), how far the data pipeline gets.
 * The LED blinks a GROUP of N flashes, pauses ~1.5 s, and repeats. Count N:
 *
 *   1 flash  = main() runs, clock + LED OK, but synth DMA callbacks never fire
 *              -> TIM2 -> DMA chain is dead (motor_synth)
 *   2 flashes= synth callbacks fire, but motor_on_block_ready() never called
 *              -> source->consumer hook not linked / not invoked
 *   3 flashes= on_block_ready runs, but arm_tx() never entered
 *              -> state-machine bug (s_tx_idx not < 0 on first block)
 *   4 flashes= arm_tx() entered, but HAL_SPI_TransmitReceive_DMA never succeeds
 *              -> SPI-slave TX DMA won't arm (PB0 never rises)        [SEND BUG]
 *   5 flashes= PB0 raised, but no SPI transfer ever completes
 *              -> the Pi isn't clocking the frame / handshake issue
 *   6 flashes= a frame completed on the STM32 side -> FULL SUCCESS (check Pi ok=)
 *
 *   FAST FLUTTER (continuous rapid blink, no grouping) = an init call trapped in
 *   Error_Handler() -> one of the *_init() faulted.
 *
 * Flash this, watch the LED, report the number. Revert to the plain main() once
 * we know the stage.
 *
 * Main loop also calls motor_synth_service() to run any deferred apply work
 * (pattern rebuild on synth_cycles change, TIM2 reprogram on period change)
 * outside the TIM2 ISR -- keeps the ISR cheap so reconfigs don't stall the
 * SPI pipeline at slow clocks.
 * ----------------------------------------------------------------------------
 */
#include "stm32f4xx_hal.h"
#include "motor_synth.h"
#include "motor_send.h"

void SystemClock_Config(void);          /* system_clock_config.c */

#define BLOCK_ROWS  200u

/* diagnostic counters (defined in motor_synth.c / motor_send.c) */
extern volatile uint32_t g_synth_cb;    /* synth DMA half/full callbacks   */
extern volatile uint32_t g_obr;         /* motor_on_block_ready entered    */
extern volatile uint32_t g_arm_called;  /* arm_tx entered                  */
extern volatile uint32_t g_arm_ok;      /* PB0 raised (DMA armed)          */
extern volatile uint32_t g_arm_fail;    /* DMA arm failed                  */
extern volatile uint32_t g_sent;        /* SPI transfer completed          */

/* PC13 is active-low on the Black Pill: RESET = lit, SET = dark. */
static void led_on(void)  { HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_RESET); }
static void led_off(void) { HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_SET);   }

static void led_init(void)
{
    __HAL_RCC_GPIOC_CLK_ENABLE();
    GPIO_InitTypeDef g = {0};
    g.Pin   = GPIO_PIN_13;
    g.Mode  = GPIO_MODE_OUTPUT_PP;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOC, &g);
    led_off();
}

/* Blink-group helper that ALSO services deferred apply work between LED edges,
 * so a slow rebuild (~500 us) doesn't have to wait for a whole blink cycle.
 * motor_synth_service() is cheap when there's nothing to do.                 */
static void blink_group(int n)
{
    for (int i = 0; i < n; ++i) {
        motor_synth_service();
        led_on();  HAL_Delay(150);
        motor_synth_service();
        led_off(); HAL_Delay(250);
    }
    motor_synth_service();
    HAL_Delay(1500);
}

int main(void)
{
    HAL_Init();
    SystemClock_Config();
    led_init();

    motor_send_init(BLOCK_ROWS);
    HAL_Delay(10);
    motor_synth_init(BLOCK_ROWS);
    HAL_Delay(10);
    motor_synth_start();

    for (;;) {
        motor_synth_service();          /* drain any pending reconfig work  */

        int stage = 1;
        if (g_synth_cb)   stage = 2;
        if (g_obr)        stage = 3;
        if (g_arm_called) stage = 4;
        if (g_arm_ok)     stage = 5;
        if (g_sent)       stage = 6;
        blink_group(stage);
    }
}

void SysTick_Handler(void)
{
    HAL_IncTick();
}

/* Fault = fast flutter (no SysTick dependence), visibly distinct from the
 * counted stage groups above. */
void Error_Handler(void)
{
    __HAL_RCC_GPIOC_CLK_ENABLE();
    GPIO_InitTypeDef g = {0};
    g.Pin = GPIO_PIN_13; g.Mode = GPIO_MODE_OUTPUT_PP;
    HAL_GPIO_Init(GPIOC, &g);
    for (;;) {
        HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_13);
        for (volatile uint32_t d = 0; d < 300000u; ++d) { }
    }
}