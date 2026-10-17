#include "app.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "inputblock.h"
#include "usbdev.h"
#include "watchdog.h"

#define SEND_TIMEOUT_MS  500
#define POLL_TIMEOUT_MS  1
#define POLL_MAX_PACKETS 16

static int s_poll_fails;

/* ------------------------------------------------------------- logging -- */

void app_log(app_t *a, app_log_kind_t kind, const char *fmt, ...)
{
    int slot;
    if (a->log_count < APP_LOG_CAPACITY) {
        slot = (a->log_first + a->log_count) % APP_LOG_CAPACITY;
        a->log_count++;
    } else {
        slot = a->log_first;
        a->log_first = (a->log_first + 1) % APP_LOG_CAPACITY;
    }

    a->log[slot].kind = kind;
    a->log_seq++;

    va_list args;
    va_start(args, fmt);
    vsnprintf(a->log[slot].text, APP_LOG_TEXT_MAX, fmt, args);
    va_end(args);
}

void app_log_clear(app_t *a)
{
    a->log_count = 0;
    a->log_first = 0;
    a->log_seq++;
}

const app_log_entry_t *app_log_at(const app_t *a, int index)
{
    if (index < 0 || index >= a->log_count) {
        return NULL;
    }
    return &a->log[(a->log_first + index) % APP_LOG_CAPACITY];
}

void app_notify(app_t *a, const char *text)
{
    snprintf(a->notice, sizeof(a->notice), "%s", text ? text : "");
    a->notice_seq++;
}

/* ------------------------------------------------------- HID + arming --- */

bool app_device_selected(const app_t *a, const char *instance_id)
{
    return config_has_device(&a->cfg, instance_id);
}

void app_refresh_hids(app_t *a)
{
    int n = hid_enumerate(a->hids, APP_HID_MAX);
    if (n < 0) {
        a->hid_count = 0;
        app_log(a, APP_LOG_ERROR, "could not list HID devices: %s",
                hid_last_error());
        return;
    }
    a->hid_count = n;
}

/* Which input types the selected devices cover, from the live device list.
   When none of them is present to classify, block both -- a kill switch should
   err on the side of stopping input, and a reboot or the card undoes it. */
static void selected_input_types(const app_t *a, bool *keyboard, bool *mouse)
{
    *keyboard = false;
    *mouse = false;
    bool matched = false;

    for (int i = 0; i < a->hid_count; i++) {
        if (!config_has_device(&a->cfg, a->hids[i].instance_id)) {
            continue;
        }
        matched = true;
        if (a->hids[i].has_keyboard) *keyboard = true;
        if (a->hids[i].has_mouse)    *mouse = true;
    }
    if (!matched && a->cfg.device_count > 0) {
        *keyboard = true;
        *mouse = true;
    }
}

void app_set_armed(app_t *a, bool engage)
{
    if (a->cfg.device_count == 0) {
        app_notify(a, "No devices selected");
        app_log(a, APP_LOG_EVENT, "nothing to %s: no devices selected",
                engage ? "block" : "release");
        a->armed = false;
        return;
    }

    /*
     * Two layers. The input block (low-level hooks) is what actually stops a
     * keyboard, works on any hardware, and is removed if this process dies.
     * The device disable is best effort on top: it physically powers the
     * device off where Windows allows, and is simply skipped where it does not.
     */
    bool want_kb = false, want_mouse = false;
    selected_input_types(a, &want_kb, &want_mouse);

    if (inputblock_available()) {
        inputblock_set(engage && want_kb, engage && want_mouse);
    }

    int total = 0;
    if (hid_available()) {
        for (int i = 0; i < a->cfg.device_count; i++) {
            const config_device_t *d = &a->cfg.devices[i];
            int n = 0;
            if (hid_set_enabled(d->instance_id, !engage, &n)) {
                total += n;
            } else {
                /* Logged, not surfaced: the input block already did the job. */
                app_log(a, APP_LOG_EVENT, "%s %s (%04X:%04X): %s (input block covers it)",
                        engage ? "disable" : "enable", d->name, d->vid, d->pid,
                        hid_last_error());
            }
        }
        app_refresh_hids(a);
    }

    a->armed = engage;
    app_log(a, APP_LOG_EVENT, "kill switch %s%s%s%s",
            engage ? "ENGAGED" : "released",
            engage && (want_kb || want_mouse) ? " (blocking " : "",
            engage ? (want_kb && want_mouse ? "keyboard + mouse" :
                      want_kb ? "keyboard" : want_mouse ? "mouse" : "") : "",
            engage && (want_kb || want_mouse) ? ")" : "");
}

void app_toggle(app_t *a)
{
    app_set_armed(a, !a->armed);
}

/* --------------------------------------------------------- card logic --- */

