#include <string.h>
#include <inttypes.h>

#include "dap_engine.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "status_led.h"
#include "wireless_link.h"

#define TARGET_UART UART_NUM_1
#define UART_BUFFER_SIZE 4096

static const char *TAG = "receiver";

static esp_err_t apply_uart_config(const wireless_uart_config_t *config)
{
    if (config->baud_rate < 1200U || config->baud_rate > 3000000U) {
        return ESP_ERR_INVALID_ARG;
    }

    uart_word_length_t data_bits;
    switch (config->data_bits) {
    case 5:
        data_bits = UART_DATA_5_BITS;
        break;
    case 6:
        data_bits = UART_DATA_6_BITS;
        break;
    case 7:
        data_bits = UART_DATA_7_BITS;
        break;
    case 8:
    default:
        data_bits = UART_DATA_8_BITS;
        break;
    }

    uart_parity_t parity;
    switch (config->parity) {
    case 1:
        parity = UART_PARITY_ODD;
        break;
    case 2:
        parity = UART_PARITY_EVEN;
        break;
    default:
        parity = UART_PARITY_DISABLE;
        break;
    }

    uart_stop_bits_t stop_bits =
        config->stop_bits >= 2 ? UART_STOP_BITS_2 : UART_STOP_BITS_1;
    ESP_RETURN_ON_ERROR(uart_set_baudrate(TARGET_UART, config->baud_rate),
                        TAG, "set baud rate failed");
    ESP_RETURN_ON_ERROR(uart_set_word_length(TARGET_UART, data_bits),
                        TAG, "set data bits failed");
    ESP_RETURN_ON_ERROR(uart_set_parity(TARGET_UART, parity),
                        TAG, "set parity failed");
    ESP_RETURN_ON_ERROR(uart_set_stop_bits(TARGET_UART, stop_bits),
                        TAG, "set stop bits failed");
    ESP_LOGI(TAG, "target UART: %" PRIu32 " %u%c%u",
             config->baud_rate, config->data_bits,
             config->parity == 1 ? 'O' : (config->parity == 2 ? 'E' : 'N'),
             config->stop_bits >= 2 ? 2 : 1);
    return ESP_OK;
}

static esp_err_t target_uart_init(void)
{
    const uart_config_t config = {
        .baud_rate = CONFIG_WIRELESS_DAP_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(uart_param_config(TARGET_UART, &config),
                        TAG, "UART configure failed");
    ESP_RETURN_ON_ERROR(
        uart_set_pin(TARGET_UART,
                     CONFIG_WIRELESS_DAP_UART_TX_GPIO,
                     CONFIG_WIRELESS_DAP_UART_RX_GPIO,
                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE),
        TAG, "UART pin setup failed");
    return uart_driver_install(TARGET_UART, UART_BUFFER_SIZE, UART_BUFFER_SIZE,
                               0, NULL, 0);
}

static void dap_task(void *argument)
{
    (void)argument;
    uint8_t request[DAP_PACKET_SIZE];
    uint8_t response[DAP_PACKET_SIZE];

    while (true) {
        uint16_t sequence;
        if (wireless_link_dap_receive(request, &sequence, portMAX_DELAY) != ESP_OK) {
            continue;
        }
        memset(response, 0, sizeof(response));
#if CONFIG_WIRELESS_DAP_DIAGNOSTICS
        const int64_t execute_start_us = esp_timer_get_time();
#endif
        (void)dap_protocol_execute(request, sizeof(request), response);
#if CONFIG_WIRELESS_DAP_DIAGNOSTICS
        ESP_LOGI(TAG, "dap seq=%u execute=%" PRId64 " us", sequence,
                 esp_timer_get_time() - execute_start_us);
#endif
        if (wireless_link_dap_reply(sequence, response) == ESP_OK) {
            status_led_pulse_activity();
        }
    }
}

static void uart_to_wireless_task(void *argument)
{
    (void)argument;
    uint8_t data[WIRELESS_LINK_UART_MTU];
    while (true) {
        int length = uart_read_bytes(TARGET_UART, data, sizeof(data),
                                     pdMS_TO_TICKS(20));
        if (length > 0 &&
            wireless_link_uart_send(data, (size_t)length) == ESP_OK) {
            status_led_pulse_activity();
        }
    }
}

static void wireless_to_uart_task(void *argument)
{
    (void)argument;
    uint8_t data[WIRELESS_LINK_UART_MTU];
    size_t length;
    wireless_uart_config_t config;

    while (true) {
        if (wireless_link_uart_receive_config(&config, 0) == ESP_OK) {
            (void)apply_uart_config(&config);
        }
        if (wireless_link_uart_receive(data, sizeof(data), &length,
                                       pdMS_TO_TICKS(20)) == ESP_OK) {
            (void)uart_write_bytes(TARGET_UART, data, length);
            status_led_pulse_activity();
        }
    }
}

static void link_led_task(void *argument)
{
    (void)argument;
    bool previous = false;
    while (true) {
        const bool connected = wireless_link_is_connected();
        if (connected != previous) {
            ESP_LOGI(TAG, "wireless transmitter %s",
                     connected ? "connected" : "disconnected");
            previous = connected;
        }
        status_led_set(connected ? STATUS_LED_CONNECTED : STATUS_LED_WAITING);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(status_led_init(STATUS_LED_ROLE_RECEIVER));
    status_led_set(STATUS_LED_BOOTING);
    ESP_ERROR_CHECK(target_uart_init());
    dap_protocol_init();
    ESP_ERROR_CHECK(wireless_link_init(WIRELESS_LINK_ROLE_RECEIVER));

    if (xTaskCreatePinnedToCore(
            dap_task, "dap_swd", 6144, NULL, 12, NULL, 1) != pdPASS ||
        xTaskCreate(
            uart_to_wireless_task, "uart_to_radio", 4096, NULL, 8, NULL) != pdPASS ||
        xTaskCreate(
            wireless_to_uart_task, "radio_to_uart", 4096, NULL, 8, NULL) != pdPASS ||
        xTaskCreate(
            link_led_task, "link_led", 2048, NULL, 4, NULL) != pdPASS) {
        status_led_set(STATUS_LED_ERROR);
        ESP_LOGE(TAG, "failed to create receiver tasks");
        return;
    }

    ESP_LOGI(TAG,
             "receiver ready; SWD clk=%d io=%d reset=%d, UART tx=%d rx=%d",
             CONFIG_WIRELESS_DAP_SWD_CLK_GPIO,
             CONFIG_WIRELESS_DAP_SWD_IO_GPIO,
             CONFIG_WIRELESS_DAP_RESET_GPIO,
             CONFIG_WIRELESS_DAP_UART_TX_GPIO,
             CONFIG_WIRELESS_DAP_UART_RX_GPIO);
}
