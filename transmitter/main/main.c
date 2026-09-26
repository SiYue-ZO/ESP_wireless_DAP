#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "status_led.h"
#include "usb_bridge.h"
#include "wireless_link.h"

static const char *TAG = "transmitter";

static void link_led_task(void *argument)
{
    (void)argument;
    bool previous = false;
    while (true) {
        bool connected = wireless_link_is_connected();
        if (connected != previous) {
            ESP_LOGI(TAG, "wireless receiver %s", connected ? "connected" : "disconnected");
            previous = connected;
        }
        status_led_set(connected ? STATUS_LED_CONNECTED : STATUS_LED_WAITING);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(status_led_init(STATUS_LED_ROLE_TRANSMITTER));
    status_led_set(STATUS_LED_BOOTING);

    esp_err_t ret = wireless_link_init(WIRELESS_LINK_ROLE_TRANSMITTER);
    if (ret != ESP_OK) {
        status_led_set(STATUS_LED_ERROR);
        ESP_LOGE(TAG, "wireless init failed: %s", esp_err_to_name(ret));
        return;
    }

    ret = usb_bridge_init();
    if (ret != ESP_OK) {
        status_led_set(STATUS_LED_ERROR);
        ESP_LOGE(TAG, "USB init failed: %s", esp_err_to_name(ret));
        return;
    }

    if (xTaskCreate(link_led_task, "link_led", 2048, NULL, 4, NULL) != pdPASS) {
        status_led_set(STATUS_LED_ERROR);
        ESP_LOGE(TAG, "failed to create link LED task");
        return;
    }
    ESP_LOGI(TAG, "transmitter ready");
}
