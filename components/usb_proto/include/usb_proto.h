/*
 * Composite USB device: a CDC-ACM serial port plus a vendor-specific interface
 * carrying a small text protocol on raw bulk endpoints.
 *
 * Everything USB lives behind this header -- descriptors, the BOS / MS OS 2.0
 * blocks that bind WinUSB on Windows, TinyUSB installation, endpoint FIFO
 * handling and command dispatch. Callers supply callbacks and call
 * usb_proto_start().
 *
 * Wire protocol, host -> device. One JSON object per bulk transfer:
 *
 *   {"set":{"led":"ABCDEF"}}                 dispatched to on_led_command
 *   {"set":{"message":"This is a test"}}     dispatched to on_text_message
 *   {"set":{"config":{ ... }}}               dispatched to on_config_set
 *   {"set":{"slider":{"id":0,"value":1024}}} dispatched to on_slider_set
 *                                            device -> host adds "touch":0|1
 *   {"set":{"card":{"uid":"04A1B2C3","valid":true}}}
 *                                            dispatched to on_card_verdict
 *   {"get":"led"}          ->  {"led":"ABCDEF"}
 *   {"get":"config"}       ->  {"config":{ ... }}
 *
 * A document without a root "set" or "get" is rejected outright. Commands are
 * held in one table in usb_proto.c, so a new one is a single row plus its
 * handler.
 *
 * A "set" object may carry several keys at once; all recognised ones are
 * applied. Every "set" is answered with {"ok":true} or, on failure,
 * {"ok":false,"error":"..."}.
 *
 * Transfers larger than one 64-byte packet are reassembled, so both requests
 * and replies may exceed the endpoint size. A payload that never parses is
 * handed to on_raw_packet, or answered with an error object if none is set.
 *
 * Wire protocol, device -> host, outside of replies:
 *   {"interrupt":{"gpio":0,"state":0,"message":"..."}}
 *                  a GPIO interrupt report, sent unprompted
 *   {"card":{"uid":"04A1B2C3","atqa":"0004","sak":"08","type":"..."}}
 *                  an NFC card was presented; the host answers with
 *                  {"set":{"card":{"uid":...,"valid":...}}}
 *   anything else  asynchronous events queued with usb_proto_send_event().
 *
 * Target: ESP32-S3 full-speed USB-OTG, esp_tinyusb 2.x on ESP-IDF v6.0.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Handler for "LED.RRGGBB". @p rgb is packed 0xRRGGBB. */
typedef esp_err_t (*usb_proto_led_cb_t)(uint32_t rgb);

/** Handler for "MSG.<text>", quotes already stripped. @p text is NUL terminated. */
typedef void (*usb_proto_msg_cb_t)(const char *text, size_t len);

/** Handler for a payload that is not valid JSON. */
typedef void (*usb_proto_raw_cb_t)(const uint8_t *data, size_t len);

/** Reports the current LED colour for {"get":"led"}. */
typedef uint32_t (*usb_proto_led_query_cb_t)(void);

/**
 * Serialises the configuration for {"get":"config"}.
 *
 * Must return a newly allocated JSON object string; usb_proto releases it with
 * free(). Returning NULL yields an error reply.
 */
typedef char *(*usb_proto_config_get_cb_t)(void);

/** Applies a JSON object from {"set":{"config":{...}}}. */
typedef esp_err_t (*usb_proto_config_set_cb_t)(const char *json);

/**
 * Applies {"set":{"slider":{"id":N,"value":V}}}.
 *
 * @param id    Slider index as sent by the host.
 * @param value New position.
 */
typedef esp_err_t (*usb_proto_slider_set_cb_t)(int id, int value);

/**
 * Applies {"set":{"card":{"uid":"04A1B2C3","valid":true}}}, the host's verdict
 * on a card reported with usb_proto_send_card().
 *
 * @param uid   Upper-case hex UID echoed by the host, NUL terminated.
 * @param valid Whether the host accepts the card.
 */
typedef esp_err_t (*usb_proto_card_verdict_cb_t)(const char *uid, bool valid);

