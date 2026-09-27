#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif
#ifndef CONFIG_WIRELESS_DAP_DAP_PACKET_SIZE
#define CONFIG_WIRELESS_DAP_DAP_PACKET_SIZE 64
#endif
#ifndef CONFIG_WIRELESS_DAP_V2_PACKET_WINDOW
#define CONFIG_WIRELESS_DAP_V2_PACKET_WINDOW 1
#endif
#define WIRELESS_LINK_DAP_PACKET_SIZE CONFIG_WIRELESS_DAP_DAP_PACKET_SIZE
#define WIRELESS_LINK_DAP_PACKET_SIZE_HID 64U
#define WIRELESS_LINK_DAP_WINDOW_MAX 2U
#define WIRELESS_LINK_UART_MTU 192U
#define WIRELESS_LINK_PROTOCOL_V1 1U
#define WIRELESS_LINK_PROTOCOL_V2 2U
#define WIRELESS_LINK_CAP_DAP64 (1U << 0)
#define WIRELESS_LINK_CAP_BULK_USB (1U << 1)
#define WIRELESS_LINK_CAP_WINDOW (1U << 2)
#define WIRELESS_LINK_CAP_ESPNOW_V2 (1U << 3)
#define WIRELESS_LINK_CAP_DAP256 (1U << 4)
#define WIRELESS_LINK_CAP_HID_AGGREGATION (1U << 5)

typedef enum {
    WIRELESS_LINK_ROLE_TRANSMITTER = 1,
    WIRELESS_LINK_ROLE_RECEIVER = 2,
} wireless_link_role_t;

typedef struct {
    uint32_t baud_rate;
    uint8_t data_bits;
    uint8_t parity;
    uint8_t stop_bits;
    uint8_t dtr;
    uint8_t rts;
} __attribute__((packed)) wireless_uart_config_t;

typedef struct {
    uint16_t max_dap_packet_size;
    uint16_t max_payload_size;
    uint8_t packet_window;
    uint8_t flags;
    uint16_t reserved;
} __attribute__((packed)) wireless_link_capabilities_t;

typedef struct {
    uint32_t tx_success;
    uint32_t tx_fail;
    uint32_t tx_busy;
    uint32_t tx_dropped;
    uint32_t rx_frames;
    uint32_t dap_requests;
    uint32_t dap_retries;
    uint32_t dap_timeouts;
    uint32_t uart_dropped;
    uint32_t last_dap_latency_us;
    int8_t last_rssi_dbm;
} wireless_link_stats_t;

typedef struct {
    const uint8_t *request;
    size_t length;
    uint8_t *response;
    esp_err_t result;
} wireless_dap_exchange_item_t;

esp_err_t wireless_link_init(wireless_link_role_t role);
bool wireless_link_is_connected(void);
void wireless_link_get_peer_mac(uint8_t mac[6]);
bool wireless_link_get_peer_capabilities(wireless_link_capabilities_t *capabilities);
void wireless_link_get_stats(wireless_link_stats_t *stats);
void wireless_link_reset_stats(void);

esp_err_t wireless_link_dap_exchange(const uint8_t *request, size_t length,
                                     uint8_t *response,
                                     TickType_t timeout);
esp_err_t wireless_link_dap_exchange_window(wireless_dap_exchange_item_t *items,
                                            size_t count,
                                            TickType_t timeout);
esp_err_t wireless_link_dap_receive(uint8_t request[WIRELESS_LINK_DAP_PACKET_SIZE],
                                    size_t *length, uint16_t *sequence,
                                    TickType_t timeout);
esp_err_t wireless_link_dap_reply(uint16_t sequence,
                                  const uint8_t *response, size_t length);
esp_err_t wireless_link_dap_abort(void);
esp_err_t wireless_link_dap_receive_abort(TickType_t timeout);

esp_err_t wireless_link_uart_send(const uint8_t *data, size_t length);
esp_err_t wireless_link_uart_receive(uint8_t *data, size_t capacity,
                                     size_t *length, TickType_t timeout);
esp_err_t wireless_link_uart_send_config(const wireless_uart_config_t *config);
esp_err_t wireless_link_uart_receive_config(wireless_uart_config_t *config,
                                            TickType_t timeout);

#ifdef __cplusplus
}
#endif
