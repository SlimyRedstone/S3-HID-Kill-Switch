/*
 * NXP PN532 NFC controller over I2C (ISO14443A / MIFARE reader mode).
 *
 * Transport (UM0701-02 section 6.2.4): the host writes normal information
 * frames, then polls a one-byte status (bit 0 = ready) that prefixes every
 * read. Each command is ACKed first and answered second.
 *
 *   host -> PN532   00 00 FF LEN LCS D4 CMD data.. DCS 00
 *   PN532 -> host   RDY 00 00 FF LEN LCS D5 CMD+1 data.. DCS 00
 *
 * The I2C address is fixed at 0x24 and cannot be strapped. Module DIP
 * switches (Elechouse V3 style): SW1 = ON, SW2 = OFF selects I2C.
 *
 * All calls are serialised on an internal mutex, so any task may use them.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PN532_I2C_ADDR          0x24
#define PN532_UID_MAX           10      /* triple-size ISO14443A UID */
#define PN532_UID_HEX_MAX       (PN532_UID_MAX * 2 + 1)

#define PN532_CLASSIC_BLOCK     16      /* bytes per MIFARE Classic block   */
#define PN532_NTAG_PAGE         4       /* bytes per Ultralight / NTAG page */
#define PN532_NTAG_READ         16      /* READ returns four pages          */

typedef struct pn532 *pn532_handle_t;

typedef struct {
    i2c_master_bus_handle_t bus;
    uint32_t scl_speed_hz;      /*!< 0 selects 100 kHz */
    /*!
     * MxRtyPassiveActivation: how many times InListPassiveTarget retries
     * before reporting "no card". 0xFF retries forever and blocks the
     * command until a card shows up, so keep it finite for polling.
     */
    uint8_t passive_retries;
} pn532_config_t;

#define PN532_DEFAULT_CONFIG(_bus) ((pn532_config_t){ \
    .bus             = (_bus),                         \
    .scl_speed_hz    = 100000,                         \
    .passive_retries = 0x05,                           \
})

/* Rough family, decoded from SAK (NXP AN10833). */
typedef enum {
    PN532_CARD_UNKNOWN = 0,
    PN532_CARD_ULTRALIGHT,      /*!< SAK 0x00: Ultralight, NTAG21x            */
    PN532_CARD_CLASSIC_MINI,    /*!< SAK 0x09                                 */
    PN532_CARD_CLASSIC_1K,      /*!< SAK 0x08 / 0x88 / 0x28                   */
    PN532_CARD_CLASSIC_4K,      /*!< SAK 0x18 / 0x98 / 0x38                   */
    PN532_CARD_ISO14443_4,      /*!< SAK bit 5: DESFire, Plus SL3, phone HCE  */
} pn532_card_type_t;

typedef struct {
    uint8_t  uid[PN532_UID_MAX];
    uint8_t  uid_len;           /*!< 4, 7 or 10 */
    uint16_t atqa;              /*!< SENS_RES as received, first byte high */
    uint8_t  sak;               /*!< SEL_RES */
    uint8_t  tg;                /*!< logical target number assigned by the PN532 */
    pn532_card_type_t type;
} pn532_card_t;

typedef enum {
    PN532_KEY_A = 0x60,
    PN532_KEY_B = 0x61,
} pn532_key_t;

/**
 * @brief Attach, wake the chip, and configure it as a reader.
 *
 * Runs GetFirmwareVersion (retried, the PN532 may still be booting),
 * SAMConfiguration (normal mode) and RFConfiguration (retry counts).
 */
esp_err_t pn532_new(const pn532_config_t *cfg, pn532_handle_t *out);

void pn532_del(pn532_handle_t h);

/**
 * @brief Firmware as reported by GetFirmwareVersion.
 *
 * Packed IC << 24 | Ver << 16 | Rev << 8 | Support. A PN532 v1.6 reads
 * 0x32010607.
 */
esp_err_t pn532_get_firmware(pn532_handle_t h, uint32_t *version);

/**
 * @brief Look for one ISO14443A target (InListPassiveTarget, 106 kbps).
 *
 * The target stays selected afterwards, so the card commands below can
 * follow directly.
 *
 * @return ESP_OK with @p card filled, ESP_ERR_NOT_FOUND when no card
 *         answered within the configured retries, or a transport error.
 */
esp_err_t pn532_read_passive_target(pn532_handle_t h, pn532_card_t *card,
                                    uint32_t timeout_ms);

/** @brief Deselect and release every target (InRelease 0). */
esp_err_t pn532_release(pn532_handle_t h);

/**
 * @brief Raw InDataExchange with a selected target.
 *
 * @param tx       Bytes for the card (the PN532 adds CRC and framing).
 * @param rx       Card response, status byte stripped.
 * @param rx_len   In: capacity of @p rx. Out: bytes received.
 *
 * @return ESP_ERR_INVALID_RESPONSE if the PN532 reports a card-side error
 *         (timeout, CRC, NAK, authentication); the status is logged.
 */
esp_err_t pn532_data_exchange(pn532_handle_t h, uint8_t tg,
                              const uint8_t *tx, size_t tx_len,
                              uint8_t *rx, size_t *rx_len);

/* ----------------------------------------------------- MIFARE Classic --- */

/**
 * @brief Authenticate one sector, identified by any block inside it.
 *
 * Uses the last four UID bytes, which is what Crypto1 expects for both
 * 4-byte and 7-byte UID cards. A failed authentication halts the card:
 * call pn532_read_passive_target() again before retrying.
 */
esp_err_t pn532_classic_auth(pn532_handle_t h, const pn532_card_t *card,
                             uint8_t block, pn532_key_t key_type,
                             const uint8_t key[6]);

esp_err_t pn532_classic_read(pn532_handle_t h, const pn532_card_t *card,
                             uint8_t block, uint8_t out[PN532_CLASSIC_BLOCK]);

/**
 * @brief Write one data block.
 *
 * Refuses block 0 (manufacturer) and sector trailers with
 * ESP_ERR_NOT_ALLOWED: a bad trailer locks the sector for good. Use
 * pn532_data_exchange() with 0xA0 to write those deliberately.
 */
esp_err_t pn532_classic_write(pn532_handle_t h, const pn532_card_t *card,
                              uint8_t block, const uint8_t data[PN532_CLASSIC_BLOCK]);

/* ------------------------------------------------ Ultralight / NTAG2xx --- */

/** @brief READ: four consecutive pages starting at @p page (wraps at the end). */
esp_err_t pn532_ntag_read(pn532_handle_t h, const pn532_card_t *card,
                          uint8_t page, uint8_t out[PN532_NTAG_READ]);

/**
 * @brief WRITE one page.
 *
 * Refuses pages 0..3 (UID, static lock bytes, capability container) with
 * ESP_ERR_NOT_ALLOWED: lock and OTP bits there are irreversible.
 */
esp_err_t pn532_ntag_write(pn532_handle_t h, const pn532_card_t *card,
                           uint8_t page, const uint8_t data[PN532_NTAG_PAGE]);

/* ------------------------------------------------------------ helpers --- */

const char *pn532_card_type_name(pn532_card_type_t type);

/** @brief UID as upper-case hex without separators, e.g. "04A1B2C3D4E5F6". */
void pn532_uid_to_hex(const pn532_card_t *card, char out[PN532_UID_HEX_MAX]);

#ifdef __cplusplus
}
#endif
