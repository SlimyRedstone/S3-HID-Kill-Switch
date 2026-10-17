/*
 * Enabling and disabling HID input devices, the same operation Device Manager
 * performs on a device node (right-click -> Disable / Enable).
 *
 * The unit here is a *physical device*, not a HID collection. A keyboard or
 * mouse often exposes several HID endpoints -- a boot interface plus an N-key
 * rollover interface, or a receiver that carries both a keyboard and a mouse --
 * and Windows shows each as its own node under "Keyboards" / "Mice". They all
 * hang off one USB device, which is the thing worth switching: disabling it
 * stops every endpoint at once. So enumeration groups the collections by that
 * shared device and reports one entry per device, keyed by its instance id.
 *
 * On Windows this is the configuration manager's CM_Disable_DevNode /
 * CM_Enable_DevNode on that device node, which takes effect at once and is
 * reversible, and is not persisted, so a reboot restores everything. It needs
 * administrator rights; without them the call fails with access denied and
 * hid_last_error() says so.
 *
 * Elsewhere this is a stub: hid_available() is false and nothing is listed or
 * changed, so the rest of the program still builds and runs.
 */

#ifndef HIDCTL_H
#define HIDCTL_H

#include <stdbool.h>
#include <stdint.h>

#define HID_NAME_MAX        128
#define HID_INSTANCE_MAX    256

typedef struct {
    char     name[HID_NAME_MAX];            /* friendly name of the device    */
    char     instance_id[HID_INSTANCE_MAX]; /* the node switched; the group key */
    uint16_t vid;                           /* 0 when the device has no USB id */
    uint16_t pid;
    bool     has_keyboard;                  /* carries a keyboard endpoint     */
    bool     has_mouse;                     /* carries a mouse endpoint        */
    bool     disabled;                      /* currently disabled in Windows   */
    bool     disableable;                   /* Windows permits disabling it    */
} hid_device_t;

/** Whether this build can enumerate and switch devices (false off Windows). */
bool hid_available(void);

/**
 * List present keyboard/mouse devices, one per physical device, ordered by
 * VID:PID. Collections that share a device are merged into one entry whose
 * has_keyboard / has_mouse say what it carries.
 *
 * @return the number written, or -1 on failure (hid_last_error() explains).
 */
int hid_enumerate(hid_device_t *out, int max);

/**
 * Enable or disable the device with this instance id (an hid_device_t's
 * instance_id).
 *
 * @param count If non-NULL, receives 1 when the node was switched, else 0.
 * @return true on success; false with hid_last_error() set otherwise. A device
 *         that is no longer present is a success with @p count 0 -- it cannot
 *         lock anyone out.
 */
bool hid_set_enabled(const char *instance_id, bool enable, int *count);

/** Most recent failure, or "" since the last success. */
const char *hid_last_error(void);

#endif /* HIDCTL_H */
