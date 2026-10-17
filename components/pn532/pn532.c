/*
 * PN532 over I2C. Frame layout and command set: NXP UM0701-02.
 */

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_check.h"

#include "pn532.h"

static const char *TAG = "pn532";

#define TFI_HOST                    0xD4
#define TFI_PN532                   0xD5
#define TFI_ERROR                   0x7F

#define CMD_GET_FIRMWARE_VERSION    0x02
#define CMD_SAM_CONFIGURATION       0x14
#define CMD_RF_CONFIGURATION        0x32
#define CMD_IN_DATA_EXCHANGE        0x40
#define CMD_IN_LIST_PASSIVE_TARGET  0x4A
#define CMD_IN_RELEASE              0x52

#define RF_CFG_MAX_RETRIES          0x05
#define BRTY_106_TYPE_A             0x00

#define MIFARE_READ                 0x30
#define MIFARE_WRITE                0xA0
#define ULTRALIGHT_WRITE            0xA2

#define I2C_XFER_TIMEOUT_MS         50
#define ACK_TIMEOUT_MS              50
#define CMD_TIMEOUT_MS              200
#define EXCHANGE_TIMEOUT_MS         500
#define BOOT_ATTEMPTS               5

/*
 * LEN covers TFI + data and tops out at 255, so a frame carries at most 254
 * data bytes (command code included). On the wire that data is wrapped in
 * 00 00 FF LEN LCS TFI ... DCS 00, eight bytes of overhead, and every read
 * is prefixed by the status byte.
 */
#define DATA_MAX                    254
#define FRAME_OVERHEAD              8
#define RESP_OVERHEAD               (1 + FRAME_OVERHEAD + 1)   /* RDY + frame + CMD+1 */

/* InListPassiveTarget answer: NbTg Tg SENS_RES(2) SEL_RES NFCIDLen NFCID ATS.
   Sized for a 10-byte UID plus a generous ATS. */
#define LIST_RESP_MAX               96

static const uint8_t ACK_FRAME[6]  = { 0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00 };
static const uint8_t NACK_FRAME[6] = { 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00 };

struct pn532 {
    i2c_master_dev_handle_t dev;
    SemaphoreHandle_t       lock;
    uint8_t                 tx[FRAME_OVERHEAD + DATA_MAX];
    uint8_t                 rx[1 + FRAME_OVERHEAD + DATA_MAX];
};

/* ---------------------------------------------------------- transport --- */

