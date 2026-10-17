#include "hidctl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32

#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>

/* Some MinGW headers omit these; the values are fixed in the Windows ABI. */
#ifndef DN_DISABLEABLE
#define DN_DISABLEABLE 0x00002000
#endif
#ifndef CM_LOCATE_DEVNODE_NORMAL
#define CM_LOCATE_DEVNODE_NORMAL 0x00000000
#endif
#ifndef CM_LOCATE_DEVNODE_PHANTOM
#define CM_LOCATE_DEVNODE_PHANTOM 0x00000001
#endif

/*
 * The two Device Manager categories a kill switch cares about. Defined here
 * rather than pulled from devguid.h so the build does not depend on that
 * header's GUIDs being exported by the toolchain's import libraries.
 */
static const GUID GUID_CLASS_KEYBOARD =
    { 0x4D36E96Bu, 0xE325, 0x11CE, { 0xBF, 0xC1, 0x08, 0x00, 0x2B, 0xE1, 0x03, 0x18 } };
static const GUID GUID_CLASS_MOUSE =
    { 0x4D36E96Fu, 0xE325, 0x11CE, { 0xBF, 0xC1, 0x08, 0x00, 0x2B, 0xE1, 0x03, 0x18 } };

static char s_error[256];

static void set_error(const char *what, DWORD code)
{
    if (code == ERROR_SUCCESS) {
        snprintf(s_error, sizeof(s_error), "%s", what);
        return;
    }
    char *msg = NULL;
    FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                   FORMAT_MESSAGE_IGNORE_INSERTS, NULL, code,
                   MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                   (LPSTR)&msg, 0, NULL);
    if (msg) {
        for (char *p = msg; *p; p++) {
            if (*p == '\r' || *p == '\n') {
                *p = '\0';
                break;
            }
        }
    }
    snprintf(s_error, sizeof(s_error), "%s: %s (0x%lX)",
             what, msg ? msg : "error", (unsigned long)code);
    if (msg) {
        LocalFree(msg);
    }
}

/* Configuration-manager results have no system message table, so name the one
   that actually matters and show the rest as a code. */
static void set_error_cr(const char *what, CONFIGRET cr)
{
    const char *hint = (cr == CR_ACCESS_DENIED) ? " (run as administrator)"
                     : (cr == CR_REMOVE_VETOED) ? " (a driver refused)"
                     : "";
    snprintf(s_error, sizeof(s_error),
             "%s: configuration manager error 0x%lX%s",
             what, (unsigned long)cr, hint);
}

bool hid_available(void)
{
    return true;
}

const char *hid_last_error(void)
{
    return s_error;
}

/* Parse the hex after a "VID_" / "PID_" token in a device-id string. */
static uint16_t id_after(const char *s, const char *token)
{
    const char *at = strstr(s, token);
    if (at == NULL) {
        return 0;
    }
    return (uint16_t)strtoul(at + strlen(token), NULL, 16);
}

static bool devnode_disabled(DEVINST inst)
{
    ULONG status = 0, problem = 0;
    if (CM_Get_DevNode_Status(&status, &problem, inst, 0) != CR_SUCCESS) {
        return false;
    }
    return (status & DN_HAS_PROBLEM) && problem == CM_PROB_DISABLED;
}

static bool node_disableable(DEVINST inst)
{
    ULONG status = 0, problem = 0;
    if (CM_Get_DevNode_Status(&status, &problem, inst, 0) != CR_SUCCESS) {
        return false;
    }
    return (status & DN_DISABLEABLE) != 0;
}

static void get_instance_id(DEVINST inst, char *out, size_t cap)
{
    out[0] = '\0';
    CM_Get_Device_IDA(inst, out, (ULONG)cap, 0);
}

static void get_name(DEVINST inst, char *out, size_t cap)
{
    ULONG len = (ULONG)cap;
    if (CM_Get_DevNode_Registry_PropertyA(inst, CM_DRP_FRIENDLYNAME, NULL,
                                          out, &len, 0) == CR_SUCCESS && out[0]) {
        return;
    }
    len = (ULONG)cap;
    if (CM_Get_DevNode_Registry_PropertyA(inst, CM_DRP_DEVICEDESC, NULL,
                                          out, &len, 0) == CR_SUCCESS && out[0]) {
        return;
    }
    out[0] = '\0';
}

static bool is_hub_or_root(const char *id)
{
    return strstr(id, "ROOT_HUB") || strncmp(id, "HTREE", 5) == 0;
}

