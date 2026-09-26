#include "usb_bridge.h"

#include <inttypes.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "status_led.h"
#include "tinyusb.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_default_config.h"
#include "tusb.h"
#include "usb_descriptors.h"
#include "wireless_link.h"

#define DAP_INFO_VENDOR 0x01U
#define DAP_INFO_PRODUCT 0x02U
#define DAP_INFO_SERIAL 0x03U
#define DAP_INFO_FW_VERSION 0x04U
#define DAP_INFO_CAPABILITIES 0xf0U
#define DAP_INFO_PACKET_COUNT 0xfeU
#define DAP_INFO_PACKET_SIZE 0xffU
#define DAP_COMMAND_INFO 0x00U
#define DAP_COMMAND_HOST_STATUS 0x01U
#define DAP_COMMAND_INVALID 0xffU
#define DAP_ERROR 0xffU
#define DAP_CAP_SWD 0x01U

typedef struct {
    size_t length;
    uint8_t data[WIRELESS_LINK_UART_MTU];
} serial_chunk_t;

static const char *TAG = "usb_bridge";
static QueueHandle_t s_dap_queue;
static QueueHandle_t s_serial_tx_queue;
static QueueHandle_t s_serial_config_queue;
static uint8_t s_cdc_callback_buffer[CONFIG_TINYUSB_CDC_RX_BUFSIZE];
static wireless_uart_config_t s_uart_config = {
    .baud_rate = 115200,
    .data_bits = 8,
    .parity = 0,
    .stop_bits = 1,
};

static uint16_t make_info_string(const char *text, uint8_t response[64])
{
    size_t length = strlen(text) + 1U;
    if (length > 62U) {
        length = 62U;
    }
    response[0] = DAP_COMMAND_INFO;
    response[1] = (uint8_t)length;
    memcpy(&response[2], text, length);
    return (uint16_t)(length + 2U);
}

static bool handle_local_dap(const uint8_t request[64], uint8_t response[64])
{
    if (request[0] == DAP_COMMAND_HOST_STATUS) {
        response[0] = DAP_COMMAND_HOST_STATUS;
        response[1] = 0;
        return true;
    }
    if (request[0] != DAP_COMMAND_INFO) {
        return false;
    }

    switch (request[1]) {
    case DAP_INFO_VENDOR:
        (void)make_info_string("Wireless DAP", response);
        break;
    case DAP_INFO_PRODUCT:
        (void)make_info_string("ESP32-S3 Wireless CMSIS-DAP TX", response);
        break;
    case DAP_INFO_SERIAL:
        (void)make_info_string("WDAP-TX-0001", response);
        break;
    case DAP_INFO_FW_VERSION:
        (void)make_info_string("0.1.0", response);
        break;
    case DAP_INFO_CAPABILITIES:
        response[0] = DAP_COMMAND_INFO;
        response[1] = 1;
        response[2] = DAP_CAP_SWD;
        break;
    case DAP_INFO_PACKET_COUNT:
        response[0] = DAP_COMMAND_INFO;
        response[1] = 1;
        response[2] = 1;
        break;
    case DAP_INFO_PACKET_SIZE:
        response[0] = DAP_COMMAND_INFO;
        response[1] = 2;
        response[2] = 64;
        response[3] = 0;
        break;
    default:
        response[0] = DAP_COMMAND_INFO;
        response[1] = 0;
        break;
    }
    return true;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type,
                           const uint8_t *buffer, uint16_t size)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    if (buffer == NULL || size == 0U) {
        return;
    }

    uint8_t request[WIRELESS_LINK_DAP_PACKET_SIZE] = {0};
    if (size > sizeof(request)) {
        size = sizeof(request);
    }
    memcpy(request, buffer, size);
    const BaseType_t queued = xQueueSend(s_dap_queue, request, 0);
#if CONFIG_WIRELESS_DAP_DIAGNOSTICS
    ESP_LOGI(TAG, "hid_rx size=%u queued=%s at=%" PRId64 " us", size,
             queued == pdTRUE ? "yes" : "no", esp_timer_get_time());
#else
    (void)queued;
#endif
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t requested_length)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)requested_length;
    return 0;
}

static void cdc_rx_callback(int interface, cdcacm_event_t *event)
{
    (void)event;
    serial_chunk_t chunk = {0};
    size_t read = 0;
    if (tinyusb_cdcacm_read(interface, s_cdc_callback_buffer,
                            sizeof(s_cdc_callback_buffer), &read) != ESP_OK) {
        return;
    }

    size_t offset = 0;
    while (offset < read) {
        chunk.length = read - offset;
        if (chunk.length > sizeof(chunk.data)) {
            chunk.length = sizeof(chunk.data);
        }
        memcpy(chunk.data, &s_cdc_callback_buffer[offset], chunk.length);
        (void)xQueueSend(s_serial_tx_queue, &chunk, 0);
        offset += chunk.length;
    }
}

static void cdc_line_coding_callback(int interface, cdcacm_event_t *event)
{
    (void)interface;
    const cdc_line_coding_t *coding =
        event->line_coding_changed_data.p_line_coding;
    if (coding == NULL) {
        return;
    }
    wireless_uart_config_t config = s_uart_config;
    config.baud_rate = coding->bit_rate;
    config.data_bits = coding->data_bits;
    config.parity = coding->parity;
    config.stop_bits = coding->stop_bits == 0 ? 1 : 2;
    s_uart_config = config;
    (void)xQueueOverwrite(s_serial_config_queue, &config);
}

