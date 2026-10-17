/*
 * Waveshare's CH32V003 I/O expander, as fitted to the ESP32-S3-CAM-OVxxxx.
 *
 * The CH32V003 runs Waveshare firmware that mimics a minimal register-mapped
 * expander. Every access is a register number followed by data:
 *
 *   0x02  direction   bit = 1 makes that EXIO an output
 *   0x03  output      levels driven on output pins
 *   0x04  input       levels read back
 *
 * Board wiring of interest: EXIO4 is the NS4150B speaker amplifier enable.
 *
 * The chip answers at 0x24 -- the same fixed address as a PN532, so the two
 * can never share a bus.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CH32V003_EXIO_ADDR      0x24

typedef struct ch32v003_exio *ch32v003_exio_handle_t;

/**
 * @brief Attach to the expander and load an initial pin state.
 *
 * The output register is written before the direction register, so a pin
 * that becomes an output never glitches through a level it was not given.
 *
 * @param bus      I2C master bus the expander is on.
 * @param scl_hz   Bus speed for this device; 0 selects 400 kHz.
 * @param dir      Direction mask, bit = 1 for output.
 * @param levels   Initial output levels.
 * @param out      Receives the handle.
 */
esp_err_t ch32v003_exio_new(i2c_master_bus_handle_t bus, uint32_t scl_hz,
                            uint8_t dir, uint8_t levels,
                            ch32v003_exio_handle_t *out);

/** @brief Drive one EXIO pin (0..7). Safe from any task. */
esp_err_t ch32v003_exio_set_level(ch32v003_exio_handle_t h, uint8_t pin, bool level);

/** @brief Read all eight input levels. */
esp_err_t ch32v003_exio_get_inputs(ch32v003_exio_handle_t h, uint8_t *levels);

/** @brief Detach from the bus and free the handle. */
void ch32v003_exio_del(ch32v003_exio_handle_t h);

#ifdef __cplusplus
}
#endif