/*
 * The host document: the desktop client's own configuration -- faders, their
 * application lists, profiles and macros -- parked on the device so that it
 * travels with the hardware. It is far larger than one JSON buffer, so it
 * moves in slices and the firmware treats it as opaque bytes throughout.
 *
 *   {"set":{"host":{"begin":8502}}}            open a transfer
 *   {"set":{"host":{"text":"<slice>"}}}        append, repeatable
 *   {"set":{"host":{"commit":true}}}           replace the stored document
 *   {"set":{"host":{"abort":true}}}            give up, changing nothing
 *   {"get":"host"}                             -> {"host":{"size":8502}}
 *   {"get":{"host":{"off":1024}}}              -> {"host":{"off":...,"text":...,"eof":...}}
 *
 * The three write steps may share one message when the document is small
 * enough to fit, which is what makes a short one a single round trip.
 */

/** @brief Open a transfer of @p total bytes, discarding any already open. */
typedef esp_err_t (*usb_proto_host_begin_cb_t)(size_t total);

/** @brief Append @p len bytes to the open transfer. */
typedef esp_err_t (*usb_proto_host_write_cb_t)(const char *text, size_t len);

/** @brief Replace the stored document with what was transferred. */
typedef esp_err_t (*usb_proto_host_commit_cb_t)(void);

/** @brief Discard a transfer in progress. */
typedef void (*usb_proto_host_abort_cb_t)(void);

/** @brief Size of the stored document, or -1 when there is none. */
typedef long (*usb_proto_host_size_cb_t)(void);

/**
 * @brief Copy at most @p max bytes from @p offset of the stored document.
 *
 * @return bytes copied, 0 at the end, -1 on error.
 */
typedef int (*usb_proto_host_read_cb_t)(long offset, char *out, size_t max);

typedef struct {
    /* Command handlers. A NULL handler falls back to the built-in behaviour
       described beside each field. */
    usb_proto_led_cb_t on_led_command;   /*!< NULL: reply with an error */
    usb_proto_msg_cb_t on_text_message;  /*!< NULL: print on CDC and to the log */
    usb_proto_raw_cb_t on_raw_packet;    /*!< NULL: echo back 0xA5 + payload */

    usb_proto_led_query_cb_t  on_led_query;   /*!< NULL: {"get":"led"} errors    */
    usb_proto_config_get_cb_t on_config_get;  /*!< NULL: {"get":"config"} errors */
    usb_proto_config_set_cb_t on_config_set;  /*!< NULL: config writes error     */
    usb_proto_slider_set_cb_t on_slider_set;  /*!< NULL: slider writes error     */
    usb_proto_card_verdict_cb_t on_card_verdict; /*!< NULL: card verdicts error  */

    /* The host document. All six are needed for it to work; leaving any
       NULL makes {"set":{"host":...}} and {"get":"host"} report so. */
    usb_proto_host_begin_cb_t  on_host_begin;
    usb_proto_host_write_cb_t  on_host_write;
    usb_proto_host_commit_cb_t on_host_commit;
    usb_proto_host_abort_cb_t  on_host_abort;
    usb_proto_host_size_cb_t   on_host_size;
    usb_proto_host_read_cb_t   on_host_read;

    bool cdc_echo;    /*!< Echo back whatever is typed at the CDC port */

    /*
     * Status LED. When enabled, on_led_command is also driven on state changes,
     * with three resting colours:
     *
     *   led_disconnected  no host at all; the bus is not even powered
     *   led_boot          attached, but not yet configured enough to talk
     *   led_connected     tud_connected() && tud_ready()
     *
     * A brief flash of led_receive marks each incoming packet before returning
     * to whichever of the three applies.
     *
     * An explicit LED.RRGGBB command replaces the idle colour, so a colour set
     * by the host survives receive flashes. The next change of USB state takes
     * the LED back, since that is information the host cannot override.
     */
    bool     status_led;
    uint32_t led_connected;  /*!< packed 0xRRGGBB */
    uint32_t led_receive;    /*!< packed 0xRRGGBB */
    uint32_t led_boot;       /*!< packed 0xRRGGBB, attached but not ready */
    uint32_t led_disconnected; /*!< packed 0xRRGGBB, no host attached     */

    /* USB identity. NULL strings keep the built-in defaults. */
    const char *manufacturer;
    const char *product;
    const char *serial;
    uint16_t    vid;
    uint16_t    pid;

    /*
     * Windows caches MS OS descriptor results per VID/PID/bcdDevice under
     * HKLM\SYSTEM\CurrentControlSet\Control\usbflags and never re-queries a
     * triple it has already seen. Bump this whenever the descriptors change,
     * or Windows keeps the stale answer and refuses to bind WinUSB.
     */
    uint16_t bcd_device;
} usb_proto_config_t;

