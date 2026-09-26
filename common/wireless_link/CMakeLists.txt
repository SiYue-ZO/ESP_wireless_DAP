idf_component_register(
    SRCS "wireless_link.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_wifi
        esp_event
        esp_netif
        nvs_flash
        esp_timer
)
