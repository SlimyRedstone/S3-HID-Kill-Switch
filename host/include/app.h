/*
 * Application state shared between the GUI and the USB transport.
 *
 * Holds the connection to the S3, the authorised-card and target-device
 * configuration, the live list of HID devices, and whether the kill switch is
 * currently engaged. The USB side is polled with a near-zero timeout so a
 * frame never blocks on the device.
 *
 * The one invariant that matters: the selected devices are never left disabled
 * when there is no way to scan a card to bring them back. The controller going
 * away and the program exiting both re-enable them.
 */

#ifndef APP_H
#define APP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * No #include "usbdev.h": that pulls in libusb.h, which on Windows pulls in
 * windows.h, whose Rectangle/CloseWindow/ShowCursor collide with raylib. The
 * UI includes this header, so the device stays behind a pointer.
 */
typedef struct usbdev usbdev_t;

#include "config.h"
#include "hidctl.h"
#include "proto.h"

/* The dongle this client talks to, and nothing else. */
#define APP_USB_VID        0x303A
#define APP_USB_PID        0x9911

#define APP_CONFIG_PATH    "config.json"

#define APP_HID_MAX        64

#define APP_LOG_CAPACITY   256
#define APP_LOG_TEXT_MAX   200

/* Failures before the bus is asked whether the device is still there, and
   after which it is released even if the bus still lists it. */
#define APP_POLL_FAIL_CHECK   3
#define APP_POLL_FAIL_GIVEUP  60

typedef enum {
    APP_LOG_TX,        /*!< something we sent            */
    APP_LOG_RX,        /*!< a reply or event from the S3 */
    APP_LOG_EVENT,     /*!< our own status               */
    APP_LOG_ERROR,
} app_log_kind_t;

typedef struct {
    app_log_kind_t kind;
    char           text[APP_LOG_TEXT_MAX];
} app_log_entry_t;

typedef struct {
    usbdev_t *dev;
    bool      connected;
    char      device_name[48];
    bool      connect_failure_logged;

    proto_framer_t framer;

    config_t cfg;
    char     config_path[CONFIG_PATH_MAX];
    bool     config_dirty;

    /* Present keyboard/mouse devices, refreshed on a timer and on changes. */
    hid_device_t hids[APP_HID_MAX];
    int          hid_count;

    /*
     * Engaged: the selected devices are disabled right now. A valid card
     * flips it; losing the controller or quitting forces it back off.
     */
    bool armed;

    /* The most recent scan, for the UI to show and for "add this card". */
    char          last_uid[CONFIG_UID_MAX];
    char          last_type[32];
    bool          last_valid;
    bool          have_scan;
    unsigned long scan_seq;         /* rises on each scan, for a UI flash */

    /* Ring buffer; oldest entry is dropped once it fills. */
    app_log_entry_t log[APP_LOG_CAPACITY];
    int             log_count;
    int             log_first;
    unsigned long   log_seq;

    /* Most recent toast line. The UI watches notice_seq and runs its own
       timer, so nothing here needs a clock. */
    char          notice[APP_LOG_TEXT_MAX];
    unsigned long notice_seq;
} app_t;

void app_init(app_t *a);
void app_shutdown(app_t *a);

bool app_connect(app_t *a);
void app_disconnect(app_t *a);

void app_poll(app_t *a);

/** When debug is on and the switch is engaged, log to the traffic view the raw
 *  flags of the input events the hooks see, so physical vs injected (virtual)
 *  input is visible without the separate debug console. Cheap; call each frame. */
void app_debug_pump(app_t *a);

void app_send_json(app_t *a, const char *json);

/** Re-list the present HID keyboards and mice into a->hids. */
void app_refresh_hids(app_t *a);

/**
 * Engage or release the kill switch: disable (engage) or enable (release)
 * every selected device. Sets a->armed to what was actually achieved and
 * notifies on failure (usually missing administrator rights).
 */
void app_set_armed(app_t *a, bool engage);

/** Flip the kill switch. */
void app_toggle(app_t *a);

/** Decide a scanned card and reply to the S3; toggles on a valid card. */
void app_handle_card(app_t *a, const char *uid, const char *type);

/** Whether the device with this instance id is a selected target. */
bool app_device_selected(const app_t *a, const char *instance_id);

void app_config_save(app_t *a);
bool app_config_reload(app_t *a);

/**
 * Whether a run of receive failures means the connection should be dropped.
 * Pulled out of the poll loop so the policy can be tested without a device.
 */
bool app_poll_failure_fatal(int rc, int fails, bool present);

void app_notify(app_t *a, const char *text);
void app_log(app_t *a, app_log_kind_t kind, const char *fmt, ...);
void app_log_clear(app_t *a);
const app_log_entry_t *app_log_at(const app_t *a, int index);

#endif /* APP_H */
