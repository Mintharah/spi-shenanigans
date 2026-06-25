/*
 * motor_send.c  (instrumented)
 * ----------------------------------------------------------------------------
 * STM32 send side. Implements motor_on_block_ready(): each block becomes a wire
 * frame [ frame_header | rows | CRC ] in a double buffer, shipped to the Pi as an
 * SPI2 slave (full-duplex DMA) with a data-ready GPIO handshake.
 *
 * Diagnostic counters (read by the diagnostic main.c) are added; they have no
 * effect on behaviour. Remove them once bring-up is done.
 *
 *   SPI2 slave:  PB12 NSS, PB13 SCK, PB14 MISO, PB15 MOSI  (AF5), mode 0, MSB-first
 *   data-ready:  PB0 (output, high = frame waiting)
 *   DMA1 Stream4 Ch0 = SPI2_TX, DMA1 Stream3 Ch0 = SPI2_RX (dummy)
 * ----------------------------------------------------------------------------
 */
#include "stm32f4xx_hal.h"
#include <string.h>
#include "motor_wire.h"
#include "motor_source.h"   /* motor_on_block_ready() -- we implement it */
#include "motor_send.h"

extern void Error_Handler(void);

#ifndef MOTOR_DEFAULT_BLOCK_ROWS
#define MOTOR_DEFAULT_BLOCK_ROWS  200u
#endif

/* data-ready line */
#define DR_PORT            GPIOB
#define DR_PIN             GPIO_PIN_0

/*
 * FIX: How many APB1 cycles to busy-wait after HAL_SPI_TransmitReceive_DMA
 * returns before asserting PB0.  SPI2 is on APB1 (max 42 MHz on F4).
 * The DMA needs ~4 AHB cycles to load the first word into the TX FIFO.
 * Waiting 8 APB1 NOPs is more than enough and costs < 200 ns at 42 MHz.
 * Increase if the Pi still wins the race at very high SCK rates.
 */
#ifndef MOTOR_DR_ASSERT_DELAY_CYCLES
#define MOTOR_DR_ASSERT_DELAY_CYCLES  8u
#endif

/* ---- diagnostic counters (read by main.c) -------------------------------- */
volatile uint32_t g_obr        = 0;   /* motor_on_block_ready entered        */
volatile uint32_t g_arm_called = 0;   /* arm_tx entered                      */
volatile uint32_t g_arm_ok     = 0;   /* PB0 raised (DMA armed OK)           */
volatile uint32_t g_arm_fail   = 0;   /* HAL_SPI_TransmitReceive_DMA failed  */
volatile uint32_t g_sent       = 0;   /* SPI transfer completed (TxRxCplt)   */
volatile uint32_t g_spi_err    = 0;   /* SPI error (OVR/glitch) -> aborted + re-armed */

/* ---- state --------------------------------------------------------------- */
static SPI_HandleTypeDef s_hspi2;
static DMA_HandleTypeDef s_hdma_tx;
static DMA_HandleTypeDef s_hdma_rx;

static uint16_t s_block_rows = MOTOR_DEFAULT_BLOCK_ROWS;
static uint16_t s_frame_len  = 0;
static uint32_t s_seq        = 0;

static _Alignas(8) uint8_t s_frame[2][MOTOR_MAX_FRAME_BYTES]; /* double buffer  */
static _Alignas(8) uint8_t s_rx_dummy[MOTOR_MAX_FRAME_BYTES]; /* master's bytes, ignored */

static volatile int s_tx_idx  = -1;   /* buffer being clocked out, -1 = idle    */
static volatile int s_pending = 0;    /* a newer frame waits in the other buffer */

static volatile uint32_t s_sent    = 0;
static volatile uint32_t s_skipped = 0;

/* ---- CRC (table-driven CRC-32/MPEG-2) ------------------------------------ */
static uint32_t s_crc_table[256];

