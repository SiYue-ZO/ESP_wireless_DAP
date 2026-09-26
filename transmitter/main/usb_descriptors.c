#include "usb_descriptors.h"

enum {
    ITF_NUM_DAP = 0,
#if CONFIG_WIRELESS_DAP_USB_BULK_V2
    ITF_NUM_DAP_V2,
#endif
    ITF_NUM_CDC,
    ITF_NUM_CDC_DATA,
    ITF_NUM_TOTAL,
};

enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
    STRID_DAP,
#if CONFIG_WIRELESS_DAP_USB_BULK_V2
    STRID_DAP_V2,
#endif
    STRID_CDC,
};

#define EPNUM_DAP_OUT 0x01
#define EPNUM_DAP_IN 0x81
#define EPNUM_CDC_NOTIFY 0x82
#define EPNUM_CDC_OUT 0x03
#define EPNUM_CDC_IN 0x83
#define EPNUM_DAP_V2_OUT 0x04
#define EPNUM_DAP_V2_IN 0x84
#define USB_PACKET_SIZE 64U
#if CONFIG_WIRELESS_DAP_USB_BULK_V2
#define USB_DAP_V2_DESC_LEN TUD_VENDOR_DESC_LEN
#else
#define USB_DAP_V2_DESC_LEN 0
#endif
#define USB_CONFIG_TOTAL_LEN \
    (TUD_CONFIG_DESC_LEN + TUD_HID_INOUT_DESC_LEN + USB_DAP_V2_DESC_LEN + \
     TUD_CDC_DESC_LEN)

const tusb_desc_device_t g_usb_device_descriptor = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x303a,
    .idProduct = 0x4012,
    .bcdDevice = 0x0100,
    .iManufacturer = STRID_MANUFACTURER,
    .iProduct = STRID_PRODUCT,
    .iSerialNumber = STRID_SERIAL,
    .bNumConfigurations = 1,
};

const uint8_t g_usb_hid_report_descriptor[] = {
    TUD_HID_REPORT_DESC_GENERIC_INOUT(USB_PACKET_SIZE)
};

const uint8_t g_usb_configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, USB_CONFIG_TOTAL_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 250),

    TUD_HID_INOUT_DESCRIPTOR(ITF_NUM_DAP, STRID_DAP, HID_ITF_PROTOCOL_NONE,
                             sizeof(g_usb_hid_report_descriptor),
                             EPNUM_DAP_OUT, EPNUM_DAP_IN, USB_PACKET_SIZE, 1),

#if CONFIG_WIRELESS_DAP_USB_BULK_V2
    TUD_VENDOR_DESCRIPTOR(ITF_NUM_DAP_V2, STRID_DAP_V2,
                          EPNUM_DAP_V2_OUT, EPNUM_DAP_V2_IN, USB_PACKET_SIZE),
#endif

    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, STRID_CDC, EPNUM_CDC_NOTIFY, 8,
                       EPNUM_CDC_OUT, EPNUM_CDC_IN, USB_PACKET_SIZE),
};

const char *g_usb_string_descriptors[] = {
    (const char[]){0x09, 0x04},
    "Wireless DAP",
    "ESP32-S3 Wireless CMSIS-DAP TX",
    "WDAP-TX-0001",
    "Wireless CMSIS-DAP TX",
#if CONFIG_WIRELESS_DAP_USB_BULK_V2
    "CMSIS-DAP v2",
#endif
    "Wireless Target UART TX",
};

const size_t g_usb_string_descriptor_count =
    sizeof(g_usb_string_descriptors) / sizeof(g_usb_string_descriptors[0]);

const uint8_t *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return g_usb_hid_report_descriptor;
}
