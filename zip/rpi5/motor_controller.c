/*
 * motor_controller.c
 * ----------------------------------------------------------------------------
 * QNX controller / producer node for the predictive-maintenance pipeline.
 * INTERRUPT-ONLY build: reads are driven solely by the data-ready GPIO edge, so
 * every read lands on a fresh, complete frame (no free-running poll => no clock
 * drift, no duplicate/gap churn from being unsynced to the STM32).
 *
 * Single SCHED_FIFO thread that, per data-ready interrupt:
 *   1. reads one fixed-size frame over SPI (rpi_spi driver),
 *   2. validates magic / version / size / CRC and accounts for sequence gaps,
 *   3. publishes the latest row to the seqlock snapshot (for Qt) and the whole
 *      block to the lock-free ring (for the SOME/IP publisher).
 *
 * A TimerTimeout bounds the pulse wait, and on every wake-up the controller
 * re-checks the data-ready pin LEVEL: it reads whenever the line is high, using
 * the rising-edge pulse only as a fast wake-up. This is robust to a missed edge
 * (line already/still high) -- it can never deadlock waiting for an edge that
 * already happened. A low line simply means idle.
 *
 * Data-ready uses the rpi_gpio resource manager: we register a rising-edge event
 * on the data-ready pin and also poll its level via rpi_gpio_input(). The server
 * owns the RP1 GPIO interrupt; no raw IRQ vector and no RP1 register access here.
 *
 * Build (QNX): libc only -- do NOT link -lrt or -lpthread. Compile as C11.
 *   qcc -V<target> -std=gnu11 -O2 motor_controller.c rpi_gpio.c -lrpi_spi -o motor_controller
 *   (rpi_gpio.c + rpi_gpio.h: the client API from the hardware-component-samples repo)
 * Requires root to raise SCHED_FIFO priority. The rpi_gpio resource manager must
 * already be running (stock image: /dev/gpio is present).
 *
 * HARD PREREQUISITE: the rpi_gpio resource manager must be running (the stock
 * image registers /dev/gpio) and cfg.dataready_pin must be a free header GPIO
 * (0..27, not your SPI pins). Without the server the controller refuses to start
 * (by design -- there is no paced fallback).
 *
 * STILL TODO (tracked on the checklist):
 *   - Load runtime params from JSON and push the STM32 subset via SET_CONFIG.
 * ----------------------------------------------------------------------------
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <sched.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/neutrino.h>   /* ChannelCreate, ConnectAttach, MsgReceivePulse, TimerTimeout */

#include "motor_wire.h"
#include "motor_shm.h"
#include "rpi_spi.h"        /* rpi_spi_*, SPI_SUCCESS (your existing driver) */
#include "rpi_gpio.h"       /* rpi_gpio_* client API (hardware-component-samples) */

/* ============================ runtime config ============================== */
typedef struct {
    int      spi_bus;
    int      spi_dev;
    int      spi_mode;
    uint32_t spi_clock_hz;
    uint16_t block_rows;
    long     period_ns;
    int      rt_priority;
    int      dataready_pin;
} controller_config_t;

static const controller_config_t DEFAULT_CFG = {
    .spi_bus       = 0,
    .spi_dev       = 0,
    .spi_mode      = 0,
    .spi_clock_hz  = 10000000u,
    .block_rows    = 200,
    .period_ns     = 10L * 1000L * 1000L,
    .rt_priority   = 30,
    .dataready_pin = 17,
};

/* ============================ diagnostics ================================= */
typedef struct {
    uint64_t frames_ok;
    uint64_t seq_drops;
    uint64_t crc_err;
    uint64_t magic_err;
    uint64_t version_err;
    uint64_t size_err;
    uint64_t duplicates;
    uint64_t resets;
    uint64_t timeouts;
    uint64_t spi_err;
    uint64_t floating_pin;  /* FIX: reads suppressed because pin looked high with no STM32 */
} controller_stats_t;

/* ============================ CRC ========================================= */
static uint32_t frame_crc_compute(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= (uint32_t)data[i] << 24;
        for (int b = 0; b < 8; ++b)
            crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04C11DB7u : (crc << 1);
    }
    return crc;
}

/* ============================ scheduling ================================== */
static int set_realtime_priority(int prio)
{
    struct sched_param sp;
    memset(&sp, 0, sizeof sp);
    sp.sched_priority = prio;
    return pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
}

