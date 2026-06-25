/*
 * stm_spi_slave_test.c -- minimal SPI2-slave sanity test. NO DMA, NO frames.
 * ----------------------------------------------------------------------------
 * Replaces main.c temporarily. Each time the Pi clocks 16 bytes, the slave
 * shifts out a fixed pattern 01 02 ... 10 using BLOCKING HAL_SPI_TransmitReceive
 * (no DMA, no data-ready, no CRC). The onboard LED (PC13) TOGGLES on every
 * completed transfer:
 *     LED flickering  = transfers completing  -> the SPI link works
 *     LED frozen/dark = nothing completing    -> link or SPI setup still broken
 *
 * Pins (same as the real firmware): PB12 NSS, PB13 SCK, PB14 MISO, PB15 MOSI
 * (AF5), SPI mode 0, MSB-first, 8-bit. NSS = hard input, so keep PB12 tied to
 * GND (as you have it now) to keep the slave selected.
 * ----------------------------------------------------------------------------
 */
#include "stm32f4xx_hal.h"

void SystemClock_Config(void);   /* from your system_clock_config.c */
void Error_Handler(void);

static SPI_HandleTypeDef hspi2;

static void led_init(void)
{
    __HAL_RCC_GPIOC_CLK_ENABLE();
    GPIO_InitTypeDef g = {0};
    g.Pin = GPIO_PIN_13; g.Mode = GPIO_MODE_OUTPUT_PP; g.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOC, &g);
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_SET);   /* off (active-low) */
}

static void spi_init(void)
{
    __HAL_RCC_GPIOB_CLK_ENABLE();
    GPIO_InitTypeDef g = {0};
    g.Pin       = GPIO_PIN_12 | GPIO_PIN_13 | GPIO_PIN_14 | GPIO_PIN_15;
    g.Mode      = GPIO_MODE_AF_PP;
    g.Pull      = GPIO_NOPULL;
    g.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    g.Alternate = GPIO_AF5_SPI2;
    HAL_GPIO_Init(GPIOB, &g);

    __HAL_RCC_SPI2_CLK_ENABLE();
    hspi2.Instance            = SPI2;
    hspi2.Init.Mode           = SPI_MODE_SLAVE;
    hspi2.Init.Direction      = SPI_DIRECTION_2LINES;
    hspi2.Init.DataSize       = SPI_DATASIZE_8BIT;
    hspi2.Init.CLKPolarity    = SPI_POLARITY_LOW;
    hspi2.Init.CLKPhase       = SPI_PHASE_1EDGE;
    hspi2.Init.NSS            = SPI_NSS_HARD_INPUT;
    hspi2.Init.FirstBit       = SPI_FIRSTBIT_MSB;
    hspi2.Init.TIMode         = SPI_TIMODE_DISABLE;
    hspi2.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
    if (HAL_SPI_Init(&hspi2) != HAL_OK) Error_Handler();
}

int main(void)
{
    HAL_Init();
    SystemClock_Config();
    led_init();
    spi_init();

    uint8_t tx[16], rx[16];
    for (int i = 0; i < 16; ++i) tx[i] = (uint8_t)(i + 1);   /* 01 02 ... 10 */

    for (;;) {
        /* Block until the Pi clocks 16 bytes (1 s timeout so a dead link
         * doesn't hang us forever). Toggle the LED on each completed transfer. */
        if (HAL_SPI_TransmitReceive(&hspi2, tx, rx, sizeof tx, 1000) == HAL_OK)
            HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_13);
    }
}

void SysTick_Handler(void) { HAL_IncTick(); }

void Error_Handler(void)
{
    __HAL_RCC_GPIOC_CLK_ENABLE();
    GPIO_InitTypeDef g = {0};
    g.Pin = GPIO_PIN_13; g.Mode = GPIO_MODE_OUTPUT_PP;
    HAL_GPIO_Init(GPIOC, &g);
    for (;;) { HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_13);
               for (volatile uint32_t d = 0; d < 300000u; ++d) { } }
}