/*
 * The node worth switching for a HID collection.
 *
 * A keyboard or mouse usually presents several HID collections across one or
 * more USB interfaces (a boot interface plus an N-key-rollover one, consumer
 * controls, a combined keyboard+mouse receiver). Disabling a single collection
 * leaves the others live, so the target is the whole *physical USB device*: the
 * "USB\VID_xxxx&PID_yyyy\..." node with no "&MI_" interface suffix, which sits
 * just below the hub. Disabling it stops every endpoint at once, and every
 * collection of the device resolves to it, so they group into one entry.
 *
 * A device with no USB node in its chain (a PS/2 / ACPI built-in keyboard)
 * falls back to the nearest node that can be disabled, or the collection
 * itself, so it still appears and can be attempted.
 */
static DEVINST switch_target(DEVINST node)
{
    /* The physical USB device: the first "USB\..." node with no "&MI_"
       interface suffix, climbing up and never into a hub. */
    DEVINST usbdev = 0;
    DEVINST cur = node;
    for (int depth = 0; depth < 8; depth++) {
        char id[HID_INSTANCE_MAX] = { 0 };
        get_instance_id(cur, id, sizeof(id));
        if (strncmp(id, "USB\\", 4) == 0 && strstr(id, "&MI_") == NULL &&
            strstr(id, "ROOT_HUB") == NULL) {
            usbdev = cur;
            break;
        }
        DEVINST parent = 0;
        if (CM_Get_Parent(&parent, cur, 0) != CR_SUCCESS || parent == 0) {
            break;
        }
        char pid[HID_INSTANCE_MAX] = { 0 };
        get_instance_id(parent, pid, sizeof(pid));
        if (is_hub_or_root(pid)) {
            break;
        }
        cur = parent;
    }

    /* Switch the whole device when it can be disabled. */
    if (usbdev && node_disableable(usbdev)) {
        return usbdev;
    }

    /*
     * It cannot (a Logitech receiver's virtual composite, a built-in PS/2
     * keyboard): use the nearest ancestor that can be disabled, stopping before
     * a hub. For the receiver this is the receiver itself, which carries every
     * paired device -- so one row, and disabling it stops all of them.
     */
    cur = node;
    for (int depth = 0; depth < 8; depth++) {
        if (node_disableable(cur)) {
            return cur;
        }
        DEVINST parent = 0;
        if (CM_Get_Parent(&parent, cur, 0) != CR_SUCCESS || parent == 0) {
            break;
        }
        char pid[HID_INSTANCE_MAX] = { 0 };
        get_instance_id(parent, pid, sizeof(pid));
        if (is_hub_or_root(pid)) {
            break;
        }
        cur = parent;
    }

    /* Nothing disableable found: return the device (or the node) so it still
       appears; the attempt will report why it cannot be switched. */
    return usbdev ? usbdev : node;
}

/* ------------------------------------------------------------- listing -- */

/* One HID collection seen during enumeration, before grouping. */
typedef struct {
    char     target[HID_INSTANCE_MAX];  /* instance id of its switch target */
    bool     is_keyboard;
} raw_node_t;

#define RAW_MAX 256

static int collect_class(const GUID *cls, bool is_keyboard,
                         raw_node_t *raw, int have, int max)
{
    HDEVINFO set = SetupDiGetClassDevsA(cls, NULL, NULL, DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) {
        set_error("SetupDiGetClassDevs", GetLastError());
        return -1;
    }

    SP_DEVINFO_DATA dev = { .cbSize = sizeof(dev) };
    for (DWORD i = 0; have < max && SetupDiEnumDeviceInfo(set, i, &dev); i++) {
        DEVINST target = switch_target(dev.DevInst);
        get_instance_id(target, raw[have].target, HID_INSTANCE_MAX);
        if (raw[have].target[0] == '\0') {
            continue;                       /* no id: cannot be switched */
        }
        raw[have].is_keyboard = is_keyboard;
        have++;
    }

    SetupDiDestroyDeviceInfoList(set);
    return have;
}

/* Find the group with this target id, or -1. */
static int find_group(const hid_device_t *out, int count, const char *target)
{
    for (int i = 0; i < count; i++) {
        if (strcmp(out[i].instance_id, target) == 0) {
            return i;
        }
    }
    return -1;
}

static int by_vid_pid(const void *a, const void *b)
{
    const hid_device_t *x = a, *y = b;
    if (x->vid != y->vid) {
        return (x->vid < y->vid) ? -1 : 1;
    }
    if (x->pid != y->pid) {
        return (x->pid < y->pid) ? -1 : 1;
    }
    return strcmp(x->instance_id, y->instance_id);
}

