/*
 * Internal interface between usb_proto.c and usb_descriptors.c.
 * Not installed for consumers of the component.
 */

#pragma once

#include "usb_proto.h"
#include "tinyusb.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Interface layout. CDC occupies two consecutive interfaces bound by an IAD,
   which TUD_CDC_DESCRIPTOR emits. The vendor interface follows. */
enum {
    ITF_NUM_CDC = 0,
    ITF_NUM_CDC_DATA,
    ITF_NUM_VENDOR,
    ITF_NUM_TOTAL,
};

/*
 * Endpoint map. The S3 has 6 endpoints: 5 IN/OUT pairs plus 1 IN-only.
 * Used here: IN 1/2/3, OUT 2/3.
 */
#define EPNUM_CDC_NOTIF     0x81    /* interrupt IN  */
#define EPNUM_CDC_OUT       0x02    /* bulk OUT      */
#define EPNUM_CDC_IN        0x82    /* bulk IN       */
#define EPNUM_VENDOR_OUT    0x03    /* bulk OUT      */
#define EPNUM_VENDOR_IN     0x83    /* bulk IN       */

#define USB_PROTO_EP_SIZE       64  /* full-speed bulk maximum */
#define USB_PROTO_EP_NOTIF_SIZE 8

/**
 * @brief Apply the caller's identity to the descriptor tables.
 *
 * Must run before tinyusb_driver_install(). The strings are stored by pointer,
 * so they have to outlive the USB stack.
 */
void usb_proto_descriptors_init(const usb_proto_config_t *config);

const tusb_desc_device_t *usb_proto_device_descriptor(void);
const uint8_t            *usb_proto_config_descriptor(void);
const char              **usb_proto_string_descriptors(int *count);

#ifdef __cplusplus
}
#endif