static void cdc_line_state_callback(int interface, cdcacm_event_t *event)
{
    (void)interface;
    wireless_uart_config_t config = s_uart_config;
    config.dtr = event->line_state_changed_data.dtr;
    config.rts = event->line_state_changed_data.rts;
    s_uart_config = config;
    (void)xQueueOverwrite(s_serial_config_queue, &config);
}

static void dap_task(void *argument)
{
    (void)argument;
    uint8_t request[WIRELESS_LINK_DAP_PACKET_SIZE];
    uint8_t response[WIRELESS_LINK_DAP_PACKET_SIZE];

    while (true) {
        if (xQueueReceive(s_dap_queue, request, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        memset(response, 0, sizeof(response));
#if CONFIG_WIRELESS_DAP_DIAGNOSTICS
        const int64_t exchange_start_us = esp_timer_get_time();
#endif
        if (!handle_local_dap(request, response)) {
            esp_err_t ret = wireless_link_dap_exchange(
                request, response, pdMS_TO_TICKS(1500));
            if (ret != ESP_OK) {
                response[0] = request[0] == 0 ? DAP_COMMAND_INVALID : request[0];
                response[1] = DAP_ERROR;
                ESP_LOGW(TAG, "DAP exchange failed: %s", esp_err_to_name(ret));
            } else {
                status_led_pulse_activity();
            }
#if CONFIG_WIRELESS_DAP_DIAGNOSTICS
            ESP_LOGI(TAG, "dap_exchange elapsed=%" PRId64 " us",
                     esp_timer_get_time() - exchange_start_us);
#endif
        }

        while (!tud_hid_ready()) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        (void)tud_hid_report(0, response, sizeof(response));
    }
}

static void serial_to_wireless_task(void *argument)
{
    (void)argument;
    serial_chunk_t chunk;
    wireless_uart_config_t config = s_uart_config;
    bool config_dirty = true;

    while (true) {
        if (xQueueReceive(s_serial_config_queue, &config, 0) == pdTRUE) {
            config_dirty = true;
        }
        if (config_dirty && wireless_link_is_connected() &&
            wireless_link_uart_send_config(&config) == ESP_OK) {
            config_dirty = false;
        }
        if (xQueueReceive(s_serial_tx_queue, &chunk, pdMS_TO_TICKS(20)) == pdTRUE) {
            if (wireless_link_uart_send(chunk.data, chunk.length) == ESP_OK) {
                status_led_pulse_activity();
            }
        }
    }
}

static void wireless_to_serial_task(void *argument)
{
    (void)argument;
    uint8_t data[WIRELESS_LINK_UART_MTU];
    size_t length;

    while (true) {
        if (wireless_link_uart_receive(data, sizeof(data), &length,
                                       portMAX_DELAY) != ESP_OK) {
            continue;
        }
        size_t offset = 0;
        while (offset < length) {
            size_t written = tinyusb_cdcacm_write_queue(
                TINYUSB_CDC_ACM_0, &data[offset], length - offset);
            offset += written;
            if (written == 0U) {
                vTaskDelay(pdMS_TO_TICKS(1));
            }
        }
        (void)tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, pdMS_TO_TICKS(20));
        status_led_pulse_activity();
    }
}

esp_err_t usb_bridge_init(void)
{
    s_dap_queue = xQueueCreate(4, WIRELESS_LINK_DAP_PACKET_SIZE);
    s_serial_tx_queue = xQueueCreate(16, sizeof(serial_chunk_t));
    s_serial_config_queue = xQueueCreate(1, sizeof(wireless_uart_config_t));
    ESP_RETURN_ON_FALSE(s_dap_queue && s_serial_tx_queue && s_serial_config_queue,
                        ESP_ERR_NO_MEM, TAG, "queue allocation failed");

    tinyusb_config_t config = TINYUSB_DEFAULT_CONFIG();
    config.descriptor.device = &g_usb_device_descriptor;
    config.descriptor.string = g_usb_string_descriptors;
    config.descriptor.string_count = (int)g_usb_string_descriptor_count;
    config.descriptor.full_speed_config = g_usb_configuration_descriptor;
    ESP_RETURN_ON_ERROR(tinyusb_driver_install(&config), TAG, "TinyUSB install failed");

    const tinyusb_config_cdcacm_t cdc_config = {
        .cdc_port = TINYUSB_CDC_ACM_0,
        .callback_rx = cdc_rx_callback,
        .callback_rx_wanted_char = NULL,
        .callback_line_state_changed = cdc_line_state_callback,
        .callback_line_coding_changed = cdc_line_coding_callback,
    };
    ESP_RETURN_ON_ERROR(tinyusb_cdcacm_init(&cdc_config), TAG, "CDC init failed");

    ESP_RETURN_ON_FALSE(
        xTaskCreate(dap_task, "usb_dap", 4096, NULL, 11, NULL) == pdPASS,
        ESP_ERR_NO_MEM, TAG, "create DAP task failed");
    ESP_RETURN_ON_FALSE(
        xTaskCreate(serial_to_wireless_task, "usb_to_uart", 4096, NULL, 8, NULL) == pdPASS,
        ESP_ERR_NO_MEM, TAG, "create serial TX task failed");
    ESP_RETURN_ON_FALSE(
        xTaskCreate(wireless_to_serial_task, "uart_to_usb", 4096, NULL, 8, NULL) == pdPASS,
        ESP_ERR_NO_MEM, TAG, "create serial RX task failed");

    (void)xQueueOverwrite(s_serial_config_queue, &s_uart_config);
    ESP_LOGI(TAG, "USB CMSIS-DAP v1 + CDC ACM ready");
    return ESP_OK;
}
