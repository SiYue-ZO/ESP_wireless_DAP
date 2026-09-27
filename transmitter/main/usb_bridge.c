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
#define DAP_COMMAND_CONNECT 0x02U
#define DAP_COMMAND_DISCONNECT 0x03U
#define DAP_COMMAND_TRANSFER_CONFIGURE 0x04U
#define DAP_COMMAND_TRANSFER 0x05U
#define DAP_COMMAND_TRANSFER_BLOCK 0x06U
#define DAP_COMMAND_TRANSFER_ABORT 0x07U
#define DAP_COMMAND_WRITE_ABORT 0x08U
#define DAP_COMMAND_DELAY 0x09U
#define DAP_COMMAND_RESET_TARGET 0x0aU
#define DAP_COMMAND_SWJ_PINS 0x10U
#define DAP_COMMAND_SWJ_CLOCK 0x11U
#define DAP_COMMAND_SWJ_SEQUENCE 0x12U
#define DAP_COMMAND_SWD_CONFIGURE 0x13U
#define DAP_COMMAND_SWD_SEQUENCE 0x1dU
#define DAP_COMMAND_QUEUE_COMMANDS 0x7eU
#define DAP_COMMAND_EXECUTE_COMMANDS 0x7fU
#define DAP_COMMAND_INVALID 0xffU
#define DAP_ERROR 0xffU
#define DAP_CAP_SWD 0x01U
#define DAP_TRANSFER_RNW (1U << 1)
#define DAP_TRANSFER_MATCH_VALUE (1U << 4)
#define DAP_TRANSFER_TIMESTAMP (1U << 7)
#define DAP_SWD_SEQUENCE_CLK 0x3fU
#define DAP_SWD_SEQUENCE_DIN 0x80U
#define DAP_HID_AGGREGATION_MAX 3U

#if CONFIG_WIRELESS_DAP_USB_BULK_V2
_Static_assert(CONFIG_TINYUSB_VENDOR_RX_BUFSIZE >=
                   CONFIG_WIRELESS_DAP_DAP_PACKET_SIZE *
                       CONFIG_WIRELESS_DAP_V2_PACKET_WINDOW,
               "CMSIS-DAP v2 RX FIFO must hold the advertised packet window");
_Static_assert(CONFIG_TINYUSB_VENDOR_TX_BUFSIZE >=
                   CONFIG_WIRELESS_DAP_DAP_PACKET_SIZE *
                       CONFIG_WIRELESS_DAP_V2_PACKET_WINDOW,
               "CMSIS-DAP v2 TX FIFO must hold the advertised packet window");
#endif

typedef struct {
    size_t length;
    uint8_t data[WIRELESS_LINK_UART_MTU];
} serial_chunk_t;

typedef enum {
    DAP_TRANSPORT_HID,
#if CONFIG_WIRELESS_DAP_USB_BULK_V2
    DAP_TRANSPORT_BULK,
#endif
} dap_transport_t;

typedef struct {
    dap_transport_t transport;
    size_t length;
    uint8_t data[WIRELESS_LINK_DAP_PACKET_SIZE];
} dap_request_t;

#if CONFIG_WIRELESS_DAP_HID_AGGREGATION
typedef struct {
    uint8_t count;
    size_t request_length[DAP_HID_AGGREGATION_MAX];
    uint8_t request[DAP_HID_AGGREGATION_MAX][WIRELESS_LINK_DAP_PACKET_SIZE_HID];
} dap_hid_batch_t;
#endif

static const char *TAG = "usb_bridge";
static QueueHandle_t s_dap_queue;
static QueueHandle_t s_serial_tx_queue;
static QueueHandle_t s_serial_config_queue;
static uint8_t s_cdc_callback_buffer[CONFIG_TINYUSB_CDC_RX_BUFSIZE];
#if CONFIG_WIRELESS_DAP_USB_BULK_V2
static uint8_t s_bulk_rx_buffer[WIRELESS_LINK_DAP_PACKET_SIZE];
static size_t s_bulk_rx_length;
static bool s_bulk_packet_size_negotiated;
#endif
static wireless_uart_config_t s_uart_config = {
    .baud_rate = 115200,
    .data_bits = 8,
    .parity = 0,
    .stop_bits = 1,
};

