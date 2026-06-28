/*
 * motor_controller.c
 * ----------------------------------------------------------------------------
 * QNX controller / producer node for the predictive-maintenance pipeline.
 * INTERRUPT-DRIVEN build: reads are driven by the data-ready GPIO edge, so every
 * read lands on a fresh, complete frame (no free-running poll => no clock drift,
 * no duplicate/gap churn from being unsynced to the STM32).
 *
 * Single SCHED_FIFO thread that, per data-ready interrupt:
 *   1. reads one fixed-size frame over SPI (rpi_spi driver), full-duplex so the
 *      same transfer also DELIVERS any pending SET_CONFIG command to the STM32,
 *   2. validates magic / version / size / CRC and accounts for sequence gaps,
 *   3. publishes the latest row to the seqlock snapshot (for Qt) and the whole
 *      block to the lock-free ring (for the SOME/IP publisher),
 *   4. inspects frame_header_t.flags / _reserved for an ACK to any command in
 *      flight, and re-queues / clears it accordingly.
 *
 * Configuration is loaded from a JSON file at startup; SIGHUP triggers a
 * reload. Pi-tier fields apply locally; STM-tier fields go out as a SET_CONFIG
 * command and are not considered active here until the STM32 ACKs.
 *
 * GPIO ACCESS POLICY -- IMPORTANT:
 *   All GPIO access goes through the rpi_gpio resource manager (the client API
 *   in rpi_gpio.c / rpi_gpio.h). The server owns the RP1 hardware; this process
 *   does NOT map or poke RP1 registers directly. See the long comment in the
 *   pre-config revision for the history; the conclusion stands.
 *
 * Build (QNX): libc only -- do NOT link -lrt or -lpthread. Compile as C11.
 *   qcc -V<target> -std=gnu11 -O2 motor_controller.c config.c cJSON.c \
 *       rpi_gpio.c rpi_spi.c -lrpi_spi -o motor_controller
 *
 *   Drop cJSON.c + cJSON.h next to the controller source -- single-file
 *   library, no further deps. https://github.com/DaveGamble/cJSON
 *
 * Requires root to raise SCHED_FIFO priority. The rpi_gpio resource manager
 * must already be running (stock image: /dev/gpio is present).
 *
 * Usage:
 *   motor_controller [/path/to/config.json]   (default: /etc/motor/config.json)
 *   kill -HUP $(pidof motor_controller)       (reload after the file changes)
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
#include "rpi_spi.h"        /* rpi_spi_*, SPI_SUCCESS                       */
#include "rpi_gpio.h"       /* rpi_gpio_* client API                        */
#include "config.h"         /* full_config_t, JSON loader, SIGHUP plumbing  */

#ifndef DEFAULT_CONFIG_PATH
#define DEFAULT_CONFIG_PATH "/etc/motor/config.json"
#endif

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
    uint64_t cfg_reloads;
    uint64_t cfg_acks_ok;
    uint64_t cfg_nacks;
} controller_stats_t;

/* ============================ CRC-32/MPEG-2 ==============================
 * init 0xFFFFFFFF, poly 0x04C11DB7, MSB-first, no reflection, no final XOR.
 * Byte-for-byte identical to the STM32 crc32_mpeg2() in motor_send.c, and
 * reused for command frames going the other direction.                       */
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

typedef struct {
    int      pin;
    int      chid;
    int      coid;
    uint64_t poll_ns;
} dataready_t;

static int dataready_read_level(dataready_t *d)
{
    unsigned level = GPIO_LOW;
    if (rpi_gpio_input(d->pin, &level) != GPIO_SUCCESS)
        return 0;
    return (level == GPIO_HIGH) ? 1 : 0;
}