static esp_err_t write_frame(pn532_handle_t h, const uint8_t *data, size_t len)
{
    if (len == 0 || len > DATA_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *f = h->tx;
    size_t n = 0;
    const uint8_t frame_len = (uint8_t)(len + 1);   /* TFI + data */

    f[n++] = 0x00;                                  /* preamble   */
    f[n++] = 0x00;                                  /* start code */
    f[n++] = 0xFF;
    f[n++] = frame_len;
    f[n++] = (uint8_t)(0x100 - frame_len);          /* LCS        */
    f[n++] = TFI_HOST;

    uint8_t sum = TFI_HOST;
    for (size_t i = 0; i < len; i++) {
        f[n++] = data[i];
        sum += data[i];
    }

    f[n++] = (uint8_t)(0x100 - sum);                /* DCS        */
    f[n++] = 0x00;                                  /* postamble  */

    return i2c_master_transmit(h->dev, f, n, I2C_XFER_TIMEOUT_MS);
}

/*
 * Poll the status byte. A PN532 busy with RF may NACK its own address, so a
 * failed read is "not ready yet", never an error in its own right.
 *
 * Granularity is one RTOS tick; at CONFIG_FREERTOS_HZ=100 that is 10 ms.
 */
static esp_err_t wait_ready(pn532_handle_t h, uint32_t timeout_ms)
{
    const TickType_t start = xTaskGetTickCount();
    const TickType_t limit = pdMS_TO_TICKS(timeout_ms) + 1;

    for (;;) {
        uint8_t status = 0;
        if (i2c_master_receive(h->dev, &status, 1, I2C_XFER_TIMEOUT_MS) == ESP_OK &&
            (status & 0x01)) {
            return ESP_OK;
        }
        if ((xTaskGetTickCount() - start) >= limit) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(1);
    }
}

/* Sending an ACK frame to the PN532 aborts whatever command it is running. */
static void abort_command(pn532_handle_t h)
{
    i2c_master_transmit(h->dev, ACK_FRAME, sizeof(ACK_FRAME), I2C_XFER_TIMEOUT_MS);
}

static esp_err_t read_ack(pn532_handle_t h)
{
    ESP_RETURN_ON_ERROR(wait_ready(h, ACK_TIMEOUT_MS), TAG, "no ACK");

    uint8_t buf[1 + sizeof(ACK_FRAME)];
    ESP_RETURN_ON_ERROR(i2c_master_receive(h->dev, buf, sizeof(buf), I2C_XFER_TIMEOUT_MS),
                        TAG, "ACK read failed");

    if (memcmp(buf + 1, ACK_FRAME, sizeof(ACK_FRAME)) == 0) {
        return ESP_OK;
    }
    if (memcmp(buf + 1, NACK_FRAME, sizeof(NACK_FRAME)) == 0) {
        ESP_LOGW(TAG, "command NACKed");
    } else {
        ESP_LOGW(TAG, "garbled ACK %02X %02X %02X %02X %02X %02X",
                 buf[1], buf[2], buf[3], buf[4], buf[5], buf[6]);
    }
    return ESP_ERR_INVALID_RESPONSE;
}

/*
 * Read one response frame for @p cmd. Only cap + overhead bytes are clocked
 * out, so short answers stay short on the bus.
 */
static esp_err_t read_response(pn532_handle_t h, uint8_t cmd,
                               uint8_t *resp, size_t *resp_len)
{
    const size_t cap = *resp_len;
    size_t n = cap + RESP_OVERHEAD;
    if (n > sizeof(h->rx)) {
        n = sizeof(h->rx);
    }

    const uint8_t *f = h->rx;
    ESP_RETURN_ON_ERROR(i2c_master_receive(h->dev, h->rx, n, I2C_XFER_TIMEOUT_MS),
                        TAG, "response read failed");

    if (!(f[0] & 0x01)) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Skip RDY and any preamble zeros up to the 00 FF start code. */
    size_t i = 1;
    while (i + 1 < n && !(f[i] == 0x00 && f[i + 1] == 0xFF)) {
        i++;
    }
    i += 2;
    if (i + 2 > n) {
        ESP_LOGW(TAG, "no start code");
        return ESP_ERR_INVALID_RESPONSE;
    }

    const uint8_t len = f[i];
    if ((uint8_t)(len + f[i + 1]) != 0) {
        ESP_LOGW(TAG, "bad LCS");
        return ESP_ERR_INVALID_CRC;
    }
    i += 2;                                         /* now at TFI */

    if (len == 1 && i < n && f[i] == TFI_ERROR) {
        ESP_LOGW(TAG, "syntax error frame for cmd 0x%02X", cmd);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (len < 2 || i + len + 1 > n) {
        ESP_LOGW(TAG, "frame of %u bytes does not fit %u", len, (unsigned)n);
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t sum = 0;
    for (size_t k = 0; k <= len; k++) {             /* TFI..data plus DCS */
        sum += f[i + k];
    }
    if (sum != 0) {
        ESP_LOGW(TAG, "bad DCS");
        return ESP_ERR_INVALID_CRC;
    }

    if (f[i] != TFI_PN532 || f[i + 1] != (uint8_t)(cmd + 1)) {
        ESP_LOGW(TAG, "unexpected answer %02X %02X to 0x%02X", f[i], f[i + 1], cmd);
        return ESP_ERR_INVALID_RESPONSE;
    }

    const size_t data_len = len - 2u;
    if (data_len > cap) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(resp, f + i + 2, data_len);
    *resp_len = data_len;
    return ESP_OK;
}

/*
 * One full command: frame out, ACK in, wait, response in. Caller holds the
 * lock. @p cmd starts with the command code; @p resp gets what follows the
 * echoed code.
 */
static esp_err_t transceive(pn532_handle_t h, const uint8_t *cmd, size_t cmd_len,
                            uint8_t *resp, size_t *resp_len, uint32_t timeout_ms)
{
    esp_err_t err = write_frame(h, cmd, cmd_len);
    if (err != ESP_OK) {
        return err;
    }

    err = read_ack(h);
    if (err != ESP_OK) {
        return err;
    }

    err = wait_ready(h, timeout_ms);
    if (err != ESP_OK) {
        abort_command(h);
        return err;
    }

    return read_response(h, cmd[0], resp, resp_len);
}

static esp_err_t command(pn532_handle_t h, const uint8_t *cmd, size_t cmd_len,
                         uint8_t *resp, size_t *resp_len, uint32_t timeout_ms)
{
    uint8_t scratch[1];
    size_t scratch_len = 0;
    if (resp == NULL) {
        resp = scratch;
        resp_len = &scratch_len;
    }

    xSemaphoreTake(h->lock, portMAX_DELAY);
    esp_err_t err = transceive(h, cmd, cmd_len, resp, resp_len, timeout_ms);
    xSemaphoreGive(h->lock);
    return err;
}

/* ----------------------------------------------------------- lifecycle --- */

esp_err_t pn532_get_firmware(pn532_handle_t h, uint32_t *version)
{
    ESP_RETURN_ON_FALSE(h && version, ESP_ERR_INVALID_ARG, TAG, "bad args");

    const uint8_t cmd[] = { CMD_GET_FIRMWARE_VERSION };
    uint8_t resp[4];
    size_t len = sizeof(resp);

    ESP_RETURN_ON_ERROR(command(h, cmd, sizeof(cmd), resp, &len, CMD_TIMEOUT_MS),
                        TAG, "GetFirmwareVersion");
    ESP_RETURN_ON_FALSE(len == 4, ESP_ERR_INVALID_RESPONSE, TAG, "short firmware reply");

    *version = ((uint32_t)resp[0] << 24) | ((uint32_t)resp[1] << 16) |
               ((uint32_t)resp[2] << 8)  |  resp[3];
    return ESP_OK;
}

esp_err_t pn532_new(const pn532_config_t *cfg, pn532_handle_t *out)
{
    ESP_RETURN_ON_FALSE(cfg && cfg->bus && out, ESP_ERR_INVALID_ARG, TAG, "bad args");

    pn532_handle_t h = calloc(1, sizeof(*h));
    ESP_RETURN_ON_FALSE(h, ESP_ERR_NO_MEM, TAG, "no mem");

    esp_err_t err = ESP_ERR_NO_MEM;
    h->lock = xSemaphoreCreateMutex();
    if (h->lock == NULL) {
        goto fail;
    }

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = PN532_I2C_ADDR,
        .scl_speed_hz    = cfg->scl_speed_hz ? cfg->scl_speed_hz : 100000,
    };
    err = i2c_master_bus_add_device(cfg->bus, &dev_cfg, &h->dev);
    if (err != ESP_OK) {
        goto fail;
    }

    /* The first transfers after power-up wake the chip and may be lost. */
    uint32_t fw = 0;
    for (int attempt = 0; attempt < BOOT_ATTEMPTS; attempt++) {
        err = pn532_get_firmware(h, &fw);
        if (err == ESP_OK) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "no PN532 at 0x%02X: check wiring, pull-ups, I2C DIP switches",
                 PN532_I2C_ADDR);
        goto fail_dev;
    }
    ESP_LOGI(TAG, "PN5%02X firmware %u.%u, support 0x%02X",
             (unsigned)(fw >> 24), (unsigned)((fw >> 16) & 0xFF),
             (unsigned)((fw >> 8) & 0xFF), (unsigned)(fw & 0xFF));

    /* Normal mode (no SAM), virtual-card timeout 1 s, IRQ pin driven. */
    const uint8_t sam[] = { CMD_SAM_CONFIGURATION, 0x01, 0x14, 0x01 };
    err = command(h, sam, sizeof(sam), NULL, NULL, CMD_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SAMConfiguration failed (%s)", esp_err_to_name(err));
        goto fail_dev;
    }

    /* MxRtyATR, MxRtyPSL left at defaults; passive activation bounded. */
    const uint8_t rf[] = { CMD_RF_CONFIGURATION, RF_CFG_MAX_RETRIES,
                           0xFF, 0x01, cfg->passive_retries };
    err = command(h, rf, sizeof(rf), NULL, NULL, CMD_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RFConfiguration failed (%s)", esp_err_to_name(err));
        goto fail_dev;
    }

    *out = h;
    return ESP_OK;

fail_dev:
    i2c_master_bus_rm_device(h->dev);
fail:
    if (h->lock) {
        vSemaphoreDelete(h->lock);
    }
    free(h);
    return err;
}

void pn532_del(pn532_handle_t h)
{
    if (h == NULL) {
        return;
    }
    i2c_master_bus_rm_device(h->dev);
    vSemaphoreDelete(h->lock);
    free(h);
}

/* ------------------------------------------------------------- targets --- */

static pn532_card_type_t decode_sak(uint8_t sak)
{
    switch (sak) {
    case 0x00:
        return PN532_CARD_ULTRALIGHT;
    case 0x09:
        return PN532_CARD_CLASSIC_MINI;
    case 0x08: case 0x88: case 0x28:
        return PN532_CARD_CLASSIC_1K;
    case 0x18: case 0x98: case 0x38:
        return PN532_CARD_CLASSIC_4K;
    default:
        return (sak & 0x20) ? PN532_CARD_ISO14443_4 : PN532_CARD_UNKNOWN;
    }
}

esp_err_t pn532_read_passive_target(pn532_handle_t h, pn532_card_t *card,
                                    uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(h && card, ESP_ERR_INVALID_ARG, TAG, "bad args");

    const uint8_t cmd[] = { CMD_IN_LIST_PASSIVE_TARGET, 0x01, BRTY_106_TYPE_A };
    uint8_t resp[LIST_RESP_MAX];
    size_t len = sizeof(resp);

    esp_err_t err = command(h, cmd, sizeof(cmd), resp, &len, timeout_ms);
    if (err != ESP_OK) {
        return err;
    }
    if (len < 1 || resp[0] == 0) {
        return ESP_ERR_NOT_FOUND;
    }

    /* NbTg Tg SENS_RES[2] SEL_RES NFCIDLength NFCID1[...] */
    const uint8_t uid_len = (len >= 6) ? resp[5] : 0;
    if (uid_len == 0 || uid_len > PN532_UID_MAX || len < 6u + uid_len) {
        ESP_LOGW(TAG, "malformed target data (%u bytes)", (unsigned)len);
        return ESP_ERR_INVALID_RESPONSE;
    }

    memset(card, 0, sizeof(*card));
    card->tg      = resp[1];
    card->atqa    = (uint16_t)((resp[2] << 8) | resp[3]);
    card->sak     = resp[4];
    card->uid_len = uid_len;
    memcpy(card->uid, resp + 6, uid_len);
    card->type    = decode_sak(card->sak);
    return ESP_OK;
}

esp_err_t pn532_release(pn532_handle_t h)
{
    ESP_RETURN_ON_FALSE(h, ESP_ERR_INVALID_ARG, TAG, "bad args");

    const uint8_t cmd[] = { CMD_IN_RELEASE, 0x00 };
    uint8_t status;
    size_t len = sizeof(status);
    return command(h, cmd, sizeof(cmd), &status, &len, CMD_TIMEOUT_MS);
}

/* --------------------------------------------------------- data path ---- */

static const char *status_text(uint8_t status)
{
    switch (status) {
    case 0x01: return "timeout";
    case 0x02: return "CRC error";
    case 0x03: return "parity error";
    case 0x0A: return "RF collision";
    case 0x13: return "data format";
    case 0x14: return "authentication failed";
    case 0x27: return "wrong context";
    case 0x2B: return "card released";
    default:   return "error";
    }
}

esp_err_t pn532_data_exchange(pn532_handle_t h, uint8_t tg,
                              const uint8_t *tx, size_t tx_len,
                              uint8_t *rx, size_t *rx_len)
{
    ESP_RETURN_ON_FALSE(h && (tx || tx_len == 0), ESP_ERR_INVALID_ARG, TAG, "bad args");
    ESP_RETURN_ON_FALSE(tx_len + 2 <= DATA_MAX, ESP_ERR_INVALID_SIZE, TAG, "tx too long");

    const size_t want = (rx && rx_len) ? *rx_len : 0;
    ESP_RETURN_ON_FALSE(want + 1 <= DATA_MAX, ESP_ERR_INVALID_SIZE, TAG, "rx too long");

    uint8_t cmd[DATA_MAX];
    cmd[0] = CMD_IN_DATA_EXCHANGE;
    cmd[1] = tg;
    if (tx_len) {
        memcpy(cmd + 2, tx, tx_len);
    }

    uint8_t resp[DATA_MAX];
    size_t len = want + 1;                          /* status byte first */

    esp_err_t err = command(h, cmd, tx_len + 2, resp, &len, EXCHANGE_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }
    ESP_RETURN_ON_FALSE(len >= 1, ESP_ERR_INVALID_RESPONSE, TAG, "no status");

    const uint8_t status = resp[0] & 0x3F;
    if (status != 0) {
        ESP_LOGW(TAG, "card: %s (0x%02X)", status_text(status), status);
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (rx && rx_len) {
        memcpy(rx, resp + 1, len - 1);
        *rx_len = len - 1;
    }
    return ESP_OK;
}

static esp_err_t exchange_exact(pn532_handle_t h, const pn532_card_t *card,
                                const uint8_t *tx, size_t tx_len,
                                uint8_t *rx, size_t rx_expect)
{
    size_t got = rx_expect;
    esp_err_t err = pn532_data_exchange(h, card->tg, tx, tx_len,
                                        rx_expect ? rx : NULL, rx_expect ? &got : NULL);
    if (err == ESP_OK && got != rx_expect) {
        ESP_LOGW(TAG, "expected %u bytes, got %u", (unsigned)rx_expect, (unsigned)got);
        return ESP_ERR_INVALID_SIZE;
    }
    return err;
}

/* ---------------------------------------------------- MIFARE Classic ---- */

static bool classic_is_trailer(uint8_t block)
{
    /* 4K: 32 sectors of 4 blocks, then 8 sectors of 16 blocks. */
    return (block < 128) ? ((block & 0x03) == 0x03) : ((block & 0x0F) == 0x0F);
}

esp_err_t pn532_classic_auth(pn532_handle_t h, const pn532_card_t *card,
                             uint8_t block, pn532_key_t key_type,
                             const uint8_t key[6])
{
    ESP_RETURN_ON_FALSE(h && card && key && card->uid_len >= 4,
                        ESP_ERR_INVALID_ARG, TAG, "bad args");

    uint8_t tx[12];
    tx[0] = (uint8_t)key_type;
    tx[1] = block;
    memcpy(tx + 2, key, 6);
    memcpy(tx + 8, card->uid + card->uid_len - 4, 4);

    return exchange_exact(h, card, tx, sizeof(tx), NULL, 0);
}

esp_err_t pn532_classic_read(pn532_handle_t h, const pn532_card_t *card,
                             uint8_t block, uint8_t out[PN532_CLASSIC_BLOCK])
{
    ESP_RETURN_ON_FALSE(h && card && out, ESP_ERR_INVALID_ARG, TAG, "bad args");

    const uint8_t tx[] = { MIFARE_READ, block };
    return exchange_exact(h, card, tx, sizeof(tx), out, PN532_CLASSIC_BLOCK);
}

esp_err_t pn532_classic_write(pn532_handle_t h, const pn532_card_t *card,
                              uint8_t block, const uint8_t data[PN532_CLASSIC_BLOCK])
{
    ESP_RETURN_ON_FALSE(h && card && data, ESP_ERR_INVALID_ARG, TAG, "bad args");
    ESP_RETURN_ON_FALSE(block != 0 && !classic_is_trailer(block),
                        ESP_ERR_NOT_ALLOWED, TAG, "block %u is protected", block);

    uint8_t tx[2 + PN532_CLASSIC_BLOCK];
    tx[0] = MIFARE_WRITE;
    tx[1] = block;
    memcpy(tx + 2, data, PN532_CLASSIC_BLOCK);

    return exchange_exact(h, card, tx, sizeof(tx), NULL, 0);
}

/* ------------------------------------------------ Ultralight / NTAG ----- */

esp_err_t pn532_ntag_read(pn532_handle_t h, const pn532_card_t *card,
                          uint8_t page, uint8_t out[PN532_NTAG_READ])
{
    ESP_RETURN_ON_FALSE(h && card && out, ESP_ERR_INVALID_ARG, TAG, "bad args");

    const uint8_t tx[] = { MIFARE_READ, page };
    return exchange_exact(h, card, tx, sizeof(tx), out, PN532_NTAG_READ);
}

esp_err_t pn532_ntag_write(pn532_handle_t h, const pn532_card_t *card,
                           uint8_t page, const uint8_t data[PN532_NTAG_PAGE])
{
    ESP_RETURN_ON_FALSE(h && card && data, ESP_ERR_INVALID_ARG, TAG, "bad args");
    ESP_RETURN_ON_FALSE(page >= 4, ESP_ERR_NOT_ALLOWED, TAG, "page %u is protected", page);

    uint8_t tx[2 + PN532_NTAG_PAGE];
    tx[0] = ULTRALIGHT_WRITE;
    tx[1] = page;
    memcpy(tx + 2, data, PN532_NTAG_PAGE);

    return exchange_exact(h, card, tx, sizeof(tx), NULL, 0);
}

/* ------------------------------------------------------------ helpers --- */

const char *pn532_card_type_name(pn532_card_type_t type)
{
    switch (type) {
    case PN532_CARD_ULTRALIGHT:   return "Ultralight/NTAG";
    case PN532_CARD_CLASSIC_MINI: return "MIFARE Mini";
    case PN532_CARD_CLASSIC_1K:   return "MIFARE Classic 1K";
    case PN532_CARD_CLASSIC_4K:   return "MIFARE Classic 4K";
    case PN532_CARD_ISO14443_4:   return "ISO14443-4";
    default:                      return "unknown";
    }
}

void pn532_uid_to_hex(const pn532_card_t *card, char out[PN532_UID_HEX_MAX])
{
    static const char HEX[] = "0123456789ABCDEF";
    size_t n = 0;
    for (uint8_t i = 0; i < card->uid_len && i < PN532_UID_MAX; i++) {
        out[n++] = HEX[card->uid[i] >> 4];
        out[n++] = HEX[card->uid[i] & 0x0F];
    }
    out[n] = '\0';
}
