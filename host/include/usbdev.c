#include "usbdev.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define DRAIN_MS 100

static void print_string_desc(libusb_device_handle *h, uint8_t index,
                              const char *label)
{
    unsigned char buf[256];

    if (index == 0) {
        printf("%s: (none)\n", label);
        return;
    }

    int rc = libusb_get_string_descriptor_ascii(h, index, buf, sizeof(buf));
    if (rc < 0) {
        printf("%s: <%s>\n", label, libusb_error_name(rc));
    } else {
        printf("%s: %.*s\n", label, rc, buf);
    }
}

/*
 * Why the most recent open failed. Kept because usbdev_open() collapses every
 * failure to -1, and the interface is the only thing that knows the
 * difference between a permissions problem and a missing driver.
 */
static char s_open_error[USBDEV_REASON_MAX];

static void note_open_error(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsnprintf(s_open_error, sizeof(s_open_error), fmt, args);
    va_end(args);
}

static void report_open_failure(int rc, uint16_t vid, uint16_t pid)
{
#ifndef _WIN32
    /*
     * The device node is owned by root until a rule hands it to the logged-in
     * user. Reaching for sudo instead is the obvious move and the wrong one:
     * it opens the device but cuts the process off from the session's sound
     * server, so the mixer then reports itself unavailable.
     */
    if (rc == LIBUSB_ERROR_ACCESS) {
        note_open_error("%04X:%04X is attached but this user may not open it; "
                        "install the udev rule (sudo ./install.sh --udev) and "
                        "replug", vid, pid);
        fprintf(stderr,
            "device %04X:%04X was found, but this user may not open it.\n"
            "Install the udev rule once, then unplug and replug the device:\n"
            "  sudo ./install.sh --udev\n"
            "Running the whole program under sudo works for USB but loses the\n"
            "audio mixer: root cannot reach the desktop session's sound server.\n",
            vid, pid);
        return;
    }
#endif

    if (rc != LIBUSB_ERROR_NOT_SUPPORTED) {
        note_open_error("%04X:%04X would not open: %s", vid, pid,
                        libusb_error_name(rc));
        fprintf(stderr, "libusb_open: %s\n", libusb_error_name(rc));
        return;
    }

    note_open_error("%04X:%04X is enumerated but has no driver bound to its "
                    "vendor interface", vid, pid);

    fprintf(stderr,
        "device %04X:%04X is enumerated but cannot be opened.\n"
        "No WinUSB driver is bound to it. Fixes, in order:\n"
        "  1. Bump USB_BCD_DEVICE in the firmware and reflash -- Windows caches\n"
        "     MS OS descriptor results per VID/PID/bcdDevice and will not re-query.\n"
        "  2. As admin: delete the matching key under\n"
        "     HKLM\\SYSTEM\\CurrentControlSet\\Control\\usbflags,\n"
        "     then 'pnputil /remove-device' the instance and replug.\n"
        "  3. Last resort: bind WinUSB manually with Zadig.\n",
        vid, pid);
}

/*
 * Locate the vendor-specific interface and its bulk endpoint pair.
 *
 * The device is composite, so this walks past the CDC interfaces rather than
 * assuming the vendor function sits at a fixed index.
 */
static int find_vendor_interface(usbdev_t *d)
{
    struct libusb_config_descriptor *cfg = NULL;

    int rc = libusb_get_active_config_descriptor(d->dev, &cfg);
    if (rc != 0) {
        fprintf(stderr, "get_active_config_descriptor: %s\n",
                libusb_error_name(rc));
        return -1;
    }

    bool found = false;

    for (int i = 0; i < cfg->bNumInterfaces && !found; i++) {
        for (int a = 0; a < cfg->interface[i].num_altsetting && !found; a++) {
            const struct libusb_interface_descriptor *id =
                &cfg->interface[i].altsetting[a];

            if (id->bInterfaceClass != LIBUSB_CLASS_VENDOR_SPEC) {
                continue;
            }

            unsigned char out = 0, in = 0;
            int packet = 0;

            for (int e = 0; e < id->bNumEndpoints; e++) {
                const struct libusb_endpoint_descriptor *ep = &id->endpoint[e];

                if ((ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK)
                        != LIBUSB_TRANSFER_TYPE_BULK) {
                    continue;
                }
                if (ep->bEndpointAddress & LIBUSB_ENDPOINT_IN) {
                    in = ep->bEndpointAddress;
                } else {
                    out = ep->bEndpointAddress;
                }
                /* Both endpoints share a packet size on this device. */
                packet = ep->wMaxPacketSize;
            }

            if (out && in) {
                d->interface  = id->bInterfaceNumber;
                d->ep_out     = out;
                d->ep_in      = in;
                d->max_packet = packet;
                found = true;
            }
        }
    }

    libusb_free_config_descriptor(cfg);

    if (!found) {
        fprintf(stderr, "no vendor-specific interface with bulk IN+OUT found\n");
        return -1;
    }
    return 0;
}