#define USB_PROTO_DEFAULT_CONFIG() ((usb_proto_config_t){ \
    .cdc_echo   = true,                                   \
    .vid        = 0x303A,                                 \
    .pid        = 0x6902,                                 \
    .bcd_device = 0x0102,                                 \
})

/**
 * @brief Install TinyUSB, bring up both functions and start the dispatch task.
 */
esp_err_t usb_proto_start(const usb_proto_config_t *config);

/**
 * @brief Tear everything down again.
 */
esp_err_t usb_proto_stop(void);

/**
 * @brief True once the host has configured the vendor interface.
 */
bool usb_proto_vendor_mounted(void);

/**
 * @brief Send raw bytes on the vendor bulk IN endpoint.
 *
 * Bounded internally, so a host that has stopped reading cannot wedge the
 * caller. Returns ESP_ERR_TIMEOUT if the transmit FIFO never drained.
 *
 * TinyUSB's vendor FIFOs carry no mutex. The component's own dispatch task is
 * one writer already, so call this from one task only, or serialise it
 * yourself.
 */
esp_err_t usb_proto_vendor_send(const uint8_t *data, size_t len);

/** Largest asynchronous event payload, in bytes. */
#define USB_PROTO_EVENT_MAX 192

/**
 * @brief Queue bytes to be sent on the vendor IN endpoint by the dispatch task.
 *
 * Safe to call from any task, unlike usb_proto_vendor_send(), because the
 * actual write still happens on the single writer. Returns ESP_ERR_NO_MEM if
 * the outbound queue is full, or ESP_ERR_INVALID_SIZE if @p len exceeds
 * USB_PROTO_EVENT_MAX. Not callable from an ISR.
 */
esp_err_t usb_proto_send_event(const void *data, size_t len);

/**
 * @brief Report a GPIO interrupt to the host.
 *
 * Queues one JSON object:
 * @code
 * {"interrupt":{"gpio":0,"state":0,"message":"Button Triggered"}}
 * @endcode
 *
 * @param gpio    Pin that triggered.
 * @param level   Pin level at the moment it triggered.
 * @param message NUL-terminated string placed in the "message" field, or NULL
 *                for an empty one. Escaped on the way out, so any text is safe.
 *
 * Matches button_cb_t, so a button can be wired directly to a USB event:
 * @code
 * button_init(&cfg, usb_proto_send_interrupt_cb, "Button Triggered");
 * @endcode
 */
void usb_proto_send_interrupt_cb(int gpio, int level, void *message);

/**
 * @brief Report a slider that moved on the device.
 *
 * Queues {"set":{"slider":{"id":N,"value":V,"touch":T}}}, the same form the
 * host sends, so both directions share one message. The host ignores "touch"
 * on its way in and reads it on the way out: a slider under a finger is one
 * the host must not drive back.
 *
 * @param id      Slider index.
 * @param value   New position.
 * @param touched Whether a finger is on it.
 */
esp_err_t usb_proto_send_slider(int id, int value, bool touched);

/**
 * @brief Ask the host for a slider's state.
 *
 * Queues {"get":{"slider":{"id":N}}}. The host answers with
 * {"set":{"slider":{"id":N,"value":V,"name":"...","update":B}}}, where "update"
 * says whether its value is newer than the one held here.
 *
 * @param id Slider index.
 */
esp_err_t usb_proto_request_slider(int id);

/**
 * @brief Report an NFC card to the host.
 *
 * Queues {"card":{"uid":"04A1B2C3","atqa":"0004","sak":"08","type":"..."}}.
 * The host is expected to answer with {"set":{"card":{"uid":...,"valid":...}}},
 * which reaches on_card_verdict.
 *
 * @param uid_hex Upper-case hex UID, at most 20 characters.
 * @param atqa    ATQA / SENS_RES, first byte high.
 * @param sak     SAK / SEL_RES.
 * @param type    Card family name, plain ASCII without quotes or backslashes.
 */
esp_err_t usb_proto_send_card(const char *uid_hex, uint16_t atqa, uint8_t sak,
                              const char *type);

/**
 * @brief Write text to the CDC serial port.
 *
 * Note that the host discards device-to-host serial data unless something has
 * the COM port open at that moment.
 */
esp_err_t usb_proto_cdc_print(const char *text, size_t len);

#ifdef __cplusplus
}
#endif