/* Reply to the S3, which turns the verdict into a success or error melody. */
static void reply_card(app_t *a, const char *uid, bool valid)
{
    char json[96];
    snprintf(json, sizeof(json),
             "{\"set\":{\"card\":{\"uid\":\"%s\",\"valid\":%s}}}",
             uid, valid ? "true" : "false");
    app_send_json(a, json);
}

void app_handle_card(app_t *a, const char *uid, const char *type)
{
    if (uid == NULL || uid[0] == '\0') {
        return;
    }

    snprintf(a->last_uid, sizeof(a->last_uid), "%s", uid);
    snprintf(a->last_type, sizeof(a->last_type), "%s", type ? type : "");
    a->have_scan = true;
    a->scan_seq++;

    bool valid = config_has_card(&a->cfg, uid);
    a->last_valid = valid;

    app_log(a, APP_LOG_RX, "card %s [%s] -> %s", uid,
            type && type[0] ? type : "?", valid ? "authorised" : "unknown");

    reply_card(a, uid, valid);

    if (valid) {
        app_toggle(a);
        app_notify(a, a->armed ? "Kill switch ENGAGED" : "Kill switch released");
    } else {
        app_notify(a, "Card not recognised");
    }
}

/* ----------------------------------------------------- incoming frames -- */

/*
 * One decoded document from the S3.
 *   {"card":{"uid":...,"type":...}}   a scan to decide
 *   {"ok":...} / others               replies to our own sends
 */
static void app_handle_packet(app_t *a, const unsigned char *msg, int len,
                              cJSON *json)
{
    if (json == NULL) {
        app_log(a, APP_LOG_RX, "<- %.*s", len, (const char *)msg);
        return;
    }

    const cJSON *card = cJSON_GetObjectItemCaseSensitive(json, "card");
    if (cJSON_IsObject(card)) {
        const cJSON *uid  = cJSON_GetObjectItemCaseSensitive(card, "uid");
        const cJSON *type = cJSON_GetObjectItemCaseSensitive(card, "type");
        if (cJSON_IsString(uid)) {
            app_handle_card(a, uid->valuestring,
                            cJSON_IsString(type) ? type->valuestring : NULL);
            return;
        }
    }

    /* Anything else is a reply to one of our commands; show it in debug. */
    app_log(a, APP_LOG_RX, "<- %.*s", len, (const char *)msg);
}

static void app_on_message(void *user, const unsigned char *msg, int len,
                           struct cJSON *json)
{
    app_handle_packet((app_t *)user, msg, len, (cJSON *)json);
}

/* ------------------------------------------------------- connection ----- */

bool app_poll_failure_fatal(int rc, int fails, bool present)
{
    if (rc == LIBUSB_ERROR_NO_DEVICE) {
        return true;
    }
    if (fails >= APP_POLL_FAIL_CHECK && !present) {
        return true;
    }
    return fails >= APP_POLL_FAIL_GIVEUP;
}

bool app_connect(app_t *a)
{
    if (a->connected) {
        return true;
    }
    if (a->dev == NULL) {
        a->dev = calloc(1, sizeof(*a->dev));
        if (a->dev == NULL) {
            app_log(a, APP_LOG_ERROR, "out of memory");
            return false;
        }
    }

    uint16_t pid = APP_USB_PID;
    if (usbdev_open_any(a->dev, APP_USB_VID, &pid, 1) != 0) {
        if (!a->connect_failure_logged) {
            a->connect_failure_logged = true;
            char why[USBDEV_REASON_MAX] = "";
            usbdev_explain_absence(APP_USB_VID, &pid, 1, why, sizeof(why));
            app_log(a, APP_LOG_ERROR, "no S3 KillSwitch (%04X:%04X) opened: %s",
                    APP_USB_VID, APP_USB_PID, why);
        }
        return false;
    }

    a->connect_failure_logged = false;
    a->connected = true;
    s_poll_fails = 0;
    snprintf(a->device_name, sizeof(a->device_name), "S3 KillSwitch");
    proto_framer_reset(&a->framer);

    app_log(a, APP_LOG_EVENT, "connected: %04X:%04X, interface %d, IN 0x%02X OUT 0x%02X",
            APP_USB_VID, a->dev->pid, a->dev->interface, a->dev->ep_in, a->dev->ep_out);
    return true;
}

void app_disconnect(app_t *a)
{
    if (!a->connected) {
        return;
    }

    /*
     * Safety: the card reader has just gone, so a card can no longer release
     * the kill switch. Bring the devices back before letting go of anything,
     * or the machine stays locked out with nothing able to undo it.
     */
    if (a->armed) {
        app_log(a, APP_LOG_EVENT, "controller gone while engaged: restoring input");
        app_set_armed(a, false);
    }
    inputblock_set(false, false);       /* belt-and-braces: never leave it on */

    usbdev_close(a->dev);
    a->connected = false;
    a->device_name[0] = '\0';
    s_poll_fails = 0;
    app_log(a, APP_LOG_EVENT, "disconnected");
}