static size_t dap_transport_packet_size(dap_transport_t transport)
{
#if CONFIG_WIRELESS_DAP_USB_BULK_V2
    if (transport == DAP_TRANSPORT_BULK) {
        return WIRELESS_LINK_DAP_PACKET_SIZE;
    }
#else
    (void)transport;
#endif
    return WIRELESS_LINK_DAP_PACKET_SIZE_HID;
}

static uint16_t make_info_string(const char *text, uint8_t *response)
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

static bool handle_local_dap(dap_transport_t transport, const uint8_t *request,
                             uint8_t *response)
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
        response[2] =
#if CONFIG_WIRELESS_DAP_HID_AGGREGATION
            transport == DAP_TRANSPORT_HID ? DAP_HID_AGGREGATION_MAX : 1U;
#else
            CONFIG_WIRELESS_DAP_V2_PACKET_WINDOW;
#endif
        break;
    case DAP_INFO_PACKET_SIZE: {
        const size_t packet_size = dap_transport_packet_size(transport);
        response[0] = DAP_COMMAND_INFO;
        response[1] = 2;
        response[2] = (uint8_t)(packet_size >> 0);
        response[3] = (uint8_t)(packet_size >> 8);
        break;
    }
    default:
        response[0] = DAP_COMMAND_INFO;
        response[1] = 0;
        break;
    }
    return true;
}

static void write_hid_response(const uint8_t *response)
{
    while (!tud_hid_ready()) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    (void)tud_hid_report(0, response, WIRELESS_LINK_DAP_PACKET_SIZE_HID);
}

#if CONFIG_WIRELESS_DAP_HID_AGGREGATION
static bool add_size(size_t *value, size_t increment, size_t limit)
{
    if (*value > limit || increment > limit - *value) {
        return false;
    }
    *value += increment;
    return true;
}

static bool dap_transfer_lengths(const uint8_t *request, size_t *request_length,
                                 size_t *response_length)
{
    size_t in = 3U;
    size_t out = 3U;
    const uint8_t transfer_count = request[2];

    for (uint8_t i = 0; i < transfer_count; ++i) {
        if (in >= WIRELESS_LINK_DAP_PACKET_SIZE_HID) {
            return false;
        }
        const uint8_t transfer = request[in++];
        if ((transfer & DAP_TRANSFER_RNW) != 0U) {
            if ((transfer & DAP_TRANSFER_MATCH_VALUE) != 0U) {
                if (!add_size(&in, 4U, WIRELESS_LINK_DAP_PACKET_SIZE_HID)) {
                    return false;
                }
            } else if (!add_size(&out, 4U, WIRELESS_LINK_DAP_PACKET_SIZE_HID)) {
                return false;
            }
        } else if (!add_size(&in, 4U, WIRELESS_LINK_DAP_PACKET_SIZE_HID)) {
            return false;
        }
        if ((transfer & DAP_TRANSFER_TIMESTAMP) != 0U &&
            !add_size(&out, 4U, WIRELESS_LINK_DAP_PACKET_SIZE_HID)) {
            return false;
        }
    }

    *request_length = in;
    *response_length = out;
    return true;
}

static bool dap_swd_sequence_lengths(const uint8_t *request,
                                     size_t *request_length,
                                     size_t *response_length)
{
    size_t in = 2U;
    size_t out = 2U;
    const uint8_t sequence_count = request[1];

    for (uint8_t i = 0; i < sequence_count; ++i) {
        if (in >= WIRELESS_LINK_DAP_PACKET_SIZE_HID) {
            return false;
        }
        const uint8_t info = request[in++];
        uint8_t bit_count = info & DAP_SWD_SEQUENCE_CLK;
        if (bit_count == 0U) {
            bit_count = 64U;
        }
        const size_t byte_count = (bit_count + 7U) / 8U;
        if ((info & DAP_SWD_SEQUENCE_DIN) != 0U) {
            if (!add_size(&out, byte_count, WIRELESS_LINK_DAP_PACKET_SIZE_HID)) {
                return false;
            }
        } else if (!add_size(&in, byte_count, WIRELESS_LINK_DAP_PACKET_SIZE_HID)) {
            return false;
        }
    }

    *request_length = in;
    *response_length = out;
    return true;
}