/*
 * Try every device matching vid:pid rather than only the first. A second board,
 * or a stale handle held elsewhere, should not mask a usable one.
 */
static int open_matching(usbdev_t *d, uint16_t vid, uint16_t pid)
{
    libusb_device **list = NULL;

    ssize_t count = libusb_get_device_list(d->ctx, &list);
    if (count < 0) {
        fprintf(stderr, "get_device_list: %s\n", libusb_error_name((int)count));
        return -1;
    }

    int matches = 0;
    int last_rc = 0;

    for (ssize_t i = 0; i < count; i++) {
        struct libusb_device_descriptor desc;

        if (libusb_get_device_descriptor(list[i], &desc) != 0) {
            continue;
        }
        if (desc.idVendor != vid || desc.idProduct != pid) {
            continue;
        }
        matches++;

        int rc = libusb_open(list[i], &d->handle);
        if (rc == 0) {
            d->dev = libusb_ref_device(list[i]);
            libusb_free_device_list(list, 1);
            return 0;
        }

        last_rc = rc;
        d->handle = NULL;
    }

    libusb_free_device_list(list, 1);

    if (matches == 0) {
        fprintf(stderr, "device %04X:%04X not found\n", vid, pid);
    } else {
        report_open_failure(last_rc, vid, pid);
    }
    return -1;
}

/*
 * One libusb context for the whole process.
 *
 * The scan runs every few seconds while nothing is attached, and libusb's
 * Windows backend walks the entire bus and starts an event thread for each
 * context it is given. Creating and tearing one down on that cadence is not
 * what libusb is built for; a single long-lived context costs nothing.
 */
static libusb_context *s_ctx;

static int ensure_context(void)
{
    if (s_ctx != NULL) {
        return 0;
    }

    int rc = libusb_init(&s_ctx);
    if (rc != 0) {
        fprintf(stderr, "libusb_init: %s\n", libusb_error_name(rc));
        s_ctx = NULL;
        return -1;
    }
    return 0;
}

void usbdev_shutdown(void)
{
    if (s_ctx != NULL) {
        libusb_exit(s_ctx);
        s_ctx = NULL;
    }
}

/*
 * Mark which of @p pids are on the bus, in one enumeration.
 *
 * present[i] corresponds to pids[i], so the caller keeps its preference order
 * rather than learning only which one happened to be found first.
 */
static int scan_presence(uint16_t vid, const uint16_t *pids, int count,
                         bool *present)
{
    if (ensure_context() != 0) {
        return -1;
    }

    libusb_device **list = NULL;
    ssize_t n = libusb_get_device_list(s_ctx, &list);

    if (n < 0) {
        return -1;
    }

    for (ssize_t i = 0; i < n; i++) {
        struct libusb_device_descriptor desc;

        if (libusb_get_device_descriptor(list[i], &desc) != 0) {
            continue;
        }
        if (desc.idVendor != vid) {
            continue;
        }
        for (int p = 0; p < count; p++) {
            if (desc.idProduct == pids[p]) {
                present[p] = true;
                break;
            }
        }
    }

    libusb_free_device_list(list, 1);
    return 0;
}

bool usbdev_attached(uint16_t vid, uint16_t pid)
{
    bool present = false;

    if (scan_presence(vid, &pid, 1, &present) != 0) {
        return false;
    }
    return present;
}