static int dataready_init(dataready_t *d, const pi_config_t *cfg)
{
    d->pin     = cfg->dataready_pin;
    d->chid    = -1;
    d->coid    = -1;
    d->poll_ns = 2u * 1000u * 1000u;   /* 2 ms safety poll */

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
    if (rpi_gpio_setup(d->pin, GPIO_IN) != GPIO_SUCCESS) {
        fprintf(stderr, "error: rpi_gpio_setup(pin=%d) failed\n", d->pin);
        return -1;
    }
    if (rpi_gpio_add_event_detect(d->pin, d->coid, GPIO_RISING, DR_EVENT_ID)
            != GPIO_SUCCESS) {
        fprintf(stderr, "error: rpi_gpio_add_event_detect(pin=%d) failed\n", d->pin);
        return -1;
    }
    if (rpi_gpio_setup_pull(d->pin, GPIO_IN, GPIO_PUD_DOWN) != GPIO_SUCCESS) {
        fprintf(stderr, "warning: rpi_gpio_setup_pull(pin=%d, DOWN) failed -- "
                        "fit an external pull-down resistor\n", d->pin);
    }
    if (dataready_read_level(d) == 1) {
        fprintf(stderr, "WARNING: GPIO%d reads HIGH at startup -- check wiring / "
                        "pull-down (spurious reads possible)\n", d->pin);
    } else {
        fprintf(stderr, "[ctrl] GPIO%d idle level low (pull-down active)\n", d->pin);
    }
    return 0;
}

static int dataready_wait(dataready_t *d, controller_stats_t *st)
{
    (void)st;
    uint64_t to = d->poll_ns;
    struct _pulse pulse;
    TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_RECEIVE, NULL, &to, NULL);
    int rc = MsgReceivePulse(d->chid, &pulse, sizeof pulse, NULL);
    if (rc == -1 && errno != ETIMEDOUT)
        return WR_ERROR;
    return dataready_read_level(d) ? WR_OK : WR_TIMEOUT;
}

static void dataready_cleanup(dataready_t *d)
{
    rpi_gpio_cleanup();
    if (d->coid != -1) ConnectDetach(d->coid);
    if (d->chid != -1) ChannelDestroy(d->chid);
}

/* ============================ command transmitter ========================
 *
 * One outstanding command at a time. The same frame rides every SPI transfer
 * until it is ACK'd, NACK'd, or the retry budget is exhausted. State machine:
 *
 *   IDLE  --cmd_queue()-->  PENDING
 *   PENDING  --ACK_OK-->    IDLE  (cfg.stm copied into "active")
 *   PENDING  --NACK-->      IDLE  (active unchanged, error logged)
 *   PENDING  --retries gone--> IDLE  (active unchanged, error logged)
 *
 * Only the controller's main loop touches g_cmd. No locking needed (single
 * thread).
 */
#define CMD_MAX_RETRIES   32u   /* ~320 ms at 100 Hz block cadence */

typedef enum { CMD_IDLE = 0, CMD_PENDING = 1 } cmd_state_t;

static struct {
    cmd_state_t state;
    uint16_t    seq;            /* monotonic, sent in cmd_header_t.cmd_seq    */
    uint16_t    retries_left;
    uint8_t     frame[MOTOR_CMD_FRAME_BYTES];
    config_payload_t payload;   /* what we asked for (in flight)              */
} g_cmd;

static void cmd_init(void)
{
    memset(&g_cmd, 0, sizeof g_cmd);
    g_cmd.state = CMD_IDLE;
    g_cmd.seq   = 0;
}

/* Build a SET_CONFIG frame in g_cmd.frame for the given payload. */
static void cmd_build_set_config(const config_payload_t *p)
{
    memset(g_cmd.frame, 0, sizeof g_cmd.frame);
    cmd_header_t *h = (cmd_header_t *)g_cmd.frame;
    h->magic          = MOTOR_CMD_MAGIC;
    h->cmd            = MOTOR_CMD_SET_CONFIG;
    h->schema_version = MOTOR_CONFIG_SCHEMA_VERSION;
    h->cmd_seq        = ++g_cmd.seq;
    h->_pad           = 0;

    config_payload_t *body = (config_payload_t *)(g_cmd.frame + sizeof(cmd_header_t));
    *body = *p;
    memset(body->reserved, 0, sizeof body->reserved);

    size_t covered = sizeof(cmd_header_t) + sizeof(config_payload_t);
    uint32_t crc = frame_crc_compute(g_cmd.frame, covered);
    memcpy(g_cmd.frame + covered, &crc, sizeof crc);
}

static void cmd_queue_set_config(const config_payload_t *p)
{
    g_cmd.payload      = *p;
    cmd_build_set_config(p);
    g_cmd.state        = CMD_PENDING;
    g_cmd.retries_left = CMD_MAX_RETRIES;
    fprintf(stderr, "[ctrl] SET_CONFIG queued seq=%u (block_rows=%u, run=%u)\n",
            g_cmd.seq, g_cmd.payload.block_rows, g_cmd.payload.run_state);
}

