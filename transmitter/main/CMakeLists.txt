idf_component_register(
    SRCS
        "main.c"
        "usb_bridge.c"
        "usb_descriptors.c"
    INCLUDE_DIRS "."
    REQUIRES
        wireless_link
        status_led
        esp_timer
)