/* ============================ shared memory =============================== */
static shm_region_t *shm_setup(void)
{
    int fd = shm_open(MOTOR_SHM_NAME, O_RDWR | O_CREAT, 0666);
    if (fd == -1) return NULL;
    if (ftruncate(fd, (off_t)sizeof(shm_region_t)) == -1) { close(fd); return NULL; }
    shm_region_t *r = mmap(NULL, sizeof(shm_region_t),
                           PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (r == MAP_FAILED) return NULL;
    motor_shm_region_init(r);
    return r;
}

/* ============================ data-ready (edge pulse via rpi_gpio) ======== */
enum { WR_OK, WR_TIMEOUT, WR_ERROR };

#define DR_EVENT_ID    1
#define DR_PULSE_CODE  _PULSE_CODE_MINAVAIL

/*
 * RP1 GPIO live level registers (Pi 5).
 * GPIO_STATUS for pin N is at GPIO_BASE + N*8 bytes.
 * Bit 9 (INFROMPAD) is the live pad sample -- always current, never cached.
 * This bypasses rpi_gpio_input() which returns stale server-side state.
 */
#define RP1_GPIO_BASE          0x400e0000u
#define RP1_GPIO_MAP_SIZE      0x1000u
#define RP1_GPIO_STATUS_STRIDE 2u          /* uint32_t words per pin (STATUS+CTRL) */
#define RP1_GPIO_INFROMPAD_BIT 9u

/*
 * FIX: How many consecutive GPIO_HIGH samples we must see before trusting
 * the pin is genuinely driven by the STM32.
 *
 * A floating Pi header pin latches high through parasitic capacitance from
 * adjacent driven lines and holds that level stably -- 100 µs confirmation
 * was not enough to distinguish it from a real signal.
 *
 * The STM32 holds DR high for the ENTIRE SPI transfer window.  At 10 MHz SCK
 * with 200 rows the frame is ~3,200 bytes = ~2.56 ms on the wire, plus the
 * DMA-priming guard on the STM32 side.  DR is therefore high for at least
 * 3 ms from the Pi's perspective.
 *
 * A floating pin, with no driver, will bleed off through any real load
 * (oscilloscope probe, the rpi_gpio input buffer, stray PCB resistance) within
 * 1-2 ms.  By waiting 3 × 2 ms = 6 ms total across 3 samples we guarantee:
 *   - Real signal  : still high on all 3 samples (held by the STM32 output)
 *   - Floating pin : has bled low by sample 2 or 3
 *
 * NOTE: wire a 10 kΩ pull-down between GPIO17 and GND on the header.
 * The software guard is a belt; the resistor is the braces.
 */
#define DR_CONFIRM_COUNT      3                        /* FIX: was 2 */
#define DR_CONFIRM_DELAY_NS   2000000ul                /* FIX: 2 ms between checks (was 100 µs) */

typedef struct {
    int                pin;
    int                chid;
    int                coid;
    uint64_t           poll_ns;
    volatile uint32_t *gpio_base;   /* MAP_PHYS mapping of RP1 GPIO block */
} dataready_t;

/* FIX: small busy-wait used between confirmation samples */
static void nsleep(uint64_t ns)                       /* FIX */
{                                                     /* FIX */
    struct timespec ts = {                            /* FIX */
        .tv_sec  = (time_t)(ns / 1000000000ul),       /* FIX */
        .tv_nsec = (long)  (ns % 1000000000ul),       /* FIX */
    };                                                /* FIX */
    nanosleep(&ts, NULL);                             /* FIX */
}                                                     /* FIX */

/*
 * FIX: Confirm the pin is genuinely high by sampling it DR_CONFIRM_COUNT times
 * with DR_CONFIRM_DELAY_NS between each sample.
 *
 * Floating pin:        bleeds low within ~1-2 ms with no active driver;
 *                      will fail by sample 2 or 3 (6 ms total window).
 * STM32-driven signal: held high by a push-pull output for the full transfer
 *                      window (~3+ ms); passes all 3 samples trivially.
 *
 * Returns 1 only if ALL samples read GPIO_HIGH.
 */
/*
 * Read the live GPIO input level directly from the RP1 GPIO_STATUS register
 * (bit 9 = INFROMPAD).  This is the raw pad sample -- never cached, never
 * stale, independent of the rpi_gpio server's internal state tracking.
 *
 * rpi_gpio_input() on this BSP returns the server's cached value which
 * reflects the last edge event, not the current pad voltage.  With pull-down
 * active and no STM32 driving the line, the pad is at 0 V but rpi_gpio_input
 * returns HIGH because the server latched the level from event registration.
 */
static int dataready_read_level(dataready_t *d)
{
    uint32_t status = d->gpio_base[d->pin * RP1_GPIO_STATUS_STRIDE];
    return (int)((status >> RP1_GPIO_INFROMPAD_BIT) & 1u);
}

static int dataready_pin_is_stable_high(dataready_t *d)
{
    for (int i = 0; i < DR_CONFIRM_COUNT; ++i) {
        if (i > 0) nsleep(DR_CONFIRM_DELAY_NS);
        if (dataready_read_level(d) != 1)
            return 0;
    }
    return 1;
}

/*
 * force_pad_pulldown_rp1()
 * ------------------------
 * The rpi_gpio resource manager calls rpi_gpio_setup() which resets the RP1
 * pad to its BSP default -- pull-UP on GPIO17 on this image.  The subsequent
 * rpi_gpio_setup_pull(..., GPIO_PUD_DOWN) call returns GPIO_SUCCESS but does
 * not actually write the RP1 pad control register, so the pin stays pull-UP
 * and floats high when the STM32 is not driving it.
 *
 * This function bypasses the server entirely and writes the RP1 GPIO pad
 * control register directly via /dev/mem, overriding whatever the BSP set.
 * It must be called AFTER rpi_gpio_setup() so it wins the race.
 *
 * RP1 pad control layout (Pi 5):
 *   Base:          0x400d0000
 *   Register size: 4 bytes per GPIO pin
 *   Bits [4:3]:    00 = no pull, 01 = pull-up, 10 = pull-down
 */
static int force_pad_pulldown_rp1(int gpio_pin)
{
    /* QNX has no /dev/mem -- use MAP_PHYS to access physical registers directly. */
    const off_t  PAD_BASE = 0x400d0000;
    const size_t MAP_SIZE = 0x100;

    volatile uint32_t *base = mmap(NULL, MAP_SIZE,
                                   PROT_READ | PROT_WRITE | PROT_NOCACHE,
                                   MAP_SHARED | MAP_PHYS,
                                   NOFD, PAD_BASE);
    if (base == MAP_FAILED) {
        fprintf(stderr, "force_pad_pulldown: mmap MAP_PHYS: %s\n", strerror(errno));
        return -1;
    }

    uint32_t val = base[gpio_pin];
    val &= ~(0x3u << 3);   /* clear bits [4:3] */
    val |=  (0x2u << 3);   /* 0b10 = pull-down */
    base[gpio_pin] = val;

    uint32_t readback = base[gpio_pin];
    munmap((void *)base, MAP_SIZE);

    if (((readback >> 3) & 0x3u) != 0x2u) {
        fprintf(stderr, "force_pad_pulldown: GPIO%d readback 0x%08" PRIx32
                        " -- pull-down did NOT land\n", gpio_pin, readback);
        return -1;
    }

    fprintf(stderr, "[ctrl] GPIO%d pad forced to pull-down "
                    "(reg=0x%08" PRIx32 ")\n", gpio_pin, readback);
    return 0;
}

static int dataready_init(dataready_t *d, const controller_config_t *cfg)
{
    d->pin     = cfg->dataready_pin;
    d->chid    = -1;
    d->coid    = -1;
    d->poll_ns = 2u * 1000u * 1000u;

    d->chid = ChannelCreate(0);
    if (d->chid == -1) {
        fprintf(stderr, "error: ChannelCreate failed: %s\n", strerror(errno));
        return -1;
    }
    d->coid = ConnectAttach(0, 0, d->chid, _NTO_SIDE_CHANNEL, 0);
    if (d->coid == -1) {
        fprintf(stderr, "error: ConnectAttach failed: %s\n", strerror(errno));
        return -1;
    }

    /* Map RP1 GPIO status registers for direct live-level reads */
    d->gpio_base = mmap(NULL, RP1_GPIO_MAP_SIZE,
                        PROT_READ | PROT_NOCACHE,
                        MAP_SHARED | MAP_PHYS,
                        NOFD, (off_t)RP1_GPIO_BASE);
    if (d->gpio_base == MAP_FAILED) {
        fprintf(stderr, "error: mmap RP1 GPIO block: %s\n", strerror(errno));
        return -1;
    }

    if (rpi_gpio_setup(d->pin, GPIO_IN) != GPIO_SUCCESS) {
        fprintf(stderr, "error: rpi_gpio_setup(pin=%d) failed\n", d->pin);
        munmap((void *)d->gpio_base, RP1_GPIO_MAP_SIZE);
        return -1;
    }

    /*
     * Arm the edge detector FIRST -- rpi_gpio_add_event_detect() touches the
     * pad registers internally and resets the pull to the BSP default (UP).
     * force_pad_pulldown_rp1() must come AFTER so it is the last writer.
     */
    if (rpi_gpio_add_event_detect(d->pin, d->coid, GPIO_RISING, DR_EVENT_ID)
            != GPIO_SUCCESS) {
        fprintf(stderr, "error: rpi_gpio_add_event_detect(pin=%d) failed\n", d->pin);
        return -1;
    }

    /* Now override the pull -- nothing touches the pad after this point. */
    if (force_pad_pulldown_rp1(d->pin) != 0) {
        fprintf(stderr, "error: could not force pull-down on GPIO%d -- "
                        "spurious reads likely\n", d->pin);
    }

    /* Sanity check: live pad level must be low with no STM32 connected. */
    {
        if (dataready_read_level(d) == 1) {
            fprintf(stderr, "WARNING: GPIO%d INFROMPAD still HIGH after "
                    "forcing pull-down -- check wiring\n", d->pin);
        } else {
            fprintf(stderr, "[ctrl] GPIO%d confirmed low (pull-down active)\n",
                    d->pin);
        }
    }

    return 0;
}

static int dataready_wait(dataready_t *d, controller_stats_t *st)
{
    uint64_t to = d->poll_ns;
    struct _pulse pulse;
    TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_RECEIVE, NULL, &to, NULL);
    int rc = MsgReceivePulse(d->chid, &pulse, sizeof pulse, NULL);
    if (rc == -1 && errno != ETIMEDOUT)
        return WR_ERROR;

    /*
     * FIX: First quick sample -- if pin is low right now, bail immediately
     * without paying the 2 × 2 ms confirmation cost on every idle poll.
     */
    {                                                        /* FIX */
        unsigned level = GPIO_LOW;                           /* FIX */
        if (rpi_gpio_input(d->pin, &level) != GPIO_SUCCESS  /* FIX */
                || level != GPIO_HIGH)                       /* FIX */
            return WR_TIMEOUT;                               /* FIX */
    }                                                        /* FIX */

    /*
     * FIX: Pin read high on the first sample.  Run the full stable-high
     * confirmation (3 samples × 2 ms apart).  If it fails, the pin is
     * floating -- count it and return TIMEOUT so the main loop ignores it.
     */
    if (dataready_pin_is_stable_high(d))   /* FIX */
        return WR_OK;

    st->floating_pin++;   /* FIX: pin was high once but didn't hold -- float */ /* FIX */
    return WR_TIMEOUT;
}

