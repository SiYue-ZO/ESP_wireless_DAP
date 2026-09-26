idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        wireless_link
        status_led
        dap_engine
        esp_driver_uart
)
