#pragma once

#include <stddef.h>
#include <stdint.h>

#include "tusb.h"

extern const tusb_desc_device_t g_usb_device_descriptor;
extern const uint8_t g_usb_configuration_descriptor[];
extern const uint8_t g_usb_hid_report_descriptor[];
extern const char *g_usb_string_descriptors[];
extern const size_t g_usb_string_descriptor_count;
