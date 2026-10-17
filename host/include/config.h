/*
 * config.json: the authorised cards and the HID devices the kill switch
 * controls.
 *
 *   {
 *     "debug": false,
 *     "allow_virtual": true,
 *     "cards":   [ { "uid": "04A1B2C3" } ],
 *     "devices": [ { "name": "Keyboard", "usb": { "vid": "0x046D", "pid": "0xC31C" } } ]
 *   }
 *
 * "allow_virtual" (default true): when the switch engages, let software /
 * virtual input (Unified Remote, AutoHotkey, on-screen keyboard) keep working
 * and block only the physical keyboard/mouse. Set false to block everything.
 *
 * A device listed here is a kill-switch target: the one a valid card toggles.
 * The UI shows every HID device present and ticks the ones that are in this
 * list, so editing the tick is what adds or removes an entry.
 *
 * Read with cJSON, written indented so it can be edited by hand. A missing or
 * malformed file is not an error: it falls back to empty lists, and the app
 * still starts.
 */

#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CONFIG_PATH_MAX     512
#define CONFIG_UID_MAX      24      /* 20 hex digits + NUL, rounded up */
#define CONFIG_NAME_MAX     96
#define CONFIG_INSTANCE_MAX 256     /* a Windows device instance id */
#define CONFIG_CARDS_MAX    64
#define CONFIG_DEVICES_MAX  32

typedef struct {
    char uid[CONFIG_UID_MAX];       /* upper-case hex, no separators */
} config_card_t;

/*
 * A selected physical device. Matched by its instance id -- stable on a given
 * machine and able to name devices that have no USB VID/PID -- with the VID/PID
 * and name kept only for display.
 */
typedef struct {
    char     instance_id[CONFIG_INSTANCE_MAX];
    char     name[CONFIG_NAME_MAX];
    uint16_t vid;
    uint16_t pid;
} config_device_t;

typedef struct {
    bool debug;
    bool allow_virtual;             /* block only physical input (default true) */

    config_card_t   cards[CONFIG_CARDS_MAX];
    int             card_count;

    config_device_t devices[CONFIG_DEVICES_MAX];
    int             device_count;
} config_t;

/** Reset to empty lists, debug off. */
void config_defaults(config_t *c);

/** Load from @p path. Returns false on a missing or malformed file, leaving
 *  @p c at its defaults; the caller treats that as "start empty", not fatal. */
bool config_load(config_t *c, const char *path);

/** Write @p c to @p path via a temp file and rename, so a crash mid-save
 *  cannot leave a half-written file. Returns false if it could not be written. */
bool config_save(const config_t *c, const char *path);

/* ---- cards ---- */

/** Case-insensitive lookup of @p uid in the card list. */
bool config_has_card(const config_t *c, const char *uid);

/** Add @p uid if absent and there is room. Returns true if the list changed. */
bool config_add_card(config_t *c, const char *uid);

/** Remove @p uid if present. Returns true if the list changed. */
bool config_remove_card(config_t *c, const char *uid);

/* ---- devices ---- */

/** Whether the device with this instance id is a selected kill-switch target. */
bool config_has_device(const config_t *c, const char *instance_id);

/** Add a target. @p name / @p vid / @p pid are for display; the match is by id. */
bool config_add_device(config_t *c, const char *instance_id, const char *name,
                       uint16_t vid, uint16_t pid);

bool config_remove_device(config_t *c, const char *instance_id);

#endif /* CONFIG_H */
