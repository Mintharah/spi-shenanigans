/*
 * config.h
 * ----------------------------------------------------------------------------
 * Pi-side runtime configuration: loaded from a JSON file on disk and split
 * into two tiers --
 *
 *   pi:  applied locally by the controller (SPI bus IDs, RT priority,
 *        data-ready pin, scaling constants). Never sent over the wire.
 *
 *   stm: pushed to the STM32 via SET_CONFIG and acknowledged before it
 *        is considered active here. Layout is config_payload_t from
 *        motor_wire.h -- compiled into both binaries.
 *
 * The loader rejects the WHOLE file on any single bad field; we never
 * half-apply.
 * ----------------------------------------------------------------------------
 */
#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>
#include <stdbool.h>
#include "motor_wire.h"

/* ---- Pi-only tier --------------------------------------------------------
 * Mirrors the existing controller_config_t in motor_controller.c, plus the
 * downstream scaling constants that motor_wire.h promises live on the Pi.    */
typedef struct {
    int      spi_bus;
    int      spi_dev;
    int      spi_mode;
    uint32_t spi_clock_hz;
    int      rt_priority;
    int      dataready_pin;

    /* scaling: engineering units = raw * scale + offset. Applied downstream
     * (Qt / SOME/IP publisher). The controller just stores them in shm so
     * the consumers can pick them up.                                       */
    float    current_scale;  float current_offset;
    float    vib_scale;      float vib_offset;
    float    rpm_scale;      float rpm_offset;
} pi_config_t;

/* ---- the full config the controller carries at runtime ------------------ */
typedef struct {
    pi_config_t       pi;
    config_payload_t  stm;     /* mirrored to the STM32                     */
} full_config_t;

/* ---- defaults (used if no config file is present at startup) ------------ */
extern const full_config_t CONFIG_DEFAULTS;

/* ---- API -----------------------------------------------------------------
 * config_load_file:   parse `path`, validate every field, fill *out.
 *                     Returns 0 on success, -1 on any parse / range error
 *                     (and writes a diagnostic to stderr). On failure *out
 *                     is left untouched -- callers can keep the previous
 *                     known-good config.
 *
 * config_stm_differs: true if the STM-tier subset of `a` differs from `b`.
 *                     Used to decide whether a SIGHUP reload needs to push
 *                     a new SET_CONFIG (vs. only Pi-local changes).
 *
 * config_install_sighup: install the SIGHUP handler that sets the global
 *                     reload flag. Call once from main.
 *
 * config_reload_requested: returns true (and clears the flag) when SIGHUP
 *                     has fired since the last call. Polled by the main
 *                     loop between data-ready waits.
 */
int  config_load_file(const char *path, full_config_t *out);
bool config_stm_differs(const config_payload_t *a, const config_payload_t *b);
void config_install_sighup(void);
bool config_reload_requested(void);

#endif /* CONFIG_H */
