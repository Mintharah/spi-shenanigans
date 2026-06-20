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
 * A TimerTimeout bounds InterruptWait. That bound is a LIVENESS WATCHDOG, not a
 * fallback read: if no edge arrives in the window (a silent/dead STM32), the
 * wait returns, we count it, and we go back to waiting -- we never read without
 * a real data-ready edge.
 *
 * Build (QNX): libc only -- do NOT link -lrt or -lpthread. Compile as C11.
 *   qcc -V<target> -std=gnu11 -O2 motor_controller.c -lrpi_spi -o motor_controller
 * Requires privilege (root / PROCMGR_AID_INTERRUPT) to raise SCHED_FIFO priority
 * and to attach the GPIO interrupt.
 *
 * HARD PREREQUISITE: the data-ready GPIO IRQ vector must be set (cfg.dataready_irq)
 * and the pin configured for rising-edge detection in the GPIO controller. On the
 * Pi 5 the GPIO sits behind RP1, so both are BSP-specific. Without a valid IRQ the
 * controller refuses to start (by design -- there is no paced fallback).
 *
 * STILL TODO (tracked on the checklist):
 *   - CRC parameters below are PROVISIONAL; pin them to match the STM32 exactly.
 *   - Configure the data-ready GPIO edge (see dataready_init).
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
#include <sys/neutrino.h>   /* InterruptAttachEvent, InterruptWait, TimerTimeout, SIGEV_INTR */

#include "motor_wire.h"
#include "motor_shm.h"
#include "rpi_spi.h"        /* rpi_spi_*, SPI_SUCCESS (your existing driver) */

/* ============================ runtime config ============================== */
/* These will come from the JSON config later; hardcoded defaults for now.     */
typedef struct {
    int      spi_bus;        /* rpi_spi bus index                              */
    int      spi_dev;        /* rpi_spi device/CS index                        */
    int      spi_mode;       /* SPI mode 0..3 -- MUST match the STM32 slave    */
    uint32_t spi_clock_hz;   /* 10 MHz                                          */
    uint16_t block_rows;     /* fixed sample count per block (== STM32 config)  */
    long     period_ns;      /* nominal block cadence; bounds the watchdog      */
    int      rt_priority;    /* SCHED_FIFO priority                             */
    int      dataready_irq;  /* REQUIRED GPIO IRQ vector (BSP-specific)         */
} controller_config_t;

static const controller_config_t DEFAULT_CFG = {
    .spi_bus       = 0,
    .spi_dev       = 0,
    .spi_mode      = 0,
    .spi_clock_hz  = 10000000u,
    .block_rows    = 200,
    .period_ns     = 10L * 1000L * 1000L,   /* 10 ms == 100 Hz */
    .rt_priority   = 30,
    .dataready_irq = -1,                    /* MUST set to your BSP IRQ vector */
};

/* ============================ diagnostics ================================= */
typedef struct {
    uint64_t frames_ok;
    uint64_t seq_drops;     /* total blocks the producer never saw (gaps)      */
    uint64_t crc_err;
    uint64_t magic_err;
    uint64_t version_err;
    uint64_t size_err;
    uint64_t duplicates;    /* same seq seen again (should be ~0 in interrupt)  */
    uint64_t resets;        /* seq went backwards (STM32 reboot?)               */
    uint64_t timeouts;      /* watchdog: no data-ready edge in the window       */
    uint64_t spi_err;
} controller_stats_t;

/* ============================ CRC ========================================= */
/* PROVISIONAL: CRC-32/MPEG-2 (poly 0x04C11DB7, init 0xFFFFFFFF, no reflection,
 * xorout 0). This MUST match the STM32 byte-for-byte -- verify both ends agree
 * on a shared test vector before trusting it. Simplest guaranteed-match path is
 * for both sides to run this same software routine over the frame byte buffer. */
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
    return pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp); /* 0 or errno */
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

/* ============================ data-ready (interrupt-only) ================= */
enum { WR_OK, WR_TIMEOUT, WR_ERROR };

typedef struct {
    int             irq;
    int             int_id;
    struct sigevent ev;
    uint64_t        watchdog_ns;   /* liveness bound on InterruptWait */
} dataready_t;

static int dataready_init(dataready_t *d, const controller_config_t *cfg)
{
    d->int_id = -1;

    if (cfg->dataready_irq < 0) {
        fprintf(stderr,
            "error: dataready_irq not set. This interrupt-only build needs the "
            "GPIO IRQ vector for the data-ready line (BSP-specific; Pi 5 GPIO is "
            "behind RP1). No paced fallback.\n");
        return -1;
    }
    d->irq         = cfg->dataready_irq;
    d->watchdog_ns = (uint64_t)cfg->period_ns * 8u;   /* ~8 missed blocks => link down */

    /* TODO (BSP): configure the data-ready GPIO pin as input with rising-edge
     * interrupt detection in the GPIO controller -- e.g. via the BSP GPIO driver
     * or mmap_device_memory() of the controller registers. */

    SIGEV_INTR_INIT(&d->ev);
    d->int_id = InterruptAttachEvent(d->irq, &d->ev, _NTO_INTR_FLAGS_TRK_MSK);
    if (d->int_id == -1) {
        fprintf(stderr, "error: InterruptAttachEvent(irq=%d) failed: %s "
                        "(need PROCMGR_AID_INTERRUPT / root)\n",
                d->irq, strerror(errno));
        return -1;
    }
    return 0;
}