/* Fill the outbound tx for one SPI exchange. Does NOT decrement retries --
 * that happens only after an exchange actually completes (see main loop). */
static void cmd_fill_tx(uint8_t *tx, size_t tx_len)
{
    memset(tx, 0, tx_len);
    if (g_cmd.state == CMD_PENDING) {
        memcpy(tx, g_cmd.frame, sizeof g_cmd.frame);
    }
}

/* Call once per successful SPI exchange (i.e. the command was actually
 * clocked out). Decrements the retry budget for the in-flight command.   */
static void cmd_count_attempt(void)
{
    if (g_cmd.state == CMD_PENDING && g_cmd.retries_left > 0)
        g_cmd.retries_left--;
}

/* Inspect an incoming frame header for an ACK. Returns true and updates
 * *active_stm on a successful apply (so the caller can lock its expectations
 * to the new config). */
static bool cmd_observe(const frame_header_t *h,
                        config_payload_t *active_stm,
                        controller_stats_t *st)
{
    if (g_cmd.state != CMD_PENDING) return false;
    if (h->_reserved != g_cmd.seq) {
        /* Not our ACK (could be a stale ack from a previous seq). Check the
         * retry budget. */
        if (g_cmd.retries_left == 0) {
            fprintf(stderr, "[ctrl] SET_CONFIG seq=%u: no ACK after retries, giving up\n",
                    g_cmd.seq);
            st->cfg_nacks++;
            g_cmd.state = CMD_IDLE;
        }
        return false;
    }
    /* This ACK matches our in-flight command. */
    if (h->flags & MOTOR_FLAG_ACK_OK) {
        *active_stm = g_cmd.payload;       /* now authoritative on the Pi side */
        g_cmd.state = CMD_IDLE;
        st->cfg_acks_ok++;
        fprintf(stderr, "[ctrl] SET_CONFIG seq=%u ACK_OK%s\n", g_cmd.seq,
                (h->flags & MOTOR_FLAG_CONFIG_APPLIED) ? " (config applied)" : "");
        return true;
    }
    if (h->flags & MOTOR_FLAG_ACK_NACK) {
        fprintf(stderr, "[ctrl] SET_CONFIG seq=%u NACK%s%s%s%s\n", g_cmd.seq,
                (h->flags & MOTOR_FLAG_NACK_RANGE) ? " RANGE"  : "",
                (h->flags & MOTOR_FLAG_NACK_CRC)   ? " CRC"    : "",
                (h->flags & MOTOR_FLAG_NACK_VER)   ? " VER"    : "",
                (h->flags & MOTOR_FLAG_NACK_CMD)   ? " CMD"    : "");
        st->cfg_nacks++;
        g_cmd.state = CMD_IDLE;
        return false;
    }
    /* ACK seq matches but no ack bits set yet -- the apply is still in
     * progress. Keep the command in tx; we'll see the bits in a later frame. */
    return false;
}

/* ============================ pi-tier apply ==============================
 * Things we can change live without restarting. Right now: real-time priority.
 * The SPI bus/dev/mode and the dataready pin are pinned at startup -- changing
 * them at runtime would require tearing down the SPI/GPIO setup and is out of
 * scope for this version. The loader will accept the new values; they will
 * take effect on the next process restart.                                   */
static void pi_apply_live(const pi_config_t *pi, const pi_config_t *prev)
{
    if (pi->rt_priority != prev->rt_priority) {
        if (set_realtime_priority(pi->rt_priority) != 0) {
            fprintf(stderr, "[ctrl] rt_priority %d not set: %s\n",
                    pi->rt_priority, strerror(errno));
        } else {
            fprintf(stderr, "[ctrl] rt_priority -> %d\n", pi->rt_priority);
        }
    }
    /* Scaling constants are read by downstream consumers from shm; the
     * publisher just needs to expose them. (Hookup TBD; not on the wire.) */
    (void)prev;
}

/* ============================ shutdown ==================================== */
static volatile sig_atomic_t g_running = 1;
static void on_signal(int sig) { (void)sig; g_running = 0; }

