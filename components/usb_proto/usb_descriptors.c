/*
 * Descriptor tables for the composite device.
 *
 * Windows sees two functions. usbccgp splits them, usbser takes the CDC and
 * hands out a COM port, and the MS OS 2.0 block below binds WinUSB to the
 * vendor function only, so neither needs Zadig.
 */

#include <string.h>

#include "usb_proto_descriptors.h"

/* bRequest the host uses to fetch the MS OS 2.0 descriptor set */
#define VENDOR_REQUEST_MICROSOFT    2

/* ----------------------------------------------------------- device ----- */

static tusb_desc_device_t s_device_desc = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0210,   /* 2.1 -- required for the host to ask for BOS */
    /* Composite device using an IAD. Without these three values Windows will
       not load usbccgp and the CDC function is never split out. */
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = 0x303A,
    .idProduct          = 0x6902,
    .bcdDevice          = 0x0102,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01,
};

/* ---------------------------------------------------- configuration ----- */

#define USB_CONFIG_TOTAL_LEN \
    (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_VENDOR_DESC_LEN)

static const uint8_t s_config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, USB_CONFIG_TOTAL_LEN, 0x00, 100),
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 4, EPNUM_CDC_NOTIF, USB_PROTO_EP_NOTIF_SIZE,
                       EPNUM_CDC_OUT, EPNUM_CDC_IN, USB_PROTO_EP_SIZE),
    TUD_VENDOR_DESCRIPTOR(ITF_NUM_VENDOR, 5,
                          EPNUM_VENDOR_OUT, EPNUM_VENDOR_IN, USB_PROTO_EP_SIZE),
};

/* --------------------------------------------------------- strings ------ */

static const char s_langid[] = { 0x09, 0x04 };   /* en-US */

static const char *s_string_desc[] = {
    s_langid,               /* 0: LANGID            */
    "SlimyRedstone",        /* 1: manufacturer      */
    "IOMeeter Dongle",      /* 2: product           */
    "0001",                 /* 3: serial            */
    "CDC Serial Port",      /* 4: CDC interface     */
    "Vendor Interface",     /* 5: vendor interface  */
};

/* ------------------------------------------- BOS + MS OS 2.0 (WinUSB) --- */

#define MS_OS_20_DESC_LEN   0xB2
#define BOS_TOTAL_LEN       (TUD_BOS_DESC_LEN + TUD_BOS_MICROSOFT_OS_DESC_LEN)

static const uint8_t s_bos_desc[] = {
    TUD_BOS_DESCRIPTOR(BOS_TOTAL_LEN, 1),
    TUD_BOS_MS_OS_20_DESCRIPTOR(MS_OS_20_DESC_LEN, VENDOR_REQUEST_MICROSOFT),
};