static bool dap_hid_command_lengths(const uint8_t *request,
                                    size_t *request_length,
                                    size_t *response_length)
{
    switch (request[0]) {
    case DAP_COMMAND_CONNECT:
        *request_length = 2U;
        *response_length = 2U;
        return true;
    case DAP_COMMAND_DISCONNECT:
        *request_length = 1U;
        *response_length = 2U;
        return true;
    case DAP_COMMAND_TRANSFER_CONFIGURE:
        *request_length = 6U;
        *response_length = 2U;
        return true;
    case DAP_COMMAND_TRANSFER:
        return dap_transfer_lengths(request, request_length, response_length);
    case DAP_COMMAND_TRANSFER_BLOCK: {
        const size_t count = (size_t)request[2] | ((size_t)request[3] << 8);
        *request_length = 5U;
        *response_length = 4U;
        if ((request[4] & DAP_TRANSFER_RNW) != 0U) {
            return add_size(response_length, count * 4U,
                            WIRELESS_LINK_DAP_PACKET_SIZE_HID);
        }
        return add_size(request_length, count * 4U,
                        WIRELESS_LINK_DAP_PACKET_SIZE_HID);
    }
    case DAP_COMMAND_WRITE_ABORT:
        *request_length = 6U;
        *response_length = 2U;
        return true;
    case DAP_COMMAND_DELAY:
        *request_length = 3U;
        *response_length = 2U;
        return true;
    case DAP_COMMAND_RESET_TARGET:
        *request_length = 1U;
        *response_length = 3U;
        return true;
    case DAP_COMMAND_SWJ_PINS:
        *request_length = 7U;
        *response_length = 2U;
        return true;
    case DAP_COMMAND_SWJ_CLOCK:
        *request_length = 5U;
        *response_length = 2U;
        return true;
    case DAP_COMMAND_SWJ_SEQUENCE: {
        uint16_t bit_count = request[1];
        if (bit_count == 0U) {
            bit_count = 256U;
        }
        *request_length = 2U + (bit_count + 7U) / 8U;
        *response_length = 2U;
        return *request_length <= WIRELESS_LINK_DAP_PACKET_SIZE_HID;
    }
    case DAP_COMMAND_SWD_CONFIGURE:
        *request_length = 2U;
        *response_length = 2U;
        return true;
    case DAP_COMMAND_SWD_SEQUENCE:
        return dap_swd_sequence_lengths(request, request_length, response_length);
    case DAP_COMMAND_INFO:
    case DAP_COMMAND_HOST_STATUS:
    case DAP_COMMAND_TRANSFER_ABORT:
    case DAP_COMMAND_QUEUE_COMMANDS:
    case DAP_COMMAND_EXECUTE_COMMANDS:
    default:
        return false;
    }
}

static bool dap_hid_response_length(const uint8_t *request, size_t request_length,
                                    const uint8_t *response, size_t available,
                                    size_t *response_length)
{
    if (available == 0U || response[0] != request[0]) {
        return false;
    }

    if (request[0] == DAP_COMMAND_TRANSFER) {
        if (available < 3U || request_length < 3U || response[1] > request[2]) {
            return false;
        }
        size_t in = 3U;
        size_t out = 3U;
        for (uint8_t i = 0; i < response[1]; ++i) {
            if (in >= request_length) {
                return false;
            }
            const uint8_t transfer = request[in++];
            if ((transfer & DAP_TRANSFER_RNW) != 0U) {
                if ((transfer & DAP_TRANSFER_MATCH_VALUE) != 0U) {
                    if (!add_size(&in, 4U, request_length)) {
                        return false;
                    }
                } else if (!add_size(&out, 4U, available)) {
                    return false;
                }
            } else if (!add_size(&in, 4U, request_length)) {
                return false;
            }
            if ((transfer & DAP_TRANSFER_TIMESTAMP) != 0U &&
                !add_size(&out, 4U, available)) {
                return false;
            }
        }
        *response_length = out;
        return out <= WIRELESS_LINK_DAP_PACKET_SIZE_HID;
    }

    if (request[0] == DAP_COMMAND_TRANSFER_BLOCK) {
        if (available < 4U || request_length < 5U) {
            return false;
        }
        const size_t requested = (size_t)request[2] | ((size_t)request[3] << 8);
        const size_t completed = (size_t)response[1] | ((size_t)response[2] << 8);
        if (completed > requested) {
            return false;
        }
        size_t length = 4U;
        if ((request[4] & DAP_TRANSFER_RNW) != 0U &&
            !add_size(&length, completed * 4U, available)) {
            return false;
        }
        *response_length = length;
        return length <= WIRELESS_LINK_DAP_PACKET_SIZE_HID;
    }

    size_t ignored_request_length;
    size_t expected_response_length;
    if (!dap_hid_command_lengths(request, &ignored_request_length,
                                 &expected_response_length) ||
        ignored_request_length != request_length ||
        expected_response_length > available) {
        return false;
    }
    *response_length = expected_response_length;
    return true;
}