int usbdev_open_any(usbdev_t *d, uint16_t vid, const uint16_t *pids, int count)
{
    if (count <= 0 || count > USBDEV_MAX_PIDS) {
        return -1;
    }

    bool present[USBDEV_MAX_PIDS] = { false };

    if (scan_presence(vid, pids, count, present) != 0) {
        return -1;
    }

    /*
     * Preference order, first match wins. An attached device that will not
     * open -- another process holding its interface, say -- is skipped rather
     * than ending the search, so a working second choice is still reached.
     */
    for (int i = 0; i < count; i++) {
        if (!present[i]) {
            continue;
        }
        if (usbdev_open(d, vid, pids[i]) == 0) {
            return 0;
        }
    }
    return -1;      /* nothing attached, or nothing openable */
}

int usbdev_open(usbdev_t *d, uint16_t vid, uint16_t pid)
{
    memset(d, 0, sizeof(*d));

    if (ensure_context() != 0) {
        return -1;
    }
    d->ctx = s_ctx;     /* borrowed: closing a device must not end it */

    if (open_matching(d, vid, pid) != 0) {
        usbdev_close(d);
        return -1;
    }

    if (find_vendor_interface(d) != 0) {
        usbdev_close(d);
        return -1;
    }

    /* No-op where the platform has no kernel drivers to detach. */
    libusb_set_auto_detach_kernel_driver(d->handle, 1);

    int rc = libusb_claim_interface(d->handle, d->interface);
    if (rc != 0) {
        fprintf(stderr, "claim_interface(%d): %s\n",
                d->interface, libusb_error_name(rc));
        if (rc == LIBUSB_ERROR_BUSY) {
            note_open_error("another process holds the vendor interface; close "
                            "the web page or the Python client first");
            fprintf(stderr,
                    "another process holds the interface -- close the Python "
                    "client or the web page first\n");
        } else {
            note_open_error("the vendor interface could not be claimed: %s",
                            libusb_error_name(rc));
        }
        usbdev_close(d);
        return -1;
    }
    d->claimed = true;
    d->pid = pid;

    printf("interface %d: OUT=0x%02X IN=0x%02X mps=%d\n",
           d->interface, d->ep_out, d->ep_in, d->max_packet);
    return 0;
}

void usbdev_close(usbdev_t *d)
{
    if (d->claimed) {
        libusb_release_interface(d->handle, d->interface);
        d->claimed = false;
    }
    if (d->handle) {
        libusb_close(d->handle);
        d->handle = NULL;
    }
    if (d->dev) {
        libusb_unref_device(d->dev);
        d->dev = NULL;
    }
    /* The context is shared and outlives every device; only usbdev_shutdown()
       closes it. */
    d->ctx = NULL;
}

void usbdev_print_identity(const usbdev_t *d)
{
    struct libusb_device_descriptor desc;

    if (libusb_get_device_descriptor(d->dev, &desc) != 0) {
        return;
    }

    /* Read the indices from the descriptor instead of assuming 1/2/3. */
    print_string_desc(d->handle, desc.iManufacturer, "manufacturer");
    print_string_desc(d->handle, desc.iProduct,      "product");
    print_string_desc(d->handle, desc.iSerialNumber, "serial");
}

void usbdev_explain_absence(uint16_t vid, const uint16_t *pids, int count,
                           char *out, size_t size)
{
    if (out == NULL || size == 0) {
        return;
    }
    out[0] = '\0';

    if (count <= 0) {
        snprintf(out, size, "this build knows no product ids to look for");
        return;
    }
    if (ensure_context() != 0) {
        snprintf(out, size, "libusb is not available");
        return;
    }

    libusb_device **list = NULL;
    ssize_t n = libusb_get_device_list(s_ctx, &list);

    if (n < 0) {
        snprintf(out, size, "the USB bus could not be read: %s",
                 libusb_error_name((int)n));
        return;
    }

    bool wanted = false;        /* one of ours is attached */
    bool stranger = false;      /* our vendor, a product id we do not know */
    uint16_t stranger_pid = 0;

    for (ssize_t i = 0; i < n; i++) {
        struct libusb_device_descriptor desc;

        if (libusb_get_device_descriptor(list[i], &desc) != 0 ||
            desc.idVendor != vid) {
            continue;
        }

        bool known = false;
        for (int p = 0; p < count; p++) {
            if (desc.idProduct == pids[p]) {
                known = true;
                break;
            }
        }

        if (known) {
            wanted = true;
        } else if (!stranger) {
            stranger = true;
            stranger_pid = desc.idProduct;
        }
    }

    libusb_free_device_list(list, 1);

    if (wanted) {
        /* Attached and ours, so the open is what failed. */
        snprintf(out, size, "%s", s_open_error[0]
                 ? s_open_error
                 : "the device is attached but would not open");
        return;
    }

    if (stranger) {
        /*
         * The case a product id change produces: the hardware is there and
         * answering, under a number this build was not told about.
         */
        int at = snprintf(out, size,
                          "%04X:%04X is attached, but this build looks for ",
                          vid, stranger_pid);

        for (int p = 0; p < count && at > 0 && (size_t)at < size; p++) {
            at += snprintf(out + at, size - (size_t)at, "%s%04X",
                           p ? "/" : "", pids[p]);
        }
        if (at > 0 && (size_t)at < size) {
            snprintf(out + at, size - (size_t)at,
                     " -- the firmware and the client disagree; reflash or "
                     "check the product id");
        }
        return;
    }

    snprintf(out, size, "nothing with vendor %04X is attached", vid);
}