static const uint8_t s_ms_os_20_desc[] = {
    /* Set header: length, type, windows version, total length */
    U16_TO_U8S_LE(0x000A), U16_TO_U8S_LE(MS_OS_20_SET_HEADER_DESCRIPTOR),
    U32_TO_U8S_LE(0x06030000), U16_TO_U8S_LE(MS_OS_20_DESC_LEN),

    /* Configuration subset: length, type, config index, reserved, total length */
    U16_TO_U8S_LE(0x0008), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_CONFIGURATION),
    0, 0, U16_TO_U8S_LE(MS_OS_20_DESC_LEN - 0x0A),

    /* Function subset: length, type, first interface, reserved, subset length.
       bFirstInterface targets the vendor function only. The CDC function is
       left alone so usbser keeps it and Windows still gives you a COM port. */
    U16_TO_U8S_LE(0x0008), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_FUNCTION),
    ITF_NUM_VENDOR, 0, U16_TO_U8S_LE(MS_OS_20_DESC_LEN - 0x0A - 0x08),

    /* Compatible ID: bind WinUSB to this function */
    U16_TO_U8S_LE(0x0014), U16_TO_U8S_LE(MS_OS_20_FEATURE_COMPATBLE_ID),
    'W', 'I', 'N', 'U', 'S', 'B', 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

    /* Registry property: DeviceInterfaceGUIDs (REG_MULTI_SZ, UTF-16LE) */
    U16_TO_U8S_LE(MS_OS_20_DESC_LEN - 0x0A - 0x08 - 0x08 - 0x14),
    U16_TO_U8S_LE(MS_OS_20_FEATURE_REG_PROPERTY),
    U16_TO_U8S_LE(0x0007), U16_TO_U8S_LE(0x002A),
    'D', 0x00, 'e', 0x00, 'v', 0x00, 'i', 0x00, 'c', 0x00, 'e', 0x00,
    'I', 0x00, 'n', 0x00, 't', 0x00, 'e', 0x00, 'r', 0x00, 'f', 0x00,
    'a', 0x00, 'c', 0x00, 'e', 0x00, 'G', 0x00, 'U', 0x00, 'I', 0x00,
    'D', 0x00, 's', 0x00, 0x00, 0x00,
    U16_TO_U8S_LE(0x0050),
    '{', 0x00, '9', 0x00, '7', 0x00, '5', 0x00, 'F', 0x00, '4', 0x00,
    '4', 0x00, 'D', 0x00, '9', 0x00, '-', 0x00, '0', 0x00, 'D', 0x00,
    '0', 0x00, '8', 0x00, '-', 0x00, '4', 0x00, '3', 0x00, 'F', 0x00,
    'D', 0x00, '-', 0x00, '8', 0x00, 'B', 0x00, '3', 0x00, 'E', 0x00,
    '-', 0x00, '1', 0x00, '2', 0x00, '7', 0x00, 'C', 0x00, 'A', 0x00,
    '8', 0x00, 'A', 0x00, 'F', 0x00, 'F', 0x00, 'F', 0x00, '9', 0x00,
    'D', 0x00, '}', 0x00, 0x00, 0x00, 0x00, 0x00,
};

TU_VERIFY_STATIC(sizeof(s_ms_os_20_desc) == MS_OS_20_DESC_LEN,
                 "MS OS 2.0 descriptor length mismatch");

/* ----------------------------------------------- TinyUSB weak overrides -- */

uint8_t const *tud_descriptor_bos_cb(void)
{
    return s_bos_desc;
}

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage,
                                tusb_control_request_t const *request)
{
    /* Only act on SETUP; DATA and ACK stages need no work here. */
    if (stage != CONTROL_STAGE_SETUP) {
        return true;
    }

    if (request->bmRequestType_bit.type == TUSB_REQ_TYPE_VENDOR &&
        request->bRequest == VENDOR_REQUEST_MICROSOFT &&
        request->wIndex == 7) {
        /* wTotalLength lives at offset 8 of the set header */
        uint16_t total_len;
        memcpy(&total_len, s_ms_os_20_desc + 8, sizeof(total_len));
        return tud_control_xfer(rhport, request,
                                (void *)s_ms_os_20_desc, total_len);
    }

    /* Returning false stalls EP0. Add custom vendor requests above. */
    return false;
}

/* ------------------------------------------------------- internal API --- */

void usb_proto_descriptors_init(const usb_proto_config_t *config)
{
    if (config->vid) {
        s_device_desc.idVendor = config->vid;
    }
    if (config->pid) {
        s_device_desc.idProduct = config->pid;
    }
    if (config->bcd_device) {
        s_device_desc.bcdDevice = config->bcd_device;
    }
    if (config->manufacturer) {
        s_string_desc[1] = config->manufacturer;
    }
    if (config->product) {
        s_string_desc[2] = config->product;
    }
    if (config->serial) {
        s_string_desc[3] = config->serial;
    }
}

const tusb_desc_device_t *usb_proto_device_descriptor(void)
{
    return &s_device_desc;
}

const uint8_t *usb_proto_config_descriptor(void)
{
    return s_config_desc;
}

const char **usb_proto_string_descriptors(int *count)
{
    *count = (int)(sizeof(s_string_desc) / sizeof(s_string_desc[0]));
    return s_string_desc;
}
