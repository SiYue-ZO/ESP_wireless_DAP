idf_component_register(
    SRCS
        "dap_app_compat.c"
        "dap_gpio.c"
        "dap_protocol.c"
        "dap_swd.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_driver_gpio
        freertos
        esp_hal_gpio
        esp_timer
        esp_hw_support
        soc
)
