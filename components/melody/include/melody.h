/*
 * Short melodies on an ES8311 codec driven over I2S.
 *
 * Notes are synthesised on the fly (no audio assets): a sine with its third
 * harmonic for a bit of edge on a small speaker, shaped by a short attack and
 * release so note boundaries do not click.
 *
 * Playback is queued and runs on its own task; melody_play() never blocks.
 * The speaker amplifier is switched through a callback, on just before the
 * first sample and off once the DMA has drained, which keeps the NS4150B
 * silent between melodies.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t freq_hz;   /*!< 0 for a rest */
    uint16_t ms;
} melody_note_t;

typedef enum {
    MELODY_BOOT = 0,    /*!< rising arpeggio, startup                       */
    MELODY_SCAN,        /*!< short blip, card seen                          */
    MELODY_SUCCESS,     /*!< rising major triad, card accepted              */
    MELODY_ERROR,       /*!< falling three-step, card rejected              */
    MELODY_NO_HOST,     /*!< two low beeps, no host answer                  */
    MELODY_COUNT,
} melody_id_t;

/** Switches the speaker amplifier. Called from the melody task. */
typedef void (*melody_amp_cb_t)(bool on, void *ctx);

typedef struct {
    i2c_master_bus_handle_t i2c_bus;    /*!< bus the ES8311 is on (7-bit 0x18) */
    int        i2s_port;
    gpio_num_t mclk;
    gpio_num_t bclk;
    gpio_num_t ws;
    gpio_num_t dout;                    /*!< ESP32 -> codec DSDIN */
    uint32_t   sample_rate;             /*!< MCLK = 256 x this; 16000 suits ES8311 */
    int        volume;                  /*!< 0..100 */
    melody_amp_cb_t amp;                /*!< may be NULL */
    void      *amp_ctx;
    uint32_t   amp_settle_ms;           /*!< amplifier wake-up before audio */
} melody_config_t;

/** @brief Bring up I2S + ES8311 and start the playback task. */
esp_err_t melody_init(const melody_config_t *cfg);

/** @brief Queue a built-in melody. Returns ESP_ERR_NO_MEM if the queue is full. */
esp_err_t melody_play(melody_id_t id);

/**
 * @brief Queue an arbitrary note list.
 *
 * Only the pointer is queued: @p notes must stay valid until it has played,
 * so pass static or const storage.
 */
esp_err_t melody_play_notes(const melody_note_t *notes, size_t count);

/** @brief Drop everything still queued; the melody playing now finishes. */
void melody_flush(void);

/** @brief Codec output volume, 0..100. */
esp_err_t melody_set_volume(int volume);

/** @brief Block until the queue is empty and the amplifier is off. */
esp_err_t melody_wait_idle(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