static int dataready_wait(dataready_t *d)
{
    /* Watchdog-bounded wait: a silent STM32 can't block us forever. On timeout we
     * report and loop back to wait -- we only ever read after a real edge. */
    TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_INTR, NULL, &d->watchdog_ns, NULL);
    if (InterruptWait(0, NULL) == -1)
        return (errno == ETIMEDOUT) ? WR_TIMEOUT : WR_ERROR;
    InterruptUnmask(d->irq, d->int_id);
    return WR_OK;
}

static void dataready_cleanup(dataready_t *d)
{
    if (d->int_id != -1)
        InterruptDetach(d->int_id);
}

/* ============================ shutdown ==================================== */
static volatile sig_atomic_t g_running = 1;
static void on_signal(int sig) { (void)sig; g_running = 0; }

/* ============================ main ======================================== */
int main(void)
{
    controller_config_t cfg = DEFAULT_CFG;
    /* TODO (Config/OTA): load cfg from JSON, then push the STM32 subset down
     * via a SET_CONFIG command frame before entering the loop. */

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
        return 1;                                   /* interrupt-only: no fallback */
    }
    fprintf(stderr, "[ctrl] interrupt mode on irq %d\n", dr.irq);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* Fixed frame size for the configured block. n_rows is expected to equal
     * cfg.block_rows (fixed sample count), so the CRC sits at a fixed offset.   */
    const size_t frame_bytes = sizeof(frame_header_t)
                             + (size_t)cfg.block_rows * sizeof(motor_row_t)
                             + sizeof(frame_crc_t);

    static _Alignas(8) uint8_t rx[MOTOR_MAX_FRAME_BYTES];
    static _Alignas(8) uint8_t tx[MOTOR_MAX_FRAME_BYTES];   /* zeroed clocks */
    memset(tx, 0, sizeof tx);

    controller_stats_t st = {0};
    uint32_t last_seq = 0;
    int      have_last = 0;
    struct timespec t_log;
    clock_gettime(CLOCK_MONOTONIC, &t_log);

    while (g_running) {
        int w = dataready_wait(&dr);
        if (w == WR_TIMEOUT) { st.timeouts++; continue; }    /* watchdog: link silent */
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

        /* sequence accounting (wrap-aware) */
        int publish = 1;
        if (!have_last) {
            have_last = 1;
            last_seq  = h->seq;
        } else {
            int32_t diff = (int32_t)(h->seq - last_seq);
            if      (diff == 0) { st.duplicates++; publish = 0; }
            else if (diff == 1) { last_seq = h->seq; }
            else if (diff  > 1) { st.seq_drops += (uint64_t)(diff - 1); last_seq = h->seq; }
            else                { st.resets++; last_seq = h->seq; }  /* seq went backwards */
        }

        if (publish) {
            const motor_row_t *rows = (const motor_row_t *)(rx + sizeof(frame_header_t));
            /* snapshot = most recent sample = last row in the block (assumes the
             * STM32 fills rows in time order). */
            motor_snapshot_publish(&region->snapshot, &rows[h->n_rows - 1],
                                   h->seq, h->timestamp, h->flags);
            motor_ring_publish(&region->ring, h, rows);
            st.frames_ok++;
        }

        /* periodic stats, ~1 Hz */
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec != t_log.tv_sec) {
            t_log = now;
            fprintf(stderr,
                "[ctrl] ok=%" PRIu64 " drops=%" PRIu64 " crc=%" PRIu64
                " magic=%" PRIu64 " ver=%" PRIu64 " size=%" PRIu64
                " dup=%" PRIu64 " rst=%" PRIu64 " to=%" PRIu64 " spi=%" PRIu64 "\n",
                st.frames_ok, st.seq_drops, st.crc_err, st.magic_err,
                st.version_err, st.size_err, st.duplicates, st.resets,
                st.timeouts, st.spi_err);
        }
    }

    /* cleanup */
    dataready_cleanup(&dr);
    rpi_spi_cleanup_device(cfg.spi_bus, cfg.spi_dev);
    munmap(region, sizeof(shm_region_t));
    shm_unlink(MOTOR_SHM_NAME);
    fprintf(stderr, "[ctrl] shutdown: ok=%" PRIu64 " drops=%" PRIu64
                    " crc=%" PRIu64 "\n", st.frames_ok, st.seq_drops, st.crc_err);
    return 0;
}