static void write_hid_error(const uint8_t *request)
{
    uint8_t response[WIRELESS_LINK_DAP_PACKET_SIZE_HID] = {0};
    response[0] = request[0] == DAP_COMMAND_INFO
                      ? DAP_COMMAND_INVALID
                      : request[0];
    response[1] = DAP_ERROR;
    write_hid_response(response);
}

static bool collect_hid_batch(const dap_request_t *first, dap_hid_batch_t *batch)
{
    size_t response_length;
    if (first->transport != DAP_TRANSPORT_HID ||
        !dap_hid_command_lengths(first->data, &batch->request_length[0],
                                 &response_length)) {
        return false;
    }

    batch->count = 1U;
    memcpy(batch->request[0], first->data, WIRELESS_LINK_DAP_PACKET_SIZE_HID);
    while (batch->count < DAP_HID_AGGREGATION_MAX) {
        dap_request_t next;
        const TickType_t wait = batch->count == 1U
                                    ? pdMS_TO_TICKS(CONFIG_WIRELESS_DAP_HID_AGGREGATION_WAIT_MS)
                                    : 0;
        if (xQueuePeek(s_dap_queue, &next, wait) != pdTRUE ||
            next.transport != DAP_TRANSPORT_HID) {
            break;
        }

        size_t next_request_length;
        if (!dap_hid_command_lengths(next.data, &next_request_length,
                                     &response_length)) {
            break;
        }
        if (xQueueReceive(s_dap_queue, &next, 0) != pdTRUE) {
            break;
        }
        batch->request_length[batch->count] = next_request_length;
        memcpy(batch->request[batch->count], next.data,
               WIRELESS_LINK_DAP_PACKET_SIZE_HID);
        batch->count++;
    }
    return batch->count >= 2U;
}

static void execute_hid_batch(const dap_hid_batch_t *batch)
{
    uint8_t request[WIRELESS_LINK_DAP_PACKET_SIZE] = {0};
    uint8_t response[WIRELESS_LINK_DAP_PACKET_SIZE] = {0};
    request[0] = DAP_COMMAND_EXECUTE_COMMANDS;
    request[1] = batch->count;

    size_t request_offset = 2U;
    for (uint8_t i = 0; i < batch->count; ++i) {
        memcpy(&request[request_offset], batch->request[i],
               batch->request_length[i]);
        request_offset += batch->request_length[i];
    }

    const esp_err_t ret = wireless_link_dap_exchange(
        request, sizeof(request), response, pdMS_TO_TICKS(1500));
    if (ret != ESP_OK || response[0] != DAP_COMMAND_EXECUTE_COMMANDS ||
        response[1] > batch->count) {
        ESP_LOGW(TAG, "aggregated DAP exchange failed: %s",
                 ret == ESP_OK ? "invalid response" : esp_err_to_name(ret));
        for (uint8_t i = 0; i < batch->count; ++i) {
            write_hid_error(batch->request[i]);
        }
        return;
    }

    size_t response_offset = 2U;
    uint8_t completed = 0U;
    for (; completed < response[1]; ++completed) {
        size_t child_length;
        if (!dap_hid_response_length(
                batch->request[completed], batch->request_length[completed],
                &response[response_offset], sizeof(response) - response_offset,
                &child_length)) {
            break;
        }
        uint8_t hid_response[WIRELESS_LINK_DAP_PACKET_SIZE_HID] = {0};
        memcpy(hid_response, &response[response_offset], child_length);
        response_offset += child_length;
        write_hid_response(hid_response);
    }
    for (; completed < batch->count; ++completed) {
        write_hid_error(batch->request[completed]);
    }
    status_led_pulse_activity();
}
#endif

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
    if (buffer[0] == DAP_COMMAND_TRANSFER_ABORT) {
        const esp_err_t ret = wireless_link_dap_abort();
        if (ret != ESP_OK) {
            ESP_LOGD(TAG, "DAP abort send failed: %s", esp_err_to_name(ret));
        }
        return;
    }

    dap_request_t request = {
        .transport = DAP_TRANSPORT_HID,
        .length = WIRELESS_LINK_DAP_PACKET_SIZE_HID,
    };
    if (size > WIRELESS_LINK_DAP_PACKET_SIZE_HID) {
        size = WIRELESS_LINK_DAP_PACKET_SIZE_HID;
    }
    memcpy(request.data, buffer, size);
    const BaseType_t queued = xQueueSend(s_dap_queue, &request, 0);