static void crc32_init(void)
{
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i << 24;
        for (int j = 0; j < 8; ++j)
            c = (c & 0x80000000u) ? (c << 1) ^ 0x04C11DB7u : (c << 1);
        s_crc_table[i] = c;
    }
}

static uint32_t crc32_mpeg2(const uint8_t *d, size_t n)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i)
        crc = (crc << 8) ^ s_crc_table[((crc >> 24) ^ d[i]) & 0xFFu];
    return crc;
}

/* ---- frame assembly ------------------------------------------------------ */
static void assemble_frame(uint8_t *buf, const uint16_t *samples, uint16_t n_rows)
{
    uint32_t seq = s_seq++;

    frame_header_t *h = (frame_header_t *)buf;
    h->magic     = MOTOR_FRAME_MAGIC;
    h->seq       = seq;
    h->timestamp = (uint64_t)HAL_GetTick() * 1000ull;
    h->version   = MOTOR_CONTRACT_VERSION;
    h->flags     = 0;
    h->n_rows    = n_rows;
    h->_reserved = 0;

    int16_t  vx  = (int16_t)(180 + (int)(seq % 40));
    int16_t  vy  = (int16_t)(-150 + (int)(seq % 30));
    int16_t  vz  = (int16_t)(40 + (int)(seq % 60));
    uint16_t rpm = (uint16_t)(1500);

    motor_row_t *rows = (motor_row_t *)(buf + sizeof(frame_header_t));
    for (uint16_t i = 0; i < n_rows; ++i) {
        rows[i].current = samples[i];
        rows[i].vib_x   = vx;
        rows[i].vib_y   = vy;
        rows[i].vib_z   = vz;
        rows[i].rpm     = rpm;
    }

    size_t covered = sizeof(frame_header_t) + (size_t)n_rows * sizeof(motor_row_t);
    uint32_t crc = crc32_mpeg2(buf, covered);
    memcpy(buf + covered, &crc, sizeof crc);
}

/* ---- transmit ------------------------------------------------------------ */

/*
 * FIX: Busy-wait helper.
 * Each iteration is one NOP; the compiler cannot optimise it away because the
 * loop variable is volatile.  Used to let the DMA engine load the first word
 * into the SPI TX FIFO before we raise PB0 and invite the Pi to start clocking.
 */
static void spin_cycles(uint32_t n)
{
    volatile uint32_t i = n;        /* FIX */
    while (i--) __NOP();            /* FIX */
}

static void arm_tx(int idx)
{
    g_arm_called++;
    s_tx_idx = idx;
    if (HAL_SPI_TransmitReceive_DMA(&s_hspi2, s_frame[idx], s_rx_dummy,
                                    s_frame_len) != HAL_OK) {
        s_tx_idx = -1;
        s_skipped++;
        g_arm_fail++;
        return;
    }

    /*
     * FIX: Wait until the SPI peripheral's DMA request line is actually
     * serviced (TX FIFO has at least one word) before telling the Pi the
     * frame is ready.  Without this, PB0 rises while the FIFO is still
     * empty; the Pi starts its transaction and reads 0x00/0xFF garbage.
     *
     * Belt-and-braces: also poll HAL state so we never assert DR while
     * HAL still considers the peripheral "ready" (shouldn't happen after
     * a successful TransmitReceive_DMA, but guards against future HAL
     * version quirks).
     */
    spin_cycles(MOTOR_DR_ASSERT_DELAY_CYCLES);                  /* FIX */

    /* Confirm HAL actually transitioned to BUSY before raising DR */  /* FIX */
    uint32_t timeout = 1000u;                                          /* FIX */
    while (HAL_SPI_GetState(&s_hspi2) != HAL_SPI_STATE_BUSY_TX_RX     /* FIX */
           && --timeout) {                                             /* FIX */
        __NOP();                                                       /* FIX */
    }                                                                  /* FIX */
    if (timeout == 0u) {                                               /* FIX */
        /* DMA never became busy — treat as arm failure */             /* FIX */
        HAL_SPI_DMAStop(&s_hspi2);                                     /* FIX */
        s_tx_idx = -1;                                                 /* FIX */
        s_skipped++;                                                   /* FIX */
        g_arm_fail++;                                                  /* FIX */
        return;                                                        /* FIX */
    }                                                                  /* FIX */

    g_arm_ok++;
    HAL_GPIO_WritePin(DR_PORT, DR_PIN, GPIO_PIN_SET);   /* frame waiting */
}

