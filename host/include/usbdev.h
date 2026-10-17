/*
 * Thin wrapper over libusb-1.0 for the ESP32-S3 composite device.
 *
 * Owns discovery, the vendor-interface lookup, claiming, and bulk transfers, so
 * callers never touch libusb directly. Every function reports its own failures
 * to stderr; callers only need the return code.
 */

#ifndef USBDEV_H
#define USBDEV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <libusb.h>

typedef struct usbdev {
    libusb_context       *ctx;
    libusb_device        *dev;
    libusb_device_handle *handle;

    int           interface;    /*!< vendor interface number   */
    unsigned char ep_out;       /*!< bulk OUT endpoint address */
    unsigned char ep_in;        /*!< bulk IN endpoint address  */
    int           max_packet;   /*!< wMaxPacketSize, normally 64 */
    uint16_t      pid;          /*!< product id actually opened  */
    bool          claimed;
} usbdev_t;

/**
 * Find, open and claim the vendor interface of the first matching device.
 *
 * The interface is located by class 0xFF rather than by number, so it follows
 * the composite layout instead of assuming one. Returns 0 on success; on
 * failure nothing needs releasing.
 */
int usbdev_open(usbdev_t *d, uint16_t vid, uint16_t pid);

/* Most product ids usbdev_open_any() will consider in one call. */
#define USBDEV_MAX_PIDS 8

/**
 * Open whichever of several product ids is attached, by preference.
 *
 * The bus is enumerated once, then the entries of @p pids are tried in order:
 * the earliest one that is both attached and openable wins. A missing first
 * choice costs nothing and reports nothing, and one that is attached but
 * refuses to open -- another process holding its interface, say -- is stepped
 * over rather than ending the search.
 *
 * @param pids  Product ids, most preferred first. At most USBDEV_MAX_PIDS.
 * @param count How many.
 * @return 0 on success, leaving d->pid set to the one opened. -1 if none of
 *         them could be opened.
 */
int usbdev_open_any(usbdev_t *d, uint16_t vid, const uint16_t *pids, int count);

/**
 * @brief Whether one product id is on the bus, without opening it.
 *
 * Enumerates, so it is not free. A bus that cannot be read answers false.
 */
bool usbdev_attached(uint16_t vid, uint16_t pid);

void usbdev_close(usbdev_t *d);

/**
 * Release the process-wide libusb context.
 *
 * Call once, after the last usbdev_close(). Devices borrow this context, so
 * closing one does not, and must not, end it.
 */
void usbdev_shutdown(void);

void usbdev_print_identity(const usbdev_t *d);

/** Longest explanation usbdev_explain_absence() will write, with its NUL. */
#define USBDEV_REASON_MAX 240

/**
 * @brief Why usbdev_open_any() came back empty, in one line.
 *
 * "No device found" covers three situations that need different answers, and
 * the caller cannot tell them apart: nothing is attached; something is
 * attached under a product id this build does not know, which means the
 * firmware and the client are out of step; or the right device is attached and
 * would not open, which on Linux is almost always the udev rule.
 *
 * Writes into @p out whatever it can determine. Always NUL-terminates.
 */
void usbdev_explain_absence(uint16_t vid, const uint16_t *pids, int count,
                            char *out, size_t size);

/**
 * @brief Whether the opened device is still on the bus.
 *
 * A removed device is not reported the same way everywhere: Linux usually
 * fails a transfer with LIBUSB_ERROR_NO_DEVICE, while WinUSB reports an I/O
 * error that is indistinguishable from a glitch. This asks the bus directly
 * instead of reading the error code, by looking for the same bus address
 * still carrying the same vendor and product.
 *
 * Enumerates, so it is not free; call it when a transfer has failed, not
 * every frame.
 *
 * @return false only when the device is definitely gone. A bus that cannot be
 *         read says nothing about the device, and answers true.
 */
bool usbdev_present(const usbdev_t *d);

/** Send one bulk transfer. Returns 0, or a negative libusb error code. */
int usbdev_send(usbdev_t *d, const void *data, size_t len, unsigned timeout_ms);

/**
 * Receive one bulk transfer into @p buf, storing its length in @p out_len.
 *
 * Bytes can arrive together with LIBUSB_ERROR_TIMEOUT: libusb splits a
 * transfer into chunks to suit the operating system, and the deadline may
 * pass after some of them have landed. @p out_len is therefore worth reading
 * whatever this returns, and the data taken before the result is acted on.
 *
 * Returns 0 on success, LIBUSB_ERROR_TIMEOUT if the deadline passed, or
 * another negative libusb error code.
 */
int usbdev_recv(usbdev_t *d, void *buf, size_t cap, int *out_len,
                unsigned timeout_ms);

void usbdev_drain(usbdev_t *d);

#endif /* USBDEV_H */