#if CONFIG_WIRELESS_DAP_DIAGNOSTICS
    ESP_LOGI(TAG, "hid_rx size=%u queued=%s at=%" PRId64 " us", size,
             queued == pdTRUE ? "yes" : "no", esp_timer_get_time());
#else
    (void)queued;
#endif
}

#if CONFIG_WIRELESS_DAP_USB_BULK_V2
typedef enum {
    DAP_REQUEST_INCOMPLETE,
    DAP_REQUEST_COMPLETE,
    DAP_REQUEST_INVALID,
} dap_request_parse_result_t;

static void queue_bulk_request(void)
{
    dap_request_t request = {
        .transport = DAP_TRANSPORT_BULK,
        .length = WIRELESS_LINK_DAP_PACKET_SIZE,
    };
    memcpy(request.data, s_bulk_rx_buffer, s_bulk_rx_length);

    if (request.data[0] == DAP_COMMAND_TRANSFER_ABORT) {
        const esp_err_t ret = wireless_link_dap_abort();
        if (ret != ESP_OK) {
            ESP_LOGD(TAG, "DAP abort send failed: %s", esp_err_to_name(ret));
        }
        s_bulk_rx_length = 0;
        return;
    }

    const BaseType_t queued = xQueueSend(s_dap_queue, &request, 0);
#if CONFIG_WIRELESS_DAP_DIAGNOSTICS
    ESP_LOGI(TAG, "bulk_rx size=%u queued=%s at=%" PRId64 " us",
             (unsigned)s_bulk_rx_length,
             queued == pdTRUE ? "yes" : "no", esp_timer_get_time());
#else
    (void)queued;
#endif
    s_bulk_rx_length = 0;
}