void motor_on_block_ready(const uint16_t *samples, uint16_t n_rows)
{
    g_obr++;
    int inflight = s_tx_idx;                 /* 0, 1, or -1 (idle) */
    int build    = (inflight == 0) ? 1 : 0;  /* build into the buffer not in flight */
    assemble_frame(s_frame[build], samples, n_rows);

    if (inflight < 0) {
        arm_tx(build);
    } else {
        if (s_pending) s_skipped++;
        s_pending = 1;
    }
}

void HAL_SPI_TxRxCpltCallback(SPI_HandleTypeDef *h)
{
    if (h->Instance != SPI2) return;

    HAL_GPIO_WritePin(DR_PORT, DR_PIN, GPIO_PIN_RESET);  /* frame consumed */
    s_sent++;
    g_sent++;

    int just = s_tx_idx;                 /* buffer just clocked out */

    /* Force SPI+DMA fully idle so the NEXT frame starts shifting at byte 0.
     * Keeps the slave locked to the master's chip-select framing. */
    HAL_SPI_DMAStop(&s_hspi2);

    if (s_pending) {
        s_pending = 0;
        arm_tx((just == 0) ? 1 : 0);     /* send the freshly-built other buffer */
    } else {
        s_tx_idx = -1;
    }
}

void HAL_SPI_ErrorCallback(SPI_HandleTypeDef *h)
{
    if (h->Instance != SPI2) return;
    g_spi_err++;

    HAL_GPIO_WritePin(DR_PORT, DR_PIN, GPIO_PIN_RESET);  /* don't invite a read */
    HAL_SPI_Abort(&s_hspi2);                             /* clear error, reset   */

    int idx = (s_tx_idx >= 0) ? s_tx_idx : 0;            /* reuse last frame     */
    s_pending = 0;
    arm_tx(idx);                                         /* re-arm, re-aligned   */
}

/* ---- init ---------------------------------------------------------------- */
static void gpio_init(void)
{
    __HAL_RCC_GPIOB_CLK_ENABLE();

    GPIO_InitTypeDef g = {0};
    g.Pin       = GPIO_PIN_12 | GPIO_PIN_13 | GPIO_PIN_14 | GPIO_PIN_15;
    g.Mode      = GPIO_MODE_AF_PP;
    g.Pull      = GPIO_NOPULL;
    g.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    g.Alternate = GPIO_AF5_SPI2;
    HAL_GPIO_Init(GPIOB, &g);

    GPIO_InitTypeDef d = {0};
    d.Pin   = DR_PIN;
    d.Mode  = GPIO_MODE_OUTPUT_PP;
    d.Pull  = GPIO_NOPULL;
    d.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(DR_PORT, &d);
    HAL_GPIO_WritePin(DR_PORT, DR_PIN, GPIO_PIN_RESET);
}

