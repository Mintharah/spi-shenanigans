/*
 * spi_master.c
 *
 * QNX RPi5 CTI — SPI Master  (single-transfer protocol)
 *
 * Fires a POSIX timer at TIMER_INTERVAL_MS.  On each tick it:
 *   1. Clocks out sizeof(stm32_data_t) dummy bytes in ONE rpi_spi_write_read_data()
 *      call — the STM32 slave simultaneously shifts out a fresh stm32_data_t.
 *   2. Copies the received bytes directly into stm32_data_t (no offset, no
 *      command byte to skip).
 *   3. Captures a CLOCK_REALTIME timestamp the moment the transfer returns.
 *   4. Writes everything into POSIX shared memory (spi_shared_t) so that
 *      the Qt6 app and the protocol-stack process can read it without any
 *      file I/O.
 *
 * Build (QNX aarch64):
 *   aarch64-unknown-nto-qnx8.0.0-gcc -Wall -o spi_master spi_master.c \
 *       -lrpi_spi -lpthread
 */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

#include "rpi_spi.h"
#include "shm_spi.h"

/* ─── User-configurable parameters ─────────────────────────────────────── */
#define BUS               0        /* SPI bus index                        */
#define DEV               0        /* chip-select / device index           */
#define SPI_SPEED_HZ      1000000  /* 1 MHz                                */
#define TIMER_INTERVAL_MS 100      /* fire every 100 ms (10 Hz)            */
/* ─────────────────────────────────────────────────────────────────────── */

/* Compile-time guard — catches any toolchain padding difference.
 * stm32_data_t is defined in shm_spi.h; this confirms it is correct here. */
_Static_assert(sizeof(stm32_data_t) == 15,
               "stm32_data_t must be exactly 15 bytes — check packing on both sides");

/* ═══════════════════════════════════════════════════════════════════════
 *  LOCAL TYPES
 * ═══════════════════════════════════════════════════════════════════════ */

/*
 * spi_record_t — used only inside this process for print_record().
 * Not shared; lives on the stack in timer_handler.
 */
typedef struct
{
    struct timespec rx_time;
    uint32_t        seq;
    stm32_data_t    data;
} spi_record_t;

/* ═══════════════════════════════════════════════════════════════════════
 *  SHARED STATE
 * ═══════════════════════════════════════════════════════════════════════ */
static volatile sig_atomic_t  g_running = 1;
static spi_shared_t          *g_shm     = NULL;
static uint32_t               g_seq     = 0;

/* ═══════════════════════════════════════════════════════════════════════
 *  FORWARD DECLARATIONS
 * ═══════════════════════════════════════════════════════════════════════ */
static void timer_handler(union sigval sv);
static void sig_handler(int sig);
static void print_record(const spi_record_t *r);

/* ═══════════════════════════════════════════════════════════════════════
 *  TIMER CALLBACK  (runs in its own thread every TIMER_INTERVAL_MS)
 * ═══════════════════════════════════════════════════════════════════════ */
static void timer_handler(union sigval sv)
{
    (void)sv;

    /* ── 1. Prepare TX buffer (all zeros — master has nothing to say) ── */
    uint8_t tx[sizeof(stm32_data_t)];
    uint8_t rx[sizeof(stm32_data_t)];
    memset(tx, 0x00, sizeof(tx));

    /* ── 2. Single SPI transfer — exactly sizeof(stm32_data_t) bytes ──
     *
     * The STM32 slave is permanently armed and pre-loaded with a fresh
     * struct.  The instant we assert CS it starts shifting bytes out.
     * No command byte, no inter-phase gap, no race condition.
     */
    int ret = rpi_spi_write_read_data(BUS, DEV, tx, rx, sizeof(stm32_data_t));
    if (ret != SPI_SUCCESS)
    {
        fprintf(stderr, "[timer] rpi_spi_write_read_data failed: %d\n", ret);
        return;
    }

    /* ── 2b. Guarantee CS deasserts before we do anything else ──
     *
     * A 100 µs pause guarantees the STM32 sees a clean CS rising edge,
     * which resets its HAL byte counter and signals the transfer is done.
     * Remove this if you confirm your driver deasserts CS immediately.
     */
    {
        struct timespec cs_gap = {.tv_sec = 0, .tv_nsec = 100000L}; /* 100 µs */
        nanosleep(&cs_gap, NULL);
    }

    /* ── 3. Capture timestamp immediately after transfer + CS gap ── */
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);

    /* ── 4. Copy rx bytes directly into the struct — no offset ──
     *
     * rx[0] is the first byte of field1 (low byte, little-endian).
     * There is no leading command-echo byte to skip.
     */
    stm32_data_t received;
    memcpy(&received, rx, sizeof(stm32_data_t));

    /* ── 5. Write into shared memory — consumers will see this ── */
    sem_wait(&g_shm->lock);
    g_shm->seq     = g_seq++;
    g_shm->rx_time = now;
    g_shm->data    = received;
    sem_post(&g_shm->lock);

    /* ── 6. Print locally (build a stack copy — no need to re-lock) ── */
    spi_record_t record = {
        .rx_time = now,
        .seq     = g_shm->seq - 1,  /* seq was already post-incremented */
        .data    = received
    };
    print_record(&record);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  HELPERS
 * ═══════════════════════════════════════════════════════════════════════ */