static dap_request_parse_result_t dap_bulk_request_length_internal(
    const uint8_t *request, size_t available, size_t capacity,
    size_t *request_length, unsigned depth)
{
    if (available == 0U) {
        return DAP_REQUEST_INCOMPLETE;
    }

    size_t length;
    switch (request[0]) {
    case DAP_COMMAND_INFO:
    case DAP_COMMAND_CONNECT:
    case DAP_COMMAND_SWD_CONFIGURE:
        length = 2U;
        break;
    case DAP_COMMAND_HOST_STATUS:
    case DAP_COMMAND_DELAY:
        length = 3U;
        break;
    case DAP_COMMAND_DISCONNECT:
    case DAP_COMMAND_TRANSFER_ABORT:
    case DAP_COMMAND_RESET_TARGET:
        length = 1U;
        break;
    case DAP_COMMAND_TRANSFER_CONFIGURE:
    case DAP_COMMAND_WRITE_ABORT:
        length = 6U;
        break;
    case DAP_COMMAND_SWJ_PINS:
        length = 7U;
        break;
    case DAP_COMMAND_SWJ_CLOCK:
        length = 5U;
        break;
    case DAP_COMMAND_SWJ_SEQUENCE: {
        if (available < 2U) {
            return DAP_REQUEST_INCOMPLETE;
        }
        uint16_t bit_count = request[1];
        if (bit_count == 0U) {
            bit_count = 256U;
        }
        length = 2U + (bit_count + 7U) / 8U;
        break;
    }
    case DAP_COMMAND_TRANSFER: {
        if (available < 3U) {
            return DAP_REQUEST_INCOMPLETE;
        }
        length = 3U;
        for (uint8_t i = 0; i < request[2]; ++i) {
            if (length >= capacity) {
                return DAP_REQUEST_INVALID;
            }
            if (length >= available) {
                return DAP_REQUEST_INCOMPLETE;
            }
            const uint8_t transfer = request[length++];
            const size_t data_length =
                ((transfer & DAP_TRANSFER_RNW) == 0U ||
                 (transfer & DAP_TRANSFER_MATCH_VALUE) != 0U)
                    ? 4U
                    : 0U;
            if (data_length > capacity - length) {
                return DAP_REQUEST_INVALID;
            }
            length += data_length;
            if (length > available) {
                return DAP_REQUEST_INCOMPLETE;
            }
        }
        break;
    }
    case DAP_COMMAND_TRANSFER_BLOCK: {
        if (available < 5U) {
            return DAP_REQUEST_INCOMPLETE;
        }
        const size_t count = (size_t)request[2] | ((size_t)request[3] << 8);
        const size_t data_length =
            (request[4] & DAP_TRANSFER_RNW) == 0U ? count * 4U : 0U;
        if (data_length > capacity - 5U) {
            return DAP_REQUEST_INVALID;
        }
        length = 5U + data_length;
        break;
    }
    case DAP_COMMAND_SWD_SEQUENCE: {
        if (available < 2U) {
            return DAP_REQUEST_INCOMPLETE;
        }
        length = 2U;
        for (uint8_t i = 0; i < request[1]; ++i) {
            if (length >= capacity) {
                return DAP_REQUEST_INVALID;
            }
            if (length >= available) {
                return DAP_REQUEST_INCOMPLETE;
            }
            const uint8_t info = request[length++];
            uint8_t bit_count = info & DAP_SWD_SEQUENCE_CLK;
            if (bit_count == 0U) {
                bit_count = 64U;
            }
            const size_t data_length =
                (info & DAP_SWD_SEQUENCE_DIN) == 0U
                    ? (bit_count + 7U) / 8U
                    : 0U;
            if (data_length > capacity - length) {
                return DAP_REQUEST_INVALID;
            }
            length += data_length;
            if (length > available) {
                return DAP_REQUEST_INCOMPLETE;
            }
        }
        break;
    }
    case DAP_COMMAND_QUEUE_COMMANDS:
    case DAP_COMMAND_EXECUTE_COMMANDS: {
        if (depth != 0U) {
            return DAP_REQUEST_INVALID;
        }
        if (available < 2U) {
            return DAP_REQUEST_INCOMPLETE;
        }
        length = 2U;
        for (uint8_t i = 0; i < request[1]; ++i) {
            if (length >= capacity) {
                return DAP_REQUEST_INVALID;
            }
            size_t child_length;
            const dap_request_parse_result_t result =
                dap_bulk_request_length_internal(
                    &request[length], available - length, capacity - length,
                    &child_length, depth + 1U);
            if (result != DAP_REQUEST_COMPLETE) {
                return result;
            }
            length += child_length;
        }
        break;
    }
    default:
        length = 1U;
        break;
    }

    if (length > capacity) {
        return DAP_REQUEST_INVALID;
    }
    if (length > available) {
        return DAP_REQUEST_INCOMPLETE;
    }
    *request_length = length;
    return DAP_REQUEST_COMPLETE;
}

static dap_request_parse_result_t dap_bulk_request_length(
    const uint8_t *request, size_t available, size_t *request_length)
{
    return dap_bulk_request_length_internal(
        request, available, WIRELESS_LINK_DAP_PACKET_SIZE, request_length, 0U);
}

#if CFG_TUD_API_V0_19_COMPAT
void tud_vendor_rx_cb(uint8_t interface, const uint8_t *buffer, uint16_t size)
#else
void tud_vendor_rx_cb(uint8_t interface, const uint8_t *buffer, uint32_t size)
#endif
{
    (void)buffer;
    (void)size;

    while (tud_vendor_n_available(interface) != 0U) {
        uint8_t chunk[USB_DAP_ENDPOINT_PACKET_SIZE];
        const uint32_t read = tud_vendor_n_read(interface, chunk, sizeof(chunk));
        if (read == 0U) {
            break;
        }
        if (read > sizeof(s_bulk_rx_buffer) - s_bulk_rx_length) {
            ESP_LOGW(TAG, "discarding oversized bulk request");
            s_bulk_rx_length = 0;
            continue;
        }
        memcpy(&s_bulk_rx_buffer[s_bulk_rx_length], chunk, read);
        s_bulk_rx_length += read;

        size_t request_length = 0U;
        const dap_request_parse_result_t parse_result =
            dap_bulk_request_length(s_bulk_rx_buffer, s_bulk_rx_length,
                                    &request_length);
        if (parse_result == DAP_REQUEST_COMPLETE &&
            request_length == s_bulk_rx_length) {
            queue_bulk_request();
        } else if (parse_result == DAP_REQUEST_INVALID ||
                   (parse_result == DAP_REQUEST_COMPLETE &&
                    request_length != s_bulk_rx_length)) {
            ESP_LOGW(TAG, "discarding malformed bulk request length=%u expected=%u",
                     (unsigned)s_bulk_rx_length, (unsigned)request_length);
            s_bulk_rx_length = 0U;
        } else if (read < USB_DAP_ENDPOINT_PACKET_SIZE ||
                   s_bulk_rx_length == sizeof(s_bulk_rx_buffer)) {
            queue_bulk_request();
        }
    }
}