int hid_enumerate(hid_device_t *out, int max)
{
    s_error[0] = '\0';
    if (max <= 0) {
        return 0;
    }

    static raw_node_t raw[RAW_MAX];
    int have = collect_class(&GUID_CLASS_KEYBOARD, true, raw, 0, RAW_MAX);
    if (have < 0) {
        return -1;
    }
    have = collect_class(&GUID_CLASS_MOUSE, false, raw, have, RAW_MAX);
    if (have < 0) {
        return -1;
    }

    int count = 0;
    for (int i = 0; i < have; i++) {
        int g = find_group(out, count, raw[i].target);
        if (g < 0) {
            if (count >= max) {
                continue;
            }
            g = count++;
            hid_device_t *d = &out[g];
            memset(d, 0, sizeof(*d));
            snprintf(d->instance_id, sizeof(d->instance_id), "%s", raw[i].target);

            DEVINST dn = 0;
            if (CM_Locate_DevNodeA(&dn, (DEVINSTID_A)raw[i].target,
                                   CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS) {
                get_name(dn, d->name, sizeof(d->name));
                d->disabled = devnode_disabled(dn);
                d->disableable = node_disableable(dn);
            }
            d->vid = id_after(raw[i].target, "VID_");
            d->pid = id_after(raw[i].target, "PID_");
            if (d->name[0] == '\0') {
                if (d->vid || d->pid) {
                    snprintf(d->name, sizeof(d->name), "Device %04X:%04X", d->vid, d->pid);
                } else {
                    snprintf(d->name, sizeof(d->name), "Input device");
                }
            }
        }
        if (raw[i].is_keyboard) {
            out[g].has_keyboard = true;
        } else {
            out[g].has_mouse = true;
        }
    }

    qsort(out, (size_t)count, sizeof(out[0]), by_vid_pid);
    return count;
}

/* ------------------------------------------------------------- switching - */

/*
 * The route Device Manager itself uses: DIF_PROPERTYCHANGE through the class
 * installer. Tried when CM_*_DevNode refuses a node that the device stack will
 * nonetheless let Device Manager switch (some composite devices report
 * CR_NOT_DISABLEABLE to the configuration manager yet disable this way).
 * Returns true only when the change was actually applied.
 */
static bool classinstaller_change(const char *instance_id, bool enable)
{
    HDEVINFO set = SetupDiCreateDeviceInfoList(NULL, NULL);
    if (set == INVALID_HANDLE_VALUE) {
        return false;
    }

    bool ok = false;
    SP_DEVINFO_DATA dd = { .cbSize = sizeof(dd) };
    if (SetupDiOpenDeviceInfoA(set, instance_id, NULL, 0, &dd)) {
        SP_PROPCHANGE_PARAMS pc = {
            .ClassInstallHeader = { .cbSize = sizeof(SP_CLASSINSTALL_HEADER),
                                    .InstallFunction = DIF_PROPERTYCHANGE },
            .StateChange = enable ? DICS_ENABLE : DICS_DISABLE,
            .Scope       = DICS_FLAG_GLOBAL,
            .HwProfile   = 0,
        };
        ok = SetupDiSetClassInstallParamsA(set, &dd, &pc.ClassInstallHeader, sizeof(pc)) &&
             SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, set, &dd);
    }

    SetupDiDestroyDeviceInfoList(set);
    return ok;
}

bool hid_set_enabled(const char *instance_id, bool enable, int *count)
{
    s_error[0] = '\0';
    if (count) {
        *count = 0;
    }
    if (instance_id == NULL || instance_id[0] == '\0') {
        return true;                        /* nothing to do */
    }

    DEVINST dn = 0;
    CONFIGRET cr = CM_Locate_DevNodeA(&dn, (DEVINSTID_A)instance_id,
                                      CM_LOCATE_DEVNODE_NORMAL);
    if (cr != CR_SUCCESS) {
        cr = CM_Locate_DevNodeA(&dn, (DEVINSTID_A)instance_id,
                                CM_LOCATE_DEVNODE_PHANTOM);
    }
    if (cr != CR_SUCCESS) {
        /* Gone from the bus: nothing to switch, and nothing to recover from. */
        return true;
    }

    cr = enable ? CM_Enable_DevNode(dn, 0) : CM_Disable_DevNode(dn, 0);
    if (cr != CR_SUCCESS) {
        /* The configuration manager refused; try the class-installer route. */
        if (classinstaller_change(instance_id, enable)) {
            if (count) {
                *count = 1;
            }
            return true;
        }
        set_error_cr(enable ? "enable device" : "disable device", cr);
        return false;
    }
    if (count) {
        *count = 1;
    }
    return true;
}

#else /* !_WIN32 */

bool hid_available(void)
{
    return false;
}

const char *hid_last_error(void)
{
    return "HID device switching is only implemented on Windows";
}

int hid_enumerate(hid_device_t *out, int max)
{
    (void)out;
    (void)max;
    return 0;
}

bool hid_set_enabled(const char *instance_id, bool enable, int *count)
{
    (void)instance_id;
    (void)enable;
    if (count) {
        *count = 0;
    }
    return false;
}

#endif /* _WIN32 */
