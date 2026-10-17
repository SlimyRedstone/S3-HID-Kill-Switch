/*
 * S3 KillSwitch firmware.
 *
 *   PN532 (I2C_NUM_1) --UID--> usb_proto {"card":{...}} --> host app
 *   host app {"set":{"card":{"uid":..,"valid":..}}} --> success / error melody
 *
 * The board bus (I2C_NUM_0, GPIO8/7) carries the CH32V003 expander, whose
 * EXIO4 switches the speaker amplifier, and the ES8311 codec.
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "driver/i2c_master.h"

#include "ch32v003_exio.h"
#include "melody.h"
#include "pn532.h"
#include "usb_proto.h"

static const char *TAG = "killswitch";

#if (CONFIG_KS_PN532_SDA == CONFIG_KS_BOARD_I2C_SDA) || \
    (CONFIG_KS_PN532_SDA == CONFIG_KS_BOARD_I2C_SCL) || \
    (CONFIG_KS_PN532_SCL == CONFIG_KS_BOARD_I2C_SDA) || \
    (CONFIG_KS_PN532_SCL == CONFIG_KS_BOARD_I2C_SCL)
#error "PN532 and the CH32V003 expander both sit at I2C 0x24: give the PN532 its own pins"
#endif

#define BOARD_I2C_PORT      I2C_NUM_0
#define PN532_I2C_PORT      I2C_NUM_1
#define I2S_PORT            0

#define SAMPLE_RATE_HZ      16000
#define AMP_SETTLE_MS       20

/*
 * Expander state left by Waveshare's bsp_io_expander_init(): all outputs,
 * EXIO0/1/2/5/6 high (LCD/touch reset lines, power and LED), amplifier off.
 */
#define EXIO_DIR            0xFF
#define EXIO_BOOT_LEVELS    ((uint8_t)(0x67 & ~(1u << CONFIG_KS_SPEAKER_AMP_EXIO)))

/* InListPassiveTarget answer time; the configured retries finish well inside. */
#define PN532_READ_TIMEOUT_MS   300
#define PN532_RETRY_MS          2000

typedef struct {
    char uid[PN532_UID_HEX_MAX];
    bool valid;
} verdict_t;

static ch32v003_exio_handle_t s_exio;
static QueueHandle_t          s_verdicts;
static bool                   s_audio_ok;
static char                   s_usb_serial[13];

/* ------------------------------------------------------------ audio ----- */

static void speaker_amp(bool on, void *ctx)
{
    (void)ctx;
    if (s_exio) {
        ch32v003_exio_set_level(s_exio, CONFIG_KS_SPEAKER_AMP_EXIO, on);
    }
}

static void play(melody_id_t id)
{
    if (s_audio_ok) {
        melody_play(id);
    }
}

/* ---------------------------------------------------------------- USB --- */

/* Runs on the usb_proto dispatch task: hand off, never block. */
static esp_err_t on_card_verdict(const char *uid, bool valid)
{
    verdict_t v = { .valid = valid };
    strlcpy(v.uid, uid, sizeof(v.uid));
    return xQueueSend(s_verdicts, &v, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

static void usb_start(void)
{
    uint8_t mac[6] = { 0 };
    esp_efuse_mac_get_default(mac);
    snprintf(s_usb_serial, sizeof(s_usb_serial), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    usb_proto_config_t cfg = USB_PROTO_DEFAULT_CONFIG();
    cfg.cdc_echo        = false;
    cfg.vid             = CONFIG_KS_USB_VID;
    cfg.pid             = CONFIG_KS_USB_PID;
    cfg.bcd_device      = CONFIG_KS_USB_BCD;
    cfg.manufacturer    = CONFIG_KS_USB_MANUFACTURER;
    cfg.product         = CONFIG_KS_USB_PRODUCT;
    cfg.serial          = s_usb_serial;
    cfg.on_card_verdict = on_card_verdict;

    ESP_ERROR_CHECK(usb_proto_start(&cfg));
}

/* ---------------------------------------------------------------- NFC --- */

/*
 * Report one card and turn the host's answer into a melody. Blocks the NFC
 * task for at most CONFIG_KS_VERDICT_TIMEOUT_MS, which is fine: nothing else
 * should be read while a verdict is pending.
 */
static void handle_card(const pn532_card_t *card, const char *uid)
{
#if CONFIG_KS_SCAN_BLIP
    play(MELODY_SCAN);
#endif

    if (!usb_proto_vendor_mounted()) {
        ESP_LOGW(TAG, "no host on the vendor interface");
        play(MELODY_NO_HOST);
        return;
    }

    xQueueReset(s_verdicts);    /* stale answers belong to earlier cards */

    esp_err_t err = usb_proto_send_card(uid, card->atqa, card->sak,
                                        pn532_card_type_name(card->type));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "card event not queued (%s)", esp_err_to_name(err));
        play(MELODY_NO_HOST);
        return;
    }

    const TickType_t start = xTaskGetTickCount();
    const TickType_t limit = pdMS_TO_TICKS(CONFIG_KS_VERDICT_TIMEOUT_MS);
    TickType_t elapsed;

    while ((elapsed = xTaskGetTickCount() - start) < limit) {
        verdict_t v;
        if (xQueueReceive(s_verdicts, &v, limit - elapsed) != pdTRUE) {
            break;
        }
        if (strcasecmp(v.uid, uid) != 0) {
            ESP_LOGW(TAG, "verdict for %s ignored, waiting on %s", v.uid, uid);
            continue;
        }

        ESP_LOGI(TAG, "card %s %s", uid, v.valid ? "ACCEPTED" : "REJECTED");
        play(v.valid ? MELODY_SUCCESS : MELODY_ERROR);
        return;
    }

    ESP_LOGW(TAG, "no verdict for %s within %d ms", uid, CONFIG_KS_VERDICT_TIMEOUT_MS);
    play(MELODY_NO_HOST);
}

static pn532_handle_t nfc_connect(void)
{
    i2c_master_bus_handle_t bus = NULL;
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port          = PN532_I2C_PORT,
        .sda_io_num        = CONFIG_KS_PN532_SDA,
        .scl_io_num        = CONFIG_KS_PN532_SCL,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));

    pn532_config_t cfg = PN532_DEFAULT_CONFIG(bus);
    cfg.scl_speed_hz    = CONFIG_KS_PN532_I2C_HZ;
    cfg.passive_retries = CONFIG_KS_PN532_PASSIVE_RETRIES;

    /* Keep trying: a reader plugged in late should still come up. */
    pn532_handle_t nfc = NULL;
    bool warned = false;
    while (pn532_new(&cfg, &nfc) != ESP_OK) {
        if (!warned) {
            play(MELODY_ERROR);
            warned = true;
        }
        vTaskDelay(pdMS_TO_TICKS(PN532_RETRY_MS));
    }
    return nfc;
}