static void write_bulk_response(const uint8_t *response, size_t length)
{
    while (tud_vendor_mounted() && tud_vendor_write_available() < length) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    if (!tud_vendor_mounted()) {
        return;
    }

    const uint32_t written = tud_vendor_write(response, length);
    if (written != length) {
        ESP_LOGE(TAG, "short bulk response write: expected=%u actual=%" PRIu32,
                 (unsigned)length, written);
        return;
    }
    (void)tud_vendor_write_flush();
}

static void usb_event_callback(tinyusb_event_t *event, void *argument)
{
    (void)event;
    (void)argument;
    s_bulk_rx_length = 0;
    s_bulk_packet_size_negotiated = false;
}
#endif

static void write_dap_usb_response(const dap_request_t *request,
                                   const uint8_t *response)
{
#if CONFIG_WIRELESS_DAP_USB_BULK_V2
    if (request->transport == DAP_TRANSPORT_BULK) {
        size_t length = s_bulk_packet_size_negotiated
                            ? WIRELESS_LINK_DAP_PACKET_SIZE - 1U
                            : USB_DAP_ENDPOINT_PACKET_SIZE - 1U;
        if (request->data[0] == DAP_COMMAND_INFO) {
            length = (size_t)response[1] + 2U;
        } else if (request->data[0] == DAP_COMMAND_HOST_STATUS) {
            length = 2U;
        } else if (request->data[0] == DAP_COMMAND_CONNECT) {
            length = USB_DAP_ENDPOINT_PACKET_SIZE - 1U;
        }
        write_bulk_response(response, length);
        if (request->data[0] == DAP_COMMAND_INFO &&
            request->data[1] == DAP_INFO_PACKET_SIZE) {
            s_bulk_packet_size_negotiated = true;
        } else if (request->data[0] == DAP_COMMAND_DISCONNECT) {
            s_bulk_packet_size_negotiated = false;
        }
        return;
    }
#endif
    write_hid_response(response);
}

static void make_dap_error(const dap_request_t *request, uint8_t *response)
{
    memset(response, 0, WIRELESS_LINK_DAP_PACKET_SIZE);
    response[0] = request->data[0] == DAP_COMMAND_INFO
                      ? DAP_COMMAND_INVALID
                      : request->data[0];
    response[1] = DAP_ERROR;
}