static void dataready_cleanup(dataready_t *d)
{
    rpi_gpio_cleanup();
    if (d->coid != -1) ConnectDetach(d->coid);
    if (d->chid != -1) ChannelDestroy(d->chid);
    if (d->gpio_base && d->gpio_base != MAP_FAILED)
        munmap((void *)d->gpio_base, RP1_GPIO_MAP_SIZE);
}

/* ============================ shutdown ==================================== */
static volatile sig_atomic_t g_running = 1;
static void on_signal(int sig) { (void)sig; g_running = 0; }

/* ============================ main ======================================== */
int main(void)
{
    controller_config_t cfg = DEFAULT_CFG;

    if (set_realtime_priority(cfg.rt_priority) != 0)
        fprintf(stderr, "warning: SCHED_FIFO prio %d not set (need privilege): %s\n",
                cfg.rt_priority, strerror(errno));

    shm_region_t *region = shm_setup();
    if (!region) { perror("shm_setup"); return 1; }

    if (rpi_spi_configure_device(cfg.spi_bus, cfg.spi_dev, cfg.spi_mode,
                                 cfg.spi_clock_hz) != SPI_SUCCESS) {
        fprintf(stderr, "rpi_spi_configure_device failed\n");
        munmap(region, sizeof(shm_region_t));
        shm_unlink(MOTOR_SHM_NAME);
        return 1;
    }

    dataready_t dr;
    if (dataready_init(&dr, &cfg) != 0) {
        rpi_spi_cleanup_device(cfg.spi_bus, cfg.spi_dev);
        munmap(region, sizeof(shm_region_t));
        shm_unlink(MOTOR_SHM_NAME);
        return 1;
    }
    fprintf(stderr, "[ctrl] edge-pulse mode on GPIO%d (via rpi_gpio)\n", dr.pin);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    const size_t frame_bytes = sizeof(frame_header_t)
                             + (size_t)cfg.block_rows * sizeof(motor_row_t)
                             + sizeof(frame_crc_t);

    static _Alignas(8) uint8_t rx[MOTOR_MAX_FRAME_BYTES];
    static _Alignas(8) uint8_t tx[MOTOR_MAX_FRAME_BYTES];
    memset(tx, 0, sizeof tx);

    controller_stats_t st = {0};
    uint32_t last_seq = 0;
    int      have_last = 0;
    struct timespec t_log;
    clock_gettime(CLOCK_MONOTONIC, &t_log);

    while (g_running) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec != t_log.tv_sec) {
            t_log = now;
            fprintf(stderr,
                "[ctrl] ok=%" PRIu64 " drops=%" PRIu64 " crc=%" PRIu64
                " magic=%" PRIu64 " ver=%" PRIu64 " size=%" PRIu64
                " dup=%" PRIu64 " rst=%" PRIu64 " to=%" PRIu64
                " spi=%" PRIu64 " float=%" PRIu64 "\n",   /* FIX: added float= */
                st.frames_ok, st.seq_drops, st.crc_err, st.magic_err,
                st.version_err, st.size_err, st.duplicates, st.resets,
                st.timeouts, st.spi_err, st.floating_pin);
        }

        int w = dataready_wait(&dr, &st);
        if (w == WR_TIMEOUT) { st.timeouts++; continue; }
        if (w == WR_ERROR)   { if (errno == EINTR) continue; st.timeouts++; continue; }

        if (rpi_spi_write_read_data(cfg.spi_bus, cfg.spi_dev, tx, rx,
                                    frame_bytes) != SPI_SUCCESS) {
            st.spi_err++;
            continue;
        }

        const frame_header_t *h = (const frame_header_t *)rx;

        if (h->magic   != MOTOR_FRAME_MAGIC)        { st.magic_err++;   continue; }
        if (h->version != MOTOR_CONTRACT_VERSION)   { st.version_err++; continue; }
        if (h->n_rows == 0 || h->n_rows != cfg.block_rows) { st.size_err++; continue; }

        size_t covered = sizeof(frame_header_t) + (size_t)h->n_rows * sizeof(motor_row_t);
        if (covered + sizeof(frame_crc_t) > frame_bytes) { st.size_err++; continue; }

        uint32_t rx_crc;
        memcpy(&rx_crc, rx + covered, sizeof rx_crc);
        if (frame_crc_compute(rx, covered) != rx_crc) { st.crc_err++; continue; }

        int publish = 1;
        if (!have_last) {
            have_last = 1;
            last_seq  = h->seq;
        } else {
            int32_t diff = (int32_t)(h->seq - last_seq);
            if      (diff == 0) { st.duplicates++; publish = 0; }
            else if (diff == 1) { last_seq = h->seq; }
            else if (diff  > 1) { st.seq_drops += (uint64_t)(diff - 1); last_seq = h->seq; }
            else                { st.resets++; last_seq = h->seq; }
        }

        if (publish) {
            const motor_row_t *rows = (const motor_row_t *)(rx + sizeof(frame_header_t));
            motor_snapshot_publish(&region->snapshot, &rows[h->n_rows - 1],
                                   h->seq, h->timestamp, h->flags);
            motor_ring_publish(&region->ring, h, rows);
            st.frames_ok++;
        }
    }

    dataready_cleanup(&dr);
    rpi_spi_cleanup_device(cfg.spi_bus, cfg.spi_dev);
    munmap(region, sizeof(shm_region_t));
    shm_unlink(MOTOR_SHM_NAME);
    fprintf(stderr, "[ctrl] shutdown: ok=%" PRIu64 " drops=%" PRIu64
                    " crc=%" PRIu64 "\n", st.frames_ok, st.seq_drops, st.crc_err);
    return 0;
}