static void print_record(const spi_record_t *r)
{
    struct tm t;
    gmtime_r(&r->rx_time.tv_sec, &t);

    printf("[#%05u | %02d:%02d:%02d.%09ld UTC] "
           "f1=%u  f2=%u  f3=%d  f4=%u  f5=0x%02X  f6=%u  f7=%u\n",
           r->seq,
           t.tm_hour, t.tm_min, t.tm_sec, r->rx_time.tv_nsec,
           r->data.field1, r->data.field2, r->data.field3,
           r->data.field4, r->data.field5, r->data.field6,
           r->data.field7);

    fflush(stdout);
}

static void sig_handler(int sig)
{
    (void)sig;
    g_running = 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  MAIN
 * ═══════════════════════════════════════════════════════════════════════ */
int main(void)
{
    int ret;

    /* ── Signal handlers ── */
    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);

    /* ── Driver info ── */
    spi_drvinfo_t drv = {0};
    ret = rpi_spi_get_driver_info(BUS, DEV, &drv);
    if (ret != SPI_SUCCESS)
    {
        fprintf(stderr, "get_driver_info failed: %d\n", ret);
        return EXIT_FAILURE;
    }
    printf("Driver : %s  version: 0x%x\n", drv.name, drv.version);

    /* ── Device info ── */
    spi_devinfo_t dev = {0};
    ret = rpi_spi_get_device_info(BUS, DEV, &dev);
    if (ret != SPI_SUCCESS)
    {
        fprintf(stderr, "get_device_info failed: %d\n", ret);
        return EXIT_FAILURE;
    }
    printf("Device : %s  clkrate: %u Hz  mode: 0x%x\n",
           dev.name, dev.current_clkrate, dev.cfg.mode);

    /* ── Configure SPI ── */
    ret = rpi_spi_configure_device(BUS, DEV, SPI_MODE_WORD_WIDTH_8, SPI_SPEED_HZ);
    if (ret != SPI_SUCCESS)
    {
        fprintf(stderr, "configure_device failed: %d\n", ret);
        return EXIT_FAILURE;
    }
    printf("SPI    : 8-bit, %u Hz, CPOL=0 CPHA=0\n", SPI_SPEED_HZ);

    /* ── Create shared memory ── */
    int shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (shm_fd == -1)
    {
        perror("shm_open");
        rpi_spi_cleanup_device(BUS, DEV);
        return EXIT_FAILURE;
    }

    if (ftruncate(shm_fd, sizeof(spi_shared_t)) == -1)
    {
        perror("ftruncate");
        close(shm_fd);
        shm_unlink(SHM_NAME);
        rpi_spi_cleanup_device(BUS, DEV);
        return EXIT_FAILURE;
    }

    g_shm = mmap(NULL, sizeof(spi_shared_t),
                 PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (g_shm == MAP_FAILED)
    {
        perror("mmap");
        close(shm_fd);
        shm_unlink(SHM_NAME);
        rpi_spi_cleanup_device(BUS, DEV);
        return EXIT_FAILURE;
    }
    close(shm_fd);  /* fd no longer needed after mmap */

    sem_init(&g_shm->lock, 1, 1);  /* pshared=1, initial value=1 (unlocked) */
    g_shm->seq = 0;

    /* ── Create POSIX timer (SIGEV_THREAD → spawns a thread per tick) ── */
    timer_t         timer_id;
    struct sigevent sev;

    memset(&sev, 0, sizeof(sev));
    sev.sigev_notify          = SIGEV_THREAD;
    sev.sigev_notify_function = timer_handler;

    if (timer_create(CLOCK_MONOTONIC, &sev, &timer_id) == -1)
    {
        perror("timer_create");
        sem_destroy(&g_shm->lock);
        munmap(g_shm, sizeof(spi_shared_t));
        shm_unlink(SHM_NAME);
        rpi_spi_cleanup_device(BUS, DEV);
        return EXIT_FAILURE;
    }

    long interval_ns = (long)TIMER_INTERVAL_MS * 1000000L;
    struct itimerspec its = {
        .it_interval = {.tv_sec = 0, .tv_nsec = interval_ns},
        .it_value    = {.tv_sec = 0, .tv_nsec = interval_ns},
    };

    if (timer_settime(timer_id, 0, &its, NULL) == -1)
    {
        perror("timer_settime");
        timer_delete(timer_id);
        sem_destroy(&g_shm->lock);
        munmap(g_shm, sizeof(spi_shared_t));
        shm_unlink(SHM_NAME);
        rpi_spi_cleanup_device(BUS, DEV);
        return EXIT_FAILURE;
    }

    printf("Timer  : armed at %d ms — Ctrl-C to stop\n\n", TIMER_INTERVAL_MS);

    /* ── Main loop — just keeps the process alive ── */
    while (g_running)
    {
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 10000000L}; /* 10 ms */
        nanosleep(&ts, NULL);
    }

    /* ── Cleanup ── */
    printf("\nShutting down...\n");

    /* Print last record from shared memory */
    sem_wait(&g_shm->lock);
    spi_record_t last = {
        .rx_time = g_shm->rx_time,
        .seq     = g_shm->seq,
        .data    = g_shm->data
    };
    sem_post(&g_shm->lock);

    printf("Last record captured:\n  ");
    print_record(&last);

    /* Disarm and delete timer */
    struct itimerspec zero = {0};
    timer_settime(timer_id, 0, &zero, NULL);
    timer_delete(timer_id);

    /* Release shared memory — only the owner unlinks */
    sem_destroy(&g_shm->lock);
    munmap(g_shm, sizeof(spi_shared_t));
    shm_unlink(SHM_NAME);

    rpi_spi_cleanup_device(BUS, DEV);

    printf("Done.\n");
    return EXIT_SUCCESS;
}