/* ============================ main ======================================== */
int main(int argc, char **argv)
{
    const char *cfg_path = (argc > 1) ? argv[1] : DEFAULT_CONFIG_PATH;

    /* ---- load config (fall back to compiled defaults on failure) ---- */
    full_config_t cfg;
    if (config_load_file(cfg_path, &cfg) == 0) {
        fprintf(stderr, "[ctrl] loaded config: %s\n", cfg_path);
    } else {
        fprintf(stderr, "[ctrl] config load failed; using built-in defaults\n");
        cfg = CONFIG_DEFAULTS;
    }

    /* The "active" STM config is what we believe the STM32 is currently
     * running. It starts as a sentinel (block_rows = 0) meaning "unknown" --
     * we will sync via SET_CONFIG before locking down the size check.        */
    config_payload_t active_stm = {0};

    if (set_realtime_priority(cfg.pi.rt_priority) != 0)
        fprintf(stderr, "warning: SCHED_FIFO prio %d not set (need privilege): %s\n",
                cfg.pi.rt_priority, strerror(errno));

    config_install_sighup();

    shm_region_t *region = shm_setup();
    if (!region) { perror("shm_setup"); return 1; }

    if (rpi_spi_configure_device(cfg.pi.spi_bus, cfg.pi.spi_dev, cfg.pi.spi_mode,
                                 cfg.pi.spi_clock_hz) != SPI_SUCCESS) {
        fprintf(stderr, "rpi_spi_configure_device failed\n");
        munmap(region, sizeof(shm_region_t));
        shm_unlink(MOTOR_SHM_NAME);
        return 1;
    }

    dataready_t dr;
    if (dataready_init(&dr, &cfg.pi) != 0) {
        dataready_cleanup(&dr);
        rpi_spi_cleanup_device(cfg.pi.spi_bus, cfg.pi.spi_dev);
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

    /* The SPI exchange size is the WORST case -- buffers are sized to MAX so
     * a runtime block_rows change does not require a reallocation. We always
     * clock MOTOR_MAX_FRAME_BYTES; the actual valid payload length comes from
     * h->n_rows inside the frame. This also gives the command frame plenty of
     * headroom on the tx side (it's far smaller than MAX).                   */
    const size_t xfer_bytes = MOTOR_MAX_FRAME_BYTES;

    static _Alignas(8) uint8_t rx[MOTOR_MAX_FRAME_BYTES];
    static _Alignas(8) uint8_t tx[MOTOR_MAX_FRAME_BYTES];

    /* ---- queue an initial SET_CONFIG so the STM32 matches what we loaded - */
    cmd_init();
    cmd_queue_set_config(&cfg.stm);

    controller_stats_t st = {0};
    uint32_t last_seq = 0;
    int      have_last = 0;
    uint16_t last_n_rows = 0;
    uint16_t last_h_flags = 0;       /* diagnostic: last received header  */
    uint16_t last_h_reserved = 0;
    struct timespec t_log;
    clock_gettime(CLOCK_MONOTONIC, &t_log);

    while (g_running) {
        /* ---- periodic status log ---- */
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec != t_log.tv_sec) {
            t_log = now;
            fprintf(stderr,
                "[ctrl] ok=%" PRIu64 " drops=%" PRIu64 " crc=%" PRIu64
                " magic=%" PRIu64 " ver=%" PRIu64 " size=%" PRIu64
                " dup=%" PRIu64 " rst=%" PRIu64 " to=%" PRIu64
                " spi=%" PRIu64 " cfg(rld=%" PRIu64 " ack=%" PRIu64 " nack=%" PRIu64 ")"
                " last(flags=0x%04x rsv=%u)\n",
                st.frames_ok, st.seq_drops, st.crc_err, st.magic_err,
                st.version_err, st.size_err, st.duplicates, st.resets,
                st.timeouts, st.spi_err,
                st.cfg_reloads, st.cfg_acks_ok, st.cfg_nacks,
                last_h_flags, last_h_reserved);
        }

        /* ---- SIGHUP: reload, diff, queue ---- */
        if (config_reload_requested()) {
            full_config_t next;
            if (config_load_file(cfg_path, &next) == 0) {
                pi_apply_live(&next.pi, &cfg.pi);
                cfg.pi = next.pi;
                if (config_stm_differs(&next.stm, &cfg.stm)) {
                    cfg.stm = next.stm;
                    cmd_queue_set_config(&cfg.stm);
                } else {
                    fprintf(stderr, "[ctrl] reload: pi-only changes\n");
                }
                st.cfg_reloads++;
            } else {
                fprintf(stderr, "[ctrl] reload failed; keeping previous config\n");
            }
        }

        /* ---- prepare tx (carries SET_CONFIG if one is in flight) ---- */
        cmd_fill_tx(tx, sizeof tx);

        /* ---- wait for data-ready ---- */
        int w = dataready_wait(&dr, &st);
        if (w == WR_TIMEOUT) { st.timeouts++; continue; }
        if (w == WR_ERROR)   { if (errno == EINTR) continue; st.timeouts++; continue; }

        /* ---- SPI exchange ---- */
        if (rpi_spi_write_read_data(cfg.pi.spi_bus, cfg.pi.spi_dev, tx, rx,
                                    xfer_bytes) != SPI_SUCCESS) {
            st.spi_err++;
            continue;
        }

        /* Command (if any) was actually clocked out -- count this attempt. */
        cmd_count_attempt();

        const frame_header_t *h = (const frame_header_t *)rx;

        /* ---- frame validation ---- */
        if (h->magic   != MOTOR_FRAME_MAGIC)      { st.magic_err++;   continue; }
        if (h->version != MOTOR_CONTRACT_VERSION) { st.version_err++; continue; }
        if (h->n_rows  == 0 || h->n_rows > MOTOR_MAX_ROWS_PER_BLOCK) {
            st.size_err++; continue;
        }
        /* If we have a locked-in block_rows (i.e. the STM has ACK'd our
         * config at least once), enforce it -- unless this very frame
         * is the CONFIG_APPLIED transition, which is allowed to differ. */
        if (active_stm.block_rows != 0
            && h->n_rows != active_stm.block_rows
            && !(h->flags & MOTOR_FLAG_CONFIG_APPLIED)) {
            st.size_err++; continue;
        }

        size_t covered = sizeof(frame_header_t) + (size_t)h->n_rows * sizeof(motor_row_t);
        if (covered + sizeof(frame_crc_t) > xfer_bytes) { st.size_err++; continue; }

        uint32_t rx_crc;
        memcpy(&rx_crc, rx + covered, sizeof rx_crc);
        if (frame_crc_compute(rx, covered) != rx_crc) { st.crc_err++; continue; }

        /* ---- ACK handling: may update active_stm ---- */
        cmd_observe(h, &active_stm, &st);

        /* Stash for the periodic log -- shows what the STM is actually
         * putting in flags/_reserved so we can see whether the command
         * round-trip is happening even if cmd_observe never matches.       */
        last_h_flags    = h->flags;
        last_h_reserved = h->_reserved;

        /* If we saw CONFIG_APPLIED, log the size shift so it's easy to see
         * a runtime block_rows change land in the logs. */
        if ((h->flags & MOTOR_FLAG_CONFIG_APPLIED) && h->n_rows != last_n_rows) {
            fprintf(stderr, "[ctrl] block_rows -> %u\n", h->n_rows);
        }
        last_n_rows = h->n_rows;

        /* ---- sequence accounting ---- */
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

        /* ---- publish ---- */
        if (publish) {
            const motor_row_t *rows = (const motor_row_t *)(rx + sizeof(frame_header_t));
            motor_snapshot_publish(&region->snapshot, &rows[h->n_rows - 1],
                                   h->seq, h->timestamp, h->flags);
            motor_ring_publish(&region->ring, h, rows);
            st.frames_ok++;
        }
    }

    dataready_cleanup(&dr);
    rpi_spi_cleanup_device(cfg.pi.spi_bus, cfg.pi.spi_dev);
    munmap(region, sizeof(shm_region_t));
    shm_unlink(MOTOR_SHM_NAME);
    fprintf(stderr, "[ctrl] shutdown: ok=%" PRIu64 " drops=%" PRIu64
                    " crc=%" PRIu64 " cfg_ack=%" PRIu64 " cfg_nack=%" PRIu64 "\n",
            st.frames_ok, st.seq_drops, st.crc_err,
            st.cfg_acks_ok, st.cfg_nacks);
    return 0;
}