#if CONFIG_WIRELESS_DAP_V2_PACKET_WINDOW > 1
static bool is_local_dap_command(const dap_request_t *request)
{
    return request->data[0] == DAP_COMMAND_INFO ||
           request->data[0] == DAP_COMMAND_HOST_STATUS;
}
#endif

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
    dap_request_t request;
    uint8_t response[WIRELESS_LINK_DAP_PACKET_SIZE];

    while (true) {
        if (xQueueReceive(s_dap_queue, &request, portMAX_DELAY) != pdTRUE) {
            continue;
        }
#if CONFIG_WIRELESS_DAP_HID_AGGREGATION
        _Static_assert(WIRELESS_LINK_DAP_PACKET_SIZE == 256U,
                       "HID aggregation requires 256-byte DAP packets");
        dap_hid_batch_t batch;
        if (collect_hid_batch(&request, &batch)) {
            execute_hid_batch(&batch);
            continue;
        }
#endif
#if CONFIG_WIRELESS_DAP_V2_PACKET_WINDOW > 1
        if (is_local_dap_command(&request)) {
            memset(response, 0, sizeof(response));
            (void)handle_local_dap(request.transport, request.data, response);
            write_dap_usb_response(&request, response);
            continue;
        }

        dap_request_t requests[WIRELESS_LINK_DAP_WINDOW_MAX] = {request};
        uint8_t responses[WIRELESS_LINK_DAP_WINDOW_MAX]
                         [WIRELESS_LINK_DAP_PACKET_SIZE] = {0};
        wireless_dap_exchange_item_t items[WIRELESS_LINK_DAP_WINDOW_MAX] = {0};
        size_t count = 1U;
        dap_request_t next;
        if (xQueuePeek(s_dap_queue, &next,
                       pdMS_TO_TICKS(CONFIG_WIRELESS_DAP_WINDOW_WAIT_MS)) == pdTRUE &&
            !is_local_dap_command(&next) &&
            xQueueReceive(s_dap_queue, &requests[1], 0) == pdTRUE) {
            count = 2U;
        }
        for (size_t i = 0; i < count; ++i) {
            items[i].request = requests[i].data;
            items[i].length = requests[i].length;
            items[i].response = responses[i];
        }
#if CONFIG_WIRELESS_DAP_DIAGNOSTICS
        const int64_t window_start_us = esp_timer_get_time();
#endif
        (void)wireless_link_dap_exchange_window(
            items, count, pdMS_TO_TICKS(1500));
        bool activity = false;
        for (size_t i = 0; i < count; ++i) {
            if (items[i].result == ESP_OK) {
                activity = true;
            } else {
                make_dap_error(&requests[i], responses[i]);
                ESP_LOGW(TAG, "DAP window item %u failed: %s", (unsigned)i,
                         esp_err_to_name(items[i].result));
            }
        }
        if (activity) {
            status_led_pulse_activity();
        }
#if CONFIG_WIRELESS_DAP_DIAGNOSTICS
        ESP_LOGI(TAG, "dap_window count=%u elapsed=%" PRId64 " us",
                 (unsigned)count, esp_timer_get_time() - window_start_us);
#endif
        for (size_t i = 0; i < count; ++i) {
            write_dap_usb_response(&requests[i], responses[i]);
        }
        continue;
#endif
        memset(response, 0, sizeof(response));
#if CONFIG_WIRELESS_DAP_DIAGNOSTICS
        const int64_t exchange_start_us = esp_timer_get_time();
#endif
        if (!handle_local_dap(request.transport, request.data, response)) {
            esp_err_t ret = wireless_link_dap_exchange(
                request.data, request.length, response, pdMS_TO_TICKS(1500));
            if (ret != ESP_OK) {
                make_dap_error(&request, response);
                ESP_LOGW(TAG, "DAP exchange failed: %s", esp_err_to_name(ret));
            } else {
                status_led_pulse_activity();
            }
#if CONFIG_WIRELESS_DAP_DIAGNOSTICS
            ESP_LOGI(TAG, "dap_exchange elapsed=%" PRId64 " us",
                     esp_timer_get_time() - exchange_start_us);
#endif
        }

        write_dap_usb_response(&request, response);
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
    s_dap_queue = xQueueCreate(6, sizeof(dap_request_t));
    s_serial_tx_queue = xQueueCreate(16, sizeof(serial_chunk_t));
    s_serial_config_queue = xQueueCreate(1, sizeof(wireless_uart_config_t));
    ESP_RETURN_ON_FALSE(s_dap_queue && s_serial_tx_queue && s_serial_config_queue,
                        ESP_ERR_NO_MEM, TAG, "queue allocation failed");

    tinyusb_config_t config = TINYUSB_DEFAULT_CONFIG();
    config.descriptor.device = &g_usb_device_descriptor;
    config.descriptor.string = g_usb_string_descriptors;
    config.descriptor.string_count = (int)g_usb_string_descriptor_count;
    config.descriptor.full_speed_config = g_usb_configuration_descriptor;
#if CONFIG_WIRELESS_DAP_USB_BULK_V2
    config.event_cb = usb_event_callback;
#endif
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
#if CONFIG_WIRELESS_DAP_USB_BULK_V2
    ESP_LOGI(TAG, "USB CMSIS-DAP HID64 + v2 bulk%u + CDC ACM ready",
             (unsigned)WIRELESS_LINK_DAP_PACKET_SIZE);
#elif CONFIG_WIRELESS_DAP_HID_AGGREGATION
    ESP_LOGI(TAG, "USB CMSIS-DAP HID64 aggregation x%u + CDC ACM ready",
             DAP_HID_AGGREGATION_MAX);
#else
    ESP_LOGI(TAG, "USB CMSIS-DAP v1 + CDC ACM ready");
#endif
    return ESP_OK;
}