static void dma_init(void)
{
    __HAL_RCC_DMA1_CLK_ENABLE();

    s_hdma_tx.Instance                 = DMA1_Stream4;
    s_hdma_tx.Init.Channel             = DMA_CHANNEL_0;
    s_hdma_tx.Init.Direction           = DMA_MEMORY_TO_PERIPH;
    s_hdma_tx.Init.PeriphInc           = DMA_PINC_DISABLE;
    s_hdma_tx.Init.MemInc              = DMA_MINC_ENABLE;
    s_hdma_tx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
    s_hdma_tx.Init.MemDataAlignment    = DMA_MDATAALIGN_BYTE;
    s_hdma_tx.Init.Mode                = DMA_NORMAL;
    s_hdma_tx.Init.Priority            = DMA_PRIORITY_HIGH;
    s_hdma_tx.Init.FIFOMode            = DMA_FIFOMODE_DISABLE;
    if (HAL_DMA_Init(&s_hdma_tx) != HAL_OK) Error_Handler();
    __HAL_LINKDMA(&s_hspi2, hdmatx, s_hdma_tx);

    s_hdma_rx.Instance                 = DMA1_Stream3;
    s_hdma_rx.Init.Channel             = DMA_CHANNEL_0;
    s_hdma_rx.Init.Direction           = DMA_PERIPH_TO_MEMORY;
    s_hdma_rx.Init.PeriphInc           = DMA_PINC_DISABLE;
    s_hdma_rx.Init.MemInc              = DMA_MINC_DISABLE;
    s_hdma_rx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
    s_hdma_rx.Init.MemDataAlignment    = DMA_MDATAALIGN_BYTE;
    s_hdma_rx.Init.Mode                = DMA_NORMAL;
    s_hdma_rx.Init.Priority            = DMA_PRIORITY_HIGH;
    s_hdma_rx.Init.FIFOMode            = DMA_FIFOMODE_DISABLE;
    if (HAL_DMA_Init(&s_hdma_rx) != HAL_OK) Error_Handler();
    __HAL_LINKDMA(&s_hspi2, hdmarx, s_hdma_rx);

    HAL_NVIC_SetPriority(DMA1_Stream4_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(DMA1_Stream4_IRQn);
    HAL_NVIC_SetPriority(DMA1_Stream3_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(DMA1_Stream3_IRQn);
    HAL_NVIC_SetPriority(SPI2_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(SPI2_IRQn);
}

static void spi_init(void)
{
    __HAL_RCC_SPI2_CLK_ENABLE();
    s_hspi2.Instance               = SPI2;
    s_hspi2.Init.Mode              = SPI_MODE_SLAVE;
    s_hspi2.Init.Direction         = SPI_DIRECTION_2LINES;
    s_hspi2.Init.DataSize          = SPI_DATASIZE_8BIT;
    s_hspi2.Init.CLKPolarity       = SPI_POLARITY_LOW;
    s_hspi2.Init.CLKPhase          = SPI_PHASE_1EDGE;
    s_hspi2.Init.NSS               = SPI_NSS_HARD_INPUT;
    s_hspi2.Init.FirstBit          = SPI_FIRSTBIT_MSB;
    s_hspi2.Init.TIMode            = SPI_TIMODE_DISABLE;
    s_hspi2.Init.CRCCalculation    = SPI_CRCCALCULATION_DISABLE;
    if (HAL_SPI_Init(&s_hspi2) != HAL_OK) Error_Handler();
}

void motor_send_init(uint16_t block_rows)
{
    if (block_rows == 0 || block_rows > MOTOR_MAX_ROWS_PER_BLOCK)
        block_rows = MOTOR_DEFAULT_BLOCK_ROWS;
    s_block_rows = block_rows;
    s_frame_len  = (uint16_t)(sizeof(frame_header_t)
                 + (size_t)block_rows * sizeof(motor_row_t) + sizeof(frame_crc_t));

    crc32_init();
    gpio_init();
    dma_init();
    spi_init();
}

void motor_send_get_stats(uint32_t *frames_sent, uint32_t *frames_skipped)
{
    if (frames_sent)    *frames_sent    = s_sent;
    if (frames_skipped) *frames_skipped = s_skipped;
}

/* ---- ISR plumbing -------------------------------------------------------- */
void DMA1_Stream4_IRQHandler(void) { HAL_DMA_IRQHandler(&s_hdma_tx); }
void DMA1_Stream3_IRQHandler(void) { HAL_DMA_IRQHandler(&s_hdma_rx); }
void SPI2_IRQHandler(void)         { HAL_SPI_IRQHandler(&s_hspi2); }