void app_send_json(app_t *a, const char *json)
{
    if (!a->connected) {
        return;
    }
    app_log(a, APP_LOG_TX, "-> %s", json);
    if (usbdev_send(a->dev, json, strlen(json), SEND_TIMEOUT_MS) != 0) {
        app_log(a, APP_LOG_ERROR, "send failed, dropping the connection");
        app_disconnect(a);
    }
}

void app_poll(app_t *a)
{
    if (!a->connected) {
        return;
    }

    for (int i = 0; i < POLL_MAX_PACKETS; i++) {
        unsigned char buf[512];
        int len = 0;

        watchdog_phase("usbdev_recv");
        int rc = usbdev_recv(a->dev, buf, sizeof(buf), &len, POLL_TIMEOUT_MS);

        /* Bytes first, whatever the result: a short bulk timeout routinely
           expires with the front of a message already in the buffer. */
        if (len > 0) {
            proto_framer_push(&a->framer, buf, len, app_on_message, a);
            if (!a->connected) {
                return;     /* a failed reply inside handling dropped us */
            }
        }

        if (rc == LIBUSB_ERROR_TIMEOUT) {
            s_poll_fails = 0;
            return;
        }
        if (rc != 0) {
            s_poll_fails++;
            bool present = true;
            if (rc != LIBUSB_ERROR_NO_DEVICE && s_poll_fails >= APP_POLL_FAIL_CHECK) {
                present = usbdev_present(a->dev);
            }
            if (app_poll_failure_fatal(rc, s_poll_fails, present)) {
                app_log(a, APP_LOG_ERROR, present
                        ? "device stopped responding (%s), releasing it"
                        : "device unplugged (%s)", libusb_error_name(rc));
                app_disconnect(a);
            }
            return;
        }
        s_poll_fails = 0;
    }
}

/* ---------------------------------------------------------- config ------ */

void app_config_save(app_t *a)
{
    if (config_save(&a->cfg, a->config_path)) {
        a->config_dirty = false;
    } else {
        app_log(a, APP_LOG_ERROR, "could not save %s", a->config_path);
    }
}

bool app_config_reload(app_t *a)
{
    config_t fresh;
    if (!config_load(&fresh, a->config_path)) {
        app_log(a, APP_LOG_ERROR, "could not read %s", a->config_path);
        return false;
    }
    a->cfg = fresh;
    a->config_dirty = false;
    if (inputblock_available()) {
        inputblock_set_allow_virtual(a->cfg.allow_virtual);
    }
    app_log(a, APP_LOG_EVENT, "configuration reloaded: %d card(s), %d device(s)",
            a->cfg.card_count, a->cfg.device_count);
    return true;
}

/* ------------------------------------------------------- lifecycle ------ */

void app_init(app_t *a)
{
    memset(a, 0, sizeof(*a));

    snprintf(a->config_path, sizeof(a->config_path), "%s", APP_CONFIG_PATH);
    if (!config_load(&a->cfg, a->config_path)) {
        config_defaults(&a->cfg);
        app_log(a, APP_LOG_EVENT, "no %s yet: starting empty", a->config_path);
    } else {
        app_log(a, APP_LOG_EVENT, "loaded %d card(s), %d device(s)",
                a->cfg.card_count, a->cfg.device_count);
    }

    if (!hid_available()) {
        app_log(a, APP_LOG_ERROR, "%s", hid_last_error());
    } else {
        app_refresh_hids(a);
        app_log(a, APP_LOG_EVENT, "%d HID device(s) present", a->hid_count);
    }

    inputblock_init();
    if (!inputblock_available()) {
        app_log(a, APP_LOG_EVENT, "input blocking unavailable on this platform");
    } else {
        inputblock_set_allow_virtual(a->cfg.allow_virtual);
        inputblock_set_debug(a->cfg.debug);
        app_log(a, APP_LOG_EVENT, "virtual input: %s while blocked",
                a->cfg.allow_virtual ? "allowed" : "blocked too");
    }

    app_log(a, APP_LOG_EVENT, "not connected");
}

void app_shutdown(app_t *a)
{
    /*
     * Last line of defence: never exit leaving a selected device disabled,
     * whatever the armed flag says. Enable is harmless on one already enabled.
     */
    inputblock_set(false, false);       /* release any blocked input first */
    if (hid_available()) {
        for (int i = 0; i < a->cfg.device_count; i++) {
            hid_set_enabled(a->cfg.devices[i].instance_id, true, NULL);
        }
    }
    a->armed = false;
    inputblock_shutdown();

    if (a->config_dirty) {
        app_config_save(a);
    }
    if (a->connected) {
        /* armed is already false, so this does not try to re-enable again. */
        usbdev_close(a->dev);
        a->connected = false;
    }
    free(a->dev);
    a->dev = NULL;

    usbdev_shutdown();
}
