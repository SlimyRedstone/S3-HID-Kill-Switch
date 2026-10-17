/*
 * Vendor bulk transport, CDC serial port and command dispatch.
 * Descriptors live in usb_descriptors.c.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"

#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_cdc_acm.h"

#include "usb_proto.h"
#include "usb_proto_descriptors.h"

static const char *TAG = "usb_proto";

#define VENDOR_WRITE_TIMEOUT_MS 100
#define RX_QUEUE_DEPTH          8
#define EVENT_QUEUE_DEPTH       8
#define TASK_STACK              4096
#define TASK_PRIO               4

/*
 * Reassembly buffer for JSON payloads spanning several bulk packets. A slice
 * of the host document is the largest thing that arrives, and its text is
 * escaped, so this is sized well above HOST_SLICE_MAX.
 */
#define JSON_BUF_MAX            2048

/* Small replies -- ok, an error, a colour -- built on the stack. */
#define REPLY_BUF_MAX           320

/*
 * How much of the host document one reply carries. Escaping can double it on
 * the way out, so the buffer that holds the printed reply is sized for that
 * worst case; both live in static storage rather than on the dispatch task's
 * 4K stack.
 */
#define HOST_SLICE_MAX          512
#define HOST_REPLY_MAX          (HOST_SLICE_MAX * 2 + 96)

/* One inbound bulk packet. */
typedef struct {
    uint16_t len;
    uint8_t  data[USB_PROTO_EP_SIZE];
} usb_packet_t;

/*
 * Outbound asynchronous events. Larger than a packet because an interrupt
 * report carrying a message exceeds 64 bytes; the transport splits it across
 * packets on the way out.
 */
typedef struct {
    uint16_t len;
    uint8_t  data[USB_PROTO_EVENT_MAX];
} usb_event_t;

/* How long the receive colour stays on before reverting to the idle colour. */
#define RECEIVE_FLASH_MS        120

static usb_proto_config_t s_cfg;
static QueueHandle_t      s_rx_queue;
static QueueHandle_t      s_event_queue;
static TaskHandle_t       s_task;
static volatile bool      s_running;

/* Status LED state, owned by the dispatch task. */
static uint32_t   s_idle_color;
static TickType_t s_flash_until;
static bool       s_flashing;
static int        s_last_link = -1;   /*!< usb_link_t, or -1 before the first pass */

/* How far the USB link has come, which is what the resting colour reflects. */
typedef enum {
    USB_LINK_DETACHED = 0,  /*!< no host at all; the bus is not powered   */
    USB_LINK_ATTACHED,      /*!< powered and enumerating, but not usable  */
    USB_LINK_READY,         /*!< configured; the endpoints can carry data */
} usb_link_t;

static void apply_led(uint32_t rgb)
{
    if (s_cfg.status_led && s_cfg.on_led_command) {
        s_cfg.on_led_command(rgb);
    }
}

static uint32_t link_color(usb_link_t link)
{
    switch (link) {
    case USB_LINK_READY:
        return s_cfg.led_connected;
    case USB_LINK_ATTACHED:
        return s_cfg.led_boot;
    case USB_LINK_DETACHED:
    default:
        return s_cfg.led_disconnected;
    }
}

/* ---------------------------------------------------- vendor transport --- */

bool usb_proto_vendor_mounted(void)
{
    return tud_vendor_mounted();
}

/*
 * Bounded on purpose: an unbounded retry loop here wedges the dispatch task,
 * and then queued OUT packets never get serviced.
 */