bool usbdev_present(const usbdev_t *d)
{
    if (d == NULL || d->ctx == NULL || d->dev == NULL) {
        return false;
    }

    /*
     * The handle keeps its libusb_device alive after a removal, so these still
     * read the values the device had when it was opened -- which is exactly
     * what makes them worth comparing against the bus as it is now.
     */
    uint8_t bus = libusb_get_bus_number(d->dev);
    uint8_t address = libusb_get_device_address(d->dev);

    struct libusb_device_descriptor want;
    if (libusb_get_device_descriptor(d->dev, &want) != 0) {
        return true;            /* cannot tell; do not call it gone */
    }

    libusb_device **list = NULL;
    ssize_t count = libusb_get_device_list(d->ctx, &list);
    if (count < 0) {
        return true;            /* the bus cannot be read, not the device */
    }

    bool found = false;

    for (ssize_t i = 0; i < count && !found; i++) {
        if (libusb_get_bus_number(list[i]) != bus ||
            libusb_get_device_address(list[i]) != address) {
            continue;
        }

        /* An address is reused once its device leaves, so the identity is
           checked too rather than trusting the slot. */
        struct libusb_device_descriptor got;
        if (libusb_get_device_descriptor(list[i], &got) != 0) {
            continue;
        }
        found = (got.idVendor == want.idVendor &&
                 got.idProduct == want.idProduct);
    }

    libusb_free_device_list(list, 1);
    return found;
}

int usbdev_send(usbdev_t *d, const void *data, size_t len, unsigned timeout_ms)
{
    int transferred = 0;

    /* libusb does not modify the buffer for an OUT transfer. */
    int rc = libusb_bulk_transfer(d->handle, d->ep_out,
                                  (unsigned char *)(uintptr_t)data,
                                  (int)len, &transferred, timeout_ms);
    if (rc != 0) {
        fprintf(stderr, "OUT failed: %s\n", libusb_error_name(rc));
        return rc;
    }
    if ((size_t)transferred != len) {
        fprintf(stderr, "OUT short write: %d of %u bytes\n",
                transferred, (unsigned)len);
        return LIBUSB_ERROR_IO;
    }
    return 0;
}

int usbdev_recv(usbdev_t *d, void *buf, size_t cap, int *out_len,
                unsigned timeout_ms)
{
    int transferred = 0;

    int rc = libusb_bulk_transfer(d->handle, d->ep_in, (unsigned char *)buf,
                                  (int)cap, &transferred, timeout_ms);

    /*
     * Reported whatever the result. libusb splits a transfer into chunks to
     * suit the operating system, so a timeout can expire after some of them
     * have already landed -- the data is real and is gone once this returns
     * without it. Discarding it truncates a message mid-flight, and the framer
     * then waits for a remainder that was thrown away.
     */
    *out_len = (transferred > 0) ? transferred : 0;
    return rc;
}

void usbdev_drain(usbdev_t *d)
{
    unsigned char buf[512];
    int len = 0;

    while (usbdev_recv(d, buf, sizeof(buf), &len, DRAIN_MS) == 0 && len > 0) {
        /* discard */
    }
}