static void nfc_task(void *arg)
{
    (void)arg;

    pn532_handle_t nfc = nfc_connect();
    ESP_LOGI(TAG, "PN532 on SDA=%d SCL=%d, polling every %d ms",
             CONFIG_KS_PN532_SDA, CONFIG_KS_PN532_SCL, CONFIG_KS_PN532_POLL_MS);

    char present[PN532_UID_HEX_MAX] = "";   /* card currently in the field */
    int  misses = 0;

    for (;;) {
        pn532_card_t card;
        esp_err_t err = pn532_read_passive_target(nfc, &card, PN532_READ_TIMEOUT_MS);

        if (err != ESP_OK) {
            if (err != ESP_ERR_NOT_FOUND && err != ESP_ERR_TIMEOUT) {
                ESP_LOGW(TAG, "poll failed (%s)", esp_err_to_name(err));
            }
            if (present[0] != '\0' && ++misses >= CONFIG_KS_CARD_GONE_POLLS) {
                ESP_LOGI(TAG, "card %s removed", present);
                present[0] = '\0';
            }
            vTaskDelay(pdMS_TO_TICKS(CONFIG_KS_PN532_POLL_MS));
            continue;
        }

        misses = 0;

        char uid[PN532_UID_HEX_MAX];
        pn532_uid_to_hex(&card, uid);

        if (strcmp(uid, present) != 0) {
            strlcpy(present, uid, sizeof(present));
            ESP_LOGI(TAG, "card %s  ATQA %04X  SAK %02X  %s", uid,
                     card.atqa, card.sak, pn532_card_type_name(card.type));
            handle_card(&card, uid);
        }

        vTaskDelay(pdMS_TO_TICKS(CONFIG_KS_PN532_POLL_MS));
    }
}

/* -------------------------------------------------------------- board --- */

static void board_audio_start(void)
{
    i2c_master_bus_handle_t bus = NULL;
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port          = BOARD_I2C_PORT,
        .sda_io_num        = CONFIG_KS_BOARD_I2C_SDA,
        .scl_io_num        = CONFIG_KS_BOARD_I2C_SCL,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));

    esp_err_t err = ch32v003_exio_new(bus, 0, EXIO_DIR, EXIO_BOOT_LEVELS, &s_exio);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "IO expander missing (%s): speaker amplifier stays off",
                 esp_err_to_name(err));
    }

    const melody_config_t mcfg = {
        .i2c_bus       = bus,
        .i2s_port      = I2S_PORT,
        .mclk          = CONFIG_KS_I2S_MCLK,
        .bclk          = CONFIG_KS_I2S_BCLK,
        .ws            = CONFIG_KS_I2S_WS,
        .dout          = CONFIG_KS_I2S_DOUT,
        .sample_rate   = SAMPLE_RATE_HZ,
        .volume        = CONFIG_KS_VOLUME,
        .amp           = speaker_amp,
        .amp_settle_ms = AMP_SETTLE_MS,
    };
    err = melody_init(&mcfg);
    s_audio_ok = (err == ESP_OK);
    if (!s_audio_ok) {
        ESP_LOGE(TAG, "audio unavailable (%s)", esp_err_to_name(err));
    }
}

void app_main(void)
{
    s_verdicts = xQueueCreate(4, sizeof(verdict_t));
    assert(s_verdicts);

    board_audio_start();
    usb_start();

#if CONFIG_KS_BOOT_MELODY
    play(MELODY_BOOT);
#endif

    xTaskCreate(nfc_task, "nfc", 4096, NULL, 5, NULL);
}