esp_err_t usb_proto_vendor_send(const uint8_t *data, size_t len)
{
    if (data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!tud_vendor_mounted()) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t sent = 0;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(VENDOR_WRITE_TIMEOUT_MS);

    while (sent < len) {
        uint32_t n = tud_vendor_write(data + sent, len - sent);
        if (n == 0) {
            if (xTaskGetTickCount() >= deadline) {
                ESP_LOGW(TAG, "tx stalled, dropped %u of %u bytes",
                         (unsigned)(len - sent), (unsigned)len);
                return ESP_ERR_TIMEOUT;
            }
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        sent += n;
        tud_vendor_write_flush();
    }
    return ESP_OK;
}

static void reply(const char *text)
{
    usb_proto_vendor_send((const uint8_t *)text, strlen(text));
}

esp_err_t usb_proto_send_event(const void *data, size_t len)
{
    if (data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len > USB_PROTO_EVENT_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (s_event_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    usb_event_t evt;
    evt.len = (uint16_t)len;
    memcpy(evt.data, data, len);

    if (xQueueSend(s_event_queue, &evt, 0) != pdTRUE) {
        ESP_LOGW(TAG, "event queue full, dropped %u bytes", (unsigned)len);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void usb_proto_send_interrupt_cb(int gpio, int level, void *message)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return;
    }

    cJSON *interrupt = cJSON_AddObjectToObject(root, "interrupt");
    if (interrupt == NULL) {
        cJSON_Delete(root);
        return;
    }

    cJSON_AddNumberToObject(interrupt, "gpio", gpio);
    cJSON_AddNumberToObject(interrupt, "state", level);
    /* cJSON escapes the string, so an arbitrary message stays valid JSON. */
    cJSON_AddStringToObject(interrupt, "message",
                            message ? (const char *)message : "");

    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (text == NULL) {
        return;
    }

    usb_proto_send_event(text, strlen(text));
    cJSON_free(text);
}

esp_err_t usb_proto_send_slider(int id, int value, bool touched)
{
    char json[96];
    int n = snprintf(json, sizeof(json),
                     "{\"set\":{\"slider\":{\"id\":%d,\"value\":%d,\"touch\":%d}}}",
                     id, value, touched ? 1 : 0);

    if (n < 0 || n >= (int)sizeof(json)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return usb_proto_send_event(json, (size_t)n);
}

esp_err_t usb_proto_request_slider(int id)
{
    char json[64];
    int n = snprintf(json, sizeof(json),
                     "{\"get\":{\"slider\":{\"id\":%d}}}", id);

    if (n < 0 || n >= (int)sizeof(json)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return usb_proto_send_event(json, (size_t)n);
}

esp_err_t usb_proto_send_card(const char *uid_hex, uint16_t atqa, uint8_t sak,
                              const char *type)
{
    if (uid_hex == NULL || uid_hex[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    char json[USB_PROTO_EVENT_MAX];
    int n = snprintf(json, sizeof(json),
                     "{\"card\":{\"uid\":\"%s\",\"atqa\":\"%04X\",\"sak\":\"%02X\",\"type\":\"%s\"}}",
                     uid_hex, (unsigned)atqa, (unsigned)sak, type ? type : "unknown");

    if (n < 0 || n >= (int)sizeof(json)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return usb_proto_send_event(json, (size_t)n);
}

/*
 * Host -> device. Runs on the TinyUSB task, so hand off and return fast.
 *
 * The read is mandatory, not a convenience. TinyUSB re-arms the OUT endpoint
 * only when the class RX FIFO has a whole packet of free space, and that FIFO
 * is exactly one packet deep (CFG_TUD_VENDOR_RX_BUFSIZE == wMaxPacketSize).
 * Copying out of `buffer` without calling tud_vendor_n_read() leaves the FIFO
 * occupied, so the endpoint is never re-armed and every later OUT transfer
 * NAKs until the host times out.
 */
void tud_vendor_rx_cb(uint8_t itf, uint8_t const *buffer, uint16_t bufsize)
{
    (void)buffer;
    (void)bufsize;

    if (s_rx_queue == NULL) {
        return;
    }

    /*
     * Loop until the FIFO is empty rather than reading once. A single callback
     * can carry more than pkt.data holds whenever CFG_TUD_VENDOR_EPSIZE exceeds
     * the 64-byte packet size, and any remainder left behind keeps the FIFO
     * occupied, which is precisely what stops the endpoint being re-armed.
     */
    for (;;) {
        usb_packet_t pkt;
        pkt.len = (uint16_t)tud_vendor_n_read(itf, pkt.data, sizeof(pkt.data));
        if (pkt.len == 0) {
            return;
        }

        if (xQueueSend(s_rx_queue, &pkt, 0) != pdTRUE) {
            ESP_LOGW(TAG, "rx queue full, dropped %u bytes", pkt.len);
        }
    }
}

/* ---------------------------------------------------------- cdc serial --- */

static uint8_t s_cdc_buf[CONFIG_TINYUSB_CDC_RX_BUFSIZE];

esp_err_t usb_proto_cdc_print(const char *text, size_t len)
{
    if (text == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, (const uint8_t *)text, len);
    return tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
}

static void cdc_rx_cb(int itf, cdcacm_event_t *event)
{
    (void)event;

    size_t rx_size = 0;
    if (tinyusb_cdcacm_read(itf, s_cdc_buf, sizeof(s_cdc_buf), &rx_size) != ESP_OK) {
        return;
    }
    if (rx_size == 0) {
        return;
    }

    ESP_LOGI(TAG, "cdc rx %u bytes", (unsigned)rx_size);

    if (s_cfg.cdc_echo) {
        tinyusb_cdcacm_write_queue(itf, s_cdc_buf, rx_size);
        tinyusb_cdcacm_write_flush(itf, 0);
    }
}

static void cdc_line_state_cb(int itf, cdcacm_event_t *event)
{
    (void)itf;
    ESP_LOGI(TAG, "cdc line state: dtr=%d rts=%d",
             event->line_state_changed_data.dtr,
             event->line_state_changed_data.rts);
}

/* ------------------------------------------------------ command parse --- */

static bool parse_hex6(const char *text, uint32_t *out_rgb)
{
    if (text == NULL) {
        return false;
    }
    if (*text == '#') {
        text++;
    }
    if (strlen(text) != 6) {
        return false;
    }

    uint32_t rgb = 0;
    for (int i = 0; i < 6; i++) {
        char c = text[i];
        int nib;

        if (c >= '0' && c <= '9')      nib = c - '0';
        else if (c >= 'a' && c <= 'f') nib = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') nib = c - 'A' + 10;
        else return false;

        rgb = (rgb << 4) | (uint32_t)nib;
    }

    *out_rgb = rgb;
    return true;
}

static void reply_ok(void)
{
    reply("{\"ok\":true}");
}

static void reply_error(const char *message)
{
    char buf[REPLY_BUF_MAX];
    snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"%s\"}", message);
    ESP_LOGW(TAG, "%s", message);
    reply(buf);
}

/* ----------------------------------------------------------- dispatch --- */

static bool apply_set_led(const cJSON *led)
{
    if (!cJSON_IsString(led)) {
        reply_error("led must be a string");
        return false;
    }

    uint32_t rgb;
    if (!parse_hex6(led->valuestring, &rgb)) {
        reply_error("led must be RRGGBB hex");
        return false;
    }
    if (s_cfg.on_led_command == NULL) {
        reply_error("no led handler");
        return false;
    }

    esp_err_t err = s_cfg.on_led_command(rgb);
    ESP_LOGI(TAG, "set led -> #%06X (%s)", (unsigned)rgb, esp_err_to_name(err));
    if (err != ESP_OK) {
        reply_error(esp_err_to_name(err));
        return false;
    }

    /* A colour chosen by the host becomes the new resting colour, so the next
       receive flash returns to it rather than to led_connected. */
    s_idle_color = rgb;
    s_flashing = false;
    return true;
}

static bool apply_set_message(const cJSON *message)
{
    if (!cJSON_IsString(message)) {
        reply_error("message must be a string");
        return false;
    }

    const char *text = message->valuestring;
    size_t len = strlen(text);

    if (s_cfg.on_text_message) {
        s_cfg.on_text_message(text, len);
    } else {
        ESP_LOGI(TAG, "message: %s", text);
        usb_proto_cdc_print(text, len);
        usb_proto_cdc_print("\r\n", 2);
    }
    return true;
}

static bool apply_set_config(const cJSON *config)
{
    if (!cJSON_IsObject(config)) {
        reply_error("config must be an object");
        return false;
    }
    if (s_cfg.on_config_set == NULL) {
        reply_error("no config handler");
        return false;
    }

    char *text = cJSON_PrintUnformatted(config);
    if (text == NULL) {
        reply_error("out of memory");
        return false;
    }

    esp_err_t err = s_cfg.on_config_set(text);
    ESP_LOGI(TAG, "set config %s (%s)", text, esp_err_to_name(err));
    cJSON_free(text);

    if (err != ESP_OK) {
        reply_error(esp_err_to_name(err));
        return false;
    }
    return true;
}

static bool apply_set_slider(const cJSON *slider)
{
    if (!cJSON_IsObject(slider)) {
        reply_error("slider must be an object");
        return false;
    }

    const cJSON *id = cJSON_GetObjectItemCaseSensitive(slider, "id");
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(slider, "value");

    if (!cJSON_IsNumber(id) || !cJSON_IsNumber(value)) {
        reply_error("slider needs numeric id and value");
        return false;
    }
    if (s_cfg.on_slider_set == NULL) {
        reply_error("no slider handler");
        return false;
    }

    esp_err_t err = s_cfg.on_slider_set(id->valueint, value->valueint);
    ESP_LOGI(TAG, "set slider %d -> %d (%s)",
             id->valueint, value->valueint, esp_err_to_name(err));

    if (err != ESP_OK) {
        reply_error(esp_err_to_name(err));
        return false;
    }
    return true;
}

/* Longest UID is ISO14443A triple size: 10 bytes, 20 hex digits. */
#define CARD_UID_HEX_MAX    20

static bool apply_set_card(const cJSON *card)
{
    if (!cJSON_IsObject(card)) {
        reply_error("card must be an object");
        return false;
    }

    const cJSON *uid = cJSON_GetObjectItemCaseSensitive(card, "uid");
    const cJSON *valid = cJSON_GetObjectItemCaseSensitive(card, "valid");

    if (!cJSON_IsString(uid) || !cJSON_IsBool(valid)) {
        reply_error("card needs string uid and bool valid");
        return false;
    }
    if (strlen(uid->valuestring) > CARD_UID_HEX_MAX) {
        reply_error("card uid too long");
        return false;
    }
    if (s_cfg.on_card_verdict == NULL) {
        reply_error("no card handler");
        return false;
    }

    esp_err_t err = s_cfg.on_card_verdict(uid->valuestring, cJSON_IsTrue(valid));
    ESP_LOGI(TAG, "card %s -> %s (%s)", uid->valuestring,
             cJSON_IsTrue(valid) ? "valid" : "invalid", esp_err_to_name(err));

    if (err != ESP_OK) {
        reply_error(esp_err_to_name(err));
        return false;
    }
    return true;
}

/*
 * Writes the JSON value for a get, which the dispatcher wraps in {"key":...}.
 *
 * @param args the value of the key in {"get":{"key":{...}}}, or NULL for the
 *             plain {"get":"key"} form. Only the host document uses it.
 */
static bool read_led(const cJSON *args, char *out, size_t out_size)
{
    (void)args;

    if (s_cfg.on_led_query == NULL) {
        reply_error("no led source");
        return false;
    }

    snprintf(out, out_size, "\"%06X\"",
             (unsigned)(s_cfg.on_led_query() & 0xFFFFFF));
    return true;
}

static bool read_config(const cJSON *args, char *out, size_t out_size)
{
    (void)args;

    if (s_cfg.on_config_get == NULL) {
        reply_error("no config source");
        return false;
    }

    char *text = s_cfg.on_config_get();
    if (text == NULL) {
        reply_error("config unavailable");
        return false;
    }

    int n = snprintf(out, out_size, "%s", text);
    free(text);

    if (n < 0 || n >= (int)out_size) {
        reply_error("config too large");
        return false;
    }
    return true;
}

/*
 * The host document moves in slices. Writes arrive already unescaped by the
 * parser; reads are escaped on the way out by building the reply with cJSON
 * rather than by printing into a format string, because the document is full
 * of quotes and backslashes and hand-escaping it would be a bug waiting.
 */
static bool apply_set_host(const cJSON *value)
{
    if (!cJSON_IsObject(value)) {
        reply_error("host must be an object");
        return false;
    }
    if (s_cfg.on_host_begin == NULL || s_cfg.on_host_write == NULL ||
        s_cfg.on_host_commit == NULL || s_cfg.on_host_abort == NULL) {
        reply_error("no host document store");
        return false;
    }

    const cJSON *begin  = cJSON_GetObjectItemCaseSensitive(value, "begin");
    const cJSON *text   = cJSON_GetObjectItemCaseSensitive(value, "text");
    const cJSON *commit = cJSON_GetObjectItemCaseSensitive(value, "commit");
    const cJSON *give_up = cJSON_GetObjectItemCaseSensitive(value, "abort");

    int steps = 0;

    /* Taken in this order, so one message may open, fill and close a transfer
       whenever the whole document fits into a single slice. */
    if (cJSON_IsNumber(begin)) {
        esp_err_t err = s_cfg.on_host_begin((size_t)begin->valuedouble);
        if (err != ESP_OK) {
            reply_error(esp_err_to_name(err));
            return false;
        }
        steps++;
    }

    if (cJSON_IsString(text)) {
        esp_err_t err = s_cfg.on_host_write(text->valuestring,
                                            strlen(text->valuestring));
        if (err != ESP_OK) {
            reply_error(esp_err_to_name(err));
            return false;
        }
        steps++;
    }

    if (cJSON_IsTrue(commit)) {
        esp_err_t err = s_cfg.on_host_commit();
        if (err != ESP_OK) {
            reply_error(esp_err_to_name(err));
            return false;
        }
        steps++;
    }

    if (cJSON_IsTrue(give_up)) {
        s_cfg.on_host_abort();
        steps++;
    }

    if (steps == 0) {
        reply_error("host needs begin, text, commit or abort");
        return false;
    }
    return true;
}

static bool read_host(const cJSON *args, char *out, size_t out_size)
{
    if (s_cfg.on_host_size == NULL || s_cfg.on_host_read == NULL) {
        reply_error("no host document store");
        return false;
    }

    long size = s_cfg.on_host_size();

    /* {"get":"host"} -- how much there is, so the host can plan its reads. */
    if (!cJSON_IsObject(args)) {
        snprintf(out, out_size, "{\"size\":%ld}", (size < 0) ? 0L : size);
        return true;
    }

    if (size < 0) {
        reply_error("no host document stored");
        return false;
    }

    const cJSON *off_item = cJSON_GetObjectItemCaseSensitive(args, "off");
    long off = cJSON_IsNumber(off_item) ? (long)off_item->valuedouble : 0;
    if (off < 0) {
        off = 0;
    }

    /* Static: too large for the dispatch task's stack, and only it runs here. */
    static char slice[HOST_SLICE_MAX + 1];
    int n = 0;

    if (off < size) {
        n = s_cfg.on_host_read(off, slice, HOST_SLICE_MAX);
        if (n < 0) {
            reply_error("cannot read host document");
            return false;
        }
    }
    /*
     * Never end a slice inside a UTF-8 sequence. The bytes are about to be
     * copied into a JSON string, and half a character would not survive the
     * trip -- one application path with an accent in it is enough to hit this.
     * Nothing is lost by backing off: the next read starts at those bytes.
     */
    if (n > 0 && (off + n) < size) {
        int cut = n;
        while (cut > 0 && ((unsigned char)slice[cut - 1] & 0xC0) == 0x80) {
            cut--;                          /* back over continuation bytes */
        }
        if (cut > 0) {
            unsigned char lead = (unsigned char)slice[cut - 1];
            int need = (lead < 0x80)           ? 1
                     : ((lead & 0xE0) == 0xC0) ? 2
                     : ((lead & 0xF0) == 0xE0) ? 3
                     : ((lead & 0xF8) == 0xF0) ? 4
                                               : 1;
            if ((n - (cut - 1)) < need) {
                n = cut - 1;                /* drop the incomplete character */
            }
        }
    }

    slice[n] = '\0';

    cJSON *obj = cJSON_CreateObject();
    if (obj == NULL) {
        reply_error("out of memory");
        return false;
    }

    cJSON_AddNumberToObject(obj, "off", (double)off);
    cJSON_AddStringToObject(obj, "text", slice);
    cJSON_AddBoolToObject(obj, "eof", (off + n) >= size);

    char *printed = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);

    if (printed == NULL) {
        reply_error("out of memory");
        return false;
    }

    int written = snprintf(out, out_size, "%s", printed);
    cJSON_free(printed);

    if (written < 0 || (size_t)written >= out_size) {
        reply_error("host slice too large");
        return false;
    }
    return true;
}

/*
 * The command table. Adding a command means adding one row and its handler;
 * nothing else in the dispatch changes. A NULL entry means that direction is
 * unsupported for the key.
 */
typedef bool (*set_fn_t)(const cJSON *value);
typedef bool (*get_fn_t)(const cJSON *args, char *out, size_t out_size);

typedef struct {
    const char *key;
    set_fn_t    set;
    get_fn_t    get;
} command_t;

static const command_t COMMANDS[] = {
    { "led",     apply_set_led,     read_led    },
    { "message", apply_set_message, NULL        },
    { "config",  apply_set_config,  read_config },
    { "slider",  apply_set_slider,  NULL        },
    { "host",    apply_set_host,    read_host   },
    { "card",    apply_set_card,    NULL        },
};

#define COMMAND_COUNT (sizeof(COMMANDS) / sizeof(COMMANDS[0]))

static void handle_set(const cJSON *set)
{
    int applied = 0;

    for (size_t i = 0; i < COMMAND_COUNT; i++) {
        if (COMMANDS[i].set == NULL) {
            continue;
        }

        const cJSON *value = cJSON_GetObjectItemCaseSensitive(set, COMMANDS[i].key);
        if (value == NULL) {
            continue;
        }

        /* The handler has already reported why it refused. */
        if (!COMMANDS[i].set(value)) {
            return;
        }
        applied++;
    }

    if (applied == 0) {
        reply_error("no known key in set");
        return;
    }
    reply_ok();
}

/*
 * Accepts both {"get":"led"} and the parameterised {"get":{"host":{"off":0}}}.
 * In the second form the single member names the target and its value is
 * handed to the handler.
 */
static void handle_get(const cJSON *get)
{
    const char *what = NULL;
    const cJSON *args = NULL;

    if (cJSON_IsString(get)) {
        what = get->valuestring;
    } else if (cJSON_IsObject(get) && get->child != NULL) {
        what = get->child->string;
        args = get->child;
    }

    if (what == NULL) {
        reply_error("get takes a name or a single named object");
        return;
    }

    for (size_t i = 0; i < COMMAND_COUNT; i++) {
        if (COMMANDS[i].get == NULL || strcmp(what, COMMANDS[i].key) != 0) {
            continue;
        }

        /* Static, and sized for a host slice: the dispatch task is the only
           caller, and these are far too large to sit on its stack. */
        static char value[HOST_REPLY_MAX];
        static char buf[HOST_REPLY_MAX + 32];

        if (!COMMANDS[i].get(args, value, sizeof(value))) {
            return;
        }

        int n = snprintf(buf, sizeof(buf), "{\"%s\":%s}", COMMANDS[i].key, value);
        if (n < 0 || n >= (int)sizeof(buf)) {
            reply_error("reply too large");
            return;
        }

        reply(buf);
        return;
    }

    reply_error("unknown get target");
}

/*
 * One complete JSON object from the host.
 *
 * Replies are sent from here, so this must run on the dispatch task -- it is
 * the single writer for the vendor IN endpoint.
 */
static void handle_json(const char *text)
{
    cJSON *root = cJSON_Parse(text);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        reply_error("expected a JSON object");
        return;
    }

    const cJSON *set = cJSON_GetObjectItemCaseSensitive(root, "set");
    const cJSON *get = cJSON_GetObjectItemCaseSensitive(root, "get");

    if (cJSON_IsObject(set)) {
        handle_set(set);
    } else if (get != NULL) {
        handle_get(get);
    } else {
        reply_error("expected \"set\" or \"get\"");
    }

    cJSON_Delete(root);
}

static void handle_unparsed(const uint8_t *data, uint16_t len)
{
    if (s_cfg.on_raw_packet) {
        s_cfg.on_raw_packet(data, len);
        return;
    }
    reply_error("invalid JSON");
}

static void dispatch_task(void *arg)
{
    (void)arg;

    usb_packet_t pkt;

    /* Static: too large for the task stack, and only this task touches it. */
    static char json_buf[JSON_BUF_MAX + 1];
    size_t json_len = 0;

    /*
     * Everything that writes to the vendor IN endpoint funnels through this
     * task, so the FIFO keeps a single writer. The short poll interval is what
     * lets asynchronous events (buttons, sensors) go out promptly without a
     * second writer.
     */
    while (s_running) {
        /*
         * tud_connected() says the bus is powered and enumeration has begun;
         * tud_ready() adds that the host has configured the device, so the
         * endpoints can actually carry data. The three cases are distinct
         * enough to be worth telling apart on the LED.
         */
        const usb_link_t link = !tud_connected() ? USB_LINK_DETACHED
                              : tud_ready()      ? USB_LINK_READY
                                                 : USB_LINK_ATTACHED;

        /* Only on a transition: an explicit LED command owns the colour in
           between, and reasserting it every cycle would undo that. */
        if ((int)link != s_last_link) {
            s_last_link = (int)link;
            s_idle_color = link_color(link);
            s_flashing = false;
            apply_led(s_idle_color);
        }

        if (xQueueReceive(s_rx_queue, &pkt, pdMS_TO_TICKS(20)) == pdTRUE) {
            apply_led(s_cfg.led_receive);
            s_flash_until = xTaskGetTickCount() + pdMS_TO_TICKS(RECEIVE_FLASH_MS);
            s_flashing = true;

            /*
             * A JSON document can span several bulk packets, so accumulate and
             * retry the parse. cJSON rejects a truncated object, which is what
             * tells us to keep waiting.
             *
             * A packet shorter than the endpoint size ends the USB transfer; if
             * the buffer still does not parse at that point it never will, so
             * report the error rather than waiting for bytes that never come.
             */
            if (json_len + pkt.len < JSON_BUF_MAX) {
                memcpy(json_buf + json_len, pkt.data, pkt.len);
                json_len += pkt.len;
                json_buf[json_len] = '\0';

                cJSON *probe = cJSON_Parse(json_buf);
                if (probe != NULL) {
                    cJSON_Delete(probe);
                    handle_json(json_buf);
                    json_len = 0;
                } else if (pkt.len < USB_PROTO_EP_SIZE) {
                    handle_unparsed((const uint8_t *)json_buf, json_len);
                    json_len = 0;
                }
            } else {
                reply_error("payload too large");
                json_len = 0;
            }
        }

        /* Receive flash expired: back to the resting colour. */
        if (s_flashing && xTaskGetTickCount() >= s_flash_until) {
            s_flashing = false;
            apply_led(s_idle_color);
        }

        usb_event_t evt;
        while (xQueueReceive(s_event_queue, &evt, 0) == pdTRUE) {
            if (tud_vendor_mounted()) {
                usb_proto_vendor_send(evt.data, evt.len);
                ESP_LOGI(TAG, "event: %.*s", (int)evt.len, (const char *)evt.data);
            }
        }
    }

    vTaskDelete(NULL);
}

/* --------------------------------------------------------------- init --- */

static void usb_event_cb(tinyusb_event_t *event, void *arg)
{
    (void)arg;
    switch (event->id) {
    case TINYUSB_EVENT_ATTACHED:
        ESP_LOGI(TAG, "attached to host");
        break;
    case TINYUSB_EVENT_DETACHED:
        ESP_LOGI(TAG, "detached from host");
        break;
    default:
        break;
    }
}

esp_err_t usb_proto_start(const usb_proto_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_running) {
        return ESP_ERR_INVALID_STATE;
    }

    s_cfg = *config;

    /* No host yet, and -1 forces the first pass to apply whatever is true. */
    s_idle_color = s_cfg.led_disconnected;
    s_flashing   = false;
    s_last_link  = -1;

    s_rx_queue = xQueueCreate(RX_QUEUE_DEPTH, sizeof(usb_packet_t));
    if (s_rx_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_event_queue = xQueueCreate(EVENT_QUEUE_DEPTH, sizeof(usb_event_t));
    if (s_event_queue == NULL) {
        vQueueDelete(s_rx_queue);
        s_rx_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    usb_proto_descriptors_init(&s_cfg);

    int string_count = 0;
    const char **strings = usb_proto_string_descriptors(&string_count);

    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG(usb_event_cb);
    tusb_cfg.descriptor.device            = usb_proto_device_descriptor();
    tusb_cfg.descriptor.string            = strings;
    tusb_cfg.descriptor.string_count      = string_count;
    tusb_cfg.descriptor.full_speed_config = usb_proto_config_descriptor();

    esp_err_t err = tinyusb_driver_install(&tusb_cfg);
    if (err != ESP_OK) {
        goto fail;
    }

    const tinyusb_config_cdcacm_t acm_cfg = {
        .cdc_port                    = TINYUSB_CDC_ACM_0,
        .callback_rx                 = cdc_rx_cb,
        .callback_line_state_changed = cdc_line_state_cb,
    };
    err = tinyusb_cdcacm_init(&acm_cfg);
    if (err != ESP_OK) {
        tinyusb_driver_uninstall();
        goto fail;
    }

    s_running = true;
    if (xTaskCreate(dispatch_task, "usb_proto", TASK_STACK, NULL,
                    TASK_PRIO, &s_task) != pdPASS) {
        s_running = false;
        tinyusb_cdcacm_deinit(TINYUSB_CDC_ACM_0);
        tinyusb_driver_uninstall();
        err = ESP_ERR_NO_MEM;
        goto fail;
    }

    ESP_LOGI(TAG, "composite device started (%04X:%04X): CDC + vendor",
             s_cfg.vid, s_cfg.pid);
    return ESP_OK;

fail:
    vQueueDelete(s_rx_queue);
    s_rx_queue = NULL;
    vQueueDelete(s_event_queue);
    s_event_queue = NULL;
    return err;
}

esp_err_t usb_proto_stop(void)
{
    if (!s_running) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Let dispatch_task observe the flag and delete itself. */
    s_running = false;
    vTaskDelay(pdMS_TO_TICKS(1100));
    s_task = NULL;

    tinyusb_cdcacm_deinit(TINYUSB_CDC_ACM_0);
    esp_err_t err = tinyusb_driver_uninstall();

    vQueueDelete(s_rx_queue);
    s_rx_queue = NULL;
    vQueueDelete(s_event_queue);
    s_event_queue = NULL;
    return err;
}
