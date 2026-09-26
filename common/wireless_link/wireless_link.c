#include "wireless_link.h"

#include <inttypes.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#define LINK_MAGIC 0x50414457UL /* "WDAP" in little endian */
#define LINK_QUEUE_DEPTH 16U
#define LINK_TX_QUEUE_DEPTH 24U
#define LINK_HELLO_PERIOD_MS 500U
#define LINK_HEARTBEAT_PERIOD_MS 1000U
#define LINK_LOST_TIMEOUT_MS 3500U
#ifndef CONFIG_WIRELESS_DAP_DAP_RETRY_MS
#define CONFIG_WIRELESS_DAP_DAP_RETRY_MS 45
#endif
#ifndef CONFIG_WIRELESS_DAP_TX_DELAY_MS
#define CONFIG_WIRELESS_DAP_TX_DELAY_MS 2
#endif
#ifndef CONFIG_WIRELESS_DAP_DIAGNOSTICS
#define CONFIG_WIRELESS_DAP_DIAGNOSTICS 0
#endif
#ifndef CONFIG_WIRELESS_DAP_PROTOCOL_VERSION
#define CONFIG_WIRELESS_DAP_PROTOCOL_VERSION WIRELESS_LINK_PROTOCOL_V1
#endif
#ifndef CONFIG_WIRELESS_DAP_V2_PACKET_WINDOW
#define CONFIG_WIRELESS_DAP_V2_PACKET_WINDOW 1
#endif

#if CONFIG_WIRELESS_DAP_PROTOCOL_VERSION != WIRELESS_LINK_PROTOCOL_V1 && \
    CONFIG_WIRELESS_DAP_PROTOCOL_VERSION != WIRELESS_LINK_PROTOCOL_V2
#error "CONFIG_WIRELESS_DAP_PROTOCOL_VERSION must be 1 or 2"
#endif

#define LINK_VERSION CONFIG_WIRELESS_DAP_PROTOCOL_VERSION

typedef enum {
    FRAME_HELLO = 1,
    FRAME_HELLO_ACK,
    FRAME_HEARTBEAT,
    FRAME_DAP_REQUEST,
    FRAME_DAP_RESPONSE,
    FRAME_UART_DATA,
    FRAME_UART_CONFIG,
} frame_type_t;

typedef struct {
    uint32_t magic;
    uint32_t pair_id;
    uint16_t sequence;
    uint16_t payload_length;
    uint8_t version;
    uint8_t type;
    uint8_t source_role;
    uint8_t reserved;
    uint32_t crc32;
    uint8_t payload[WIRELESS_LINK_UART_MTU];
} __attribute__((packed)) link_frame_t;

typedef struct {
    uint8_t type;
    uint16_t sequence;
    uint16_t length;
    uint8_t data[WIRELESS_LINK_UART_MTU];
} rx_item_t;

typedef struct {
    bool broadcast;
    uint8_t type;
    uint16_t sequence;
    uint16_t length;
    uint8_t data[WIRELESS_LINK_UART_MTU];
} tx_item_t;

static const char *TAG = "wireless_link";
static const uint8_t s_broadcast_mac[ESP_NOW_ETH_ALEN] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};

static wireless_link_role_t s_role;
static QueueHandle_t s_dap_tx_queue;
static QueueHandle_t s_control_tx_queue;
static QueueHandle_t s_dap_request_queue;
static QueueHandle_t s_dap_response_queue;
static QueueHandle_t s_uart_queue;
static QueueHandle_t s_uart_config_queue;
static SemaphoreHandle_t s_dap_exchange_mutex;
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static uint8_t s_peer_mac[ESP_NOW_ETH_ALEN];
static volatile bool s_connected;
static volatile int64_t s_last_rx_us;
static uint16_t s_next_sequence;
static wireless_link_stats_t s_stats;

static bool s_dap_inflight;
static uint16_t s_dap_inflight_sequence;
static bool s_dap_cache_valid;
static uint16_t s_dap_cache_sequence;
static uint8_t s_dap_cache_response[WIRELESS_LINK_DAP_PACKET_SIZE];
#if CONFIG_WIRELESS_DAP_PROTOCOL_VERSION == WIRELESS_LINK_PROTOCOL_V2
static wireless_link_capabilities_t s_peer_capabilities;
static bool s_peer_capabilities_valid;

static wireless_link_capabilities_t local_capabilities(void)
{
    return (wireless_link_capabilities_t) {
        .max_dap_packet_size = WIRELESS_LINK_DAP_PACKET_SIZE,
        .max_payload_size = WIRELESS_LINK_UART_MTU,
        .packet_window = CONFIG_WIRELESS_DAP_V2_PACKET_WINDOW,
        .flags = WIRELESS_LINK_CAP_DAP64,
        .reserved = 0,
    };
}

static bool capabilities_compatible(const wireless_link_capabilities_t *peer)
{
    if (peer == NULL) {
        return false;
    }
    return peer->max_dap_packet_size >= WIRELESS_LINK_DAP_PACKET_SIZE &&
           peer->max_payload_size >= WIRELESS_LINK_DAP_PACKET_SIZE &&
           peer->packet_window >= 1U;
}
#endif

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t length)
{
    crc = ~crc;
    while (length-- != 0U) {
        crc ^= *data++;
        for (uint8_t bit = 0; bit < 8U; ++bit) {
            crc = (crc >> 1) ^ (0xedb88320UL & (uint32_t)-(int32_t)(crc & 1U));
        }
    }
    return ~crc;
}

static uint32_t frame_crc32(const link_frame_t *frame)
{
    link_frame_t header = *frame;
    header.crc32 = 0;
    return crc32_update(0, (const uint8_t *)&header,
                        offsetof(link_frame_t, payload) + frame->payload_length);
}

static uint16_t next_sequence(void)
{
    uint16_t sequence;
    portENTER_CRITICAL(&s_state_lock);
    sequence = ++s_next_sequence;
    portEXIT_CRITICAL(&s_state_lock);
    return sequence;
}

static bool peer_matches(const uint8_t *mac)
{
    bool matches;
    portENTER_CRITICAL(&s_state_lock);
    matches = s_connected && memcmp(mac, s_peer_mac, ESP_NOW_ETH_ALEN) == 0;
    portEXIT_CRITICAL(&s_state_lock);
    return matches;
}

static esp_err_t add_peer(const uint8_t *mac)
{
    if (esp_now_is_peer_exist(mac)) {
        return ESP_OK;
    }

    esp_now_peer_info_t peer = {
        .channel = CONFIG_WIRELESS_DAP_WIFI_CHANNEL,
        .ifidx = WIFI_IF_STA,
        .encrypt = false,
    };
    memcpy(peer.peer_addr, mac, ESP_NOW_ETH_ALEN);
    esp_err_t ret = esp_now_add_peer(&peer);
    if (ret != ESP_OK && ret != ESP_ERR_ESPNOW_EXIST) {
        return ret;
    }

#if CONFIG_WIRELESS_DAP_ESPNOW_PHY_RATE != 0
    bool is_broadcast = true;
    for (size_t i = 0; i < ESP_NOW_ETH_ALEN; ++i) {
        is_broadcast = is_broadcast && mac[i] == 0xffU;
    }
    if (is_broadcast) {
        return ESP_OK;
    }
    esp_now_rate_config_t rate = {
        .phymode = WIFI_PHY_MODE_HT20,
        .rate = WIFI_PHY_RATE_MCS2_LGI,
        .ersu = false,
        .dcm = false,
    };
#if CONFIG_WIRELESS_DAP_ESPNOW_PHY_RATE == 1
    rate.phymode = WIFI_PHY_MODE_11B;
    rate.rate = WIFI_PHY_RATE_11M_L;
#elif CONFIG_WIRELESS_DAP_ESPNOW_PHY_RATE == 3
    rate.rate = WIFI_PHY_RATE_MCS3_LGI;
#endif
    ret = esp_now_set_peer_rate_config(mac, &rate);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "set peer PHY rate failed: %s", esp_err_to_name(ret));
    }
#endif
    return ESP_OK;
}

static bool set_connected_peer(const uint8_t *mac)
{
    bool accept;
    portENTER_CRITICAL(&s_state_lock);
    accept = !s_connected || memcmp(mac, s_peer_mac, ESP_NOW_ETH_ALEN) == 0;
    portEXIT_CRITICAL(&s_state_lock);
    if (!accept || add_peer(mac) != ESP_OK) {
        return false;
    }

    bool changed = false;
    portENTER_CRITICAL(&s_state_lock);
    if (!s_connected || memcmp(mac, s_peer_mac, ESP_NOW_ETH_ALEN) != 0) {
        memcpy(s_peer_mac, mac, ESP_NOW_ETH_ALEN);
        changed = true;
    }
    s_connected = true;
    s_last_rx_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_state_lock);

    if (changed) {
        ESP_LOGI(TAG, "paired with %02x:%02x:%02x:%02x:%02x:%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    return true;
}

static esp_err_t queue_tx(bool broadcast, frame_type_t type, uint16_t sequence,
                          const void *data, size_t length, TickType_t timeout)
{
    if (length > WIRELESS_LINK_UART_MTU || (length != 0U && data == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!broadcast && !wireless_link_is_connected()) {
        return ESP_ERR_INVALID_STATE;
    }

    tx_item_t item = {
        .broadcast = broadcast,
        .type = (uint8_t)type,
        .sequence = sequence,
        .length = (uint16_t)length,
    };
    if (length != 0U) {
        memcpy(item.data, data, length);
    }
    QueueHandle_t queue = type == FRAME_DAP_REQUEST || type == FRAME_DAP_RESPONSE
                              ? s_dap_tx_queue
                              : s_control_tx_queue;
    if (xQueueSend(queue, &item, timeout) == pdTRUE) {
        return ESP_OK;
    }
    portENTER_CRITICAL(&s_state_lock);
    s_stats.tx_dropped++;
    if (type == FRAME_UART_DATA || type == FRAME_UART_CONFIG) {
        s_stats.uart_dropped++;
    }
    portEXIT_CRITICAL(&s_state_lock);
    return ESP_ERR_TIMEOUT;
}

static void handle_dap_request(const link_frame_t *frame)
{
    if (s_role != WIRELESS_LINK_ROLE_RECEIVER ||
        frame->payload_length != WIRELESS_LINK_DAP_PACKET_SIZE) {
        return;
    }

    bool duplicate_inflight;
    bool duplicate_cached;
    uint8_t cached_response[WIRELESS_LINK_DAP_PACKET_SIZE];
    portENTER_CRITICAL(&s_state_lock);
    duplicate_inflight = s_dap_inflight && frame->sequence == s_dap_inflight_sequence;
    duplicate_cached = s_dap_cache_valid && frame->sequence == s_dap_cache_sequence;
    if (duplicate_cached) {
        memcpy(cached_response, s_dap_cache_response, sizeof(cached_response));
    }
    if (!duplicate_inflight && !duplicate_cached) {
        s_dap_inflight = true;
        s_dap_inflight_sequence = frame->sequence;
    }
    portEXIT_CRITICAL(&s_state_lock);

    if (duplicate_cached) {
        (void)queue_tx(false, FRAME_DAP_RESPONSE, frame->sequence,
                       cached_response, sizeof(cached_response), 0);
        return;
    }
    if (duplicate_inflight) {
        return;
    }

    rx_item_t item = {
        .type = FRAME_DAP_REQUEST,
        .sequence = frame->sequence,
        .length = frame->payload_length,
    };
    memcpy(item.data, frame->payload, frame->payload_length);
    if (xQueueSend(s_dap_request_queue, &item, 0) != pdTRUE) {
        portENTER_CRITICAL(&s_state_lock);
        s_dap_inflight = false;
        portEXIT_CRITICAL(&s_state_lock);
    }
}

static void handle_rx(const uint8_t *source_mac, const uint8_t *data, int length)
{
    if (source_mac == NULL || data == NULL ||
        length < (int)offsetof(link_frame_t, payload) ||
        length > (int)sizeof(link_frame_t)) {
        return;
    }

    link_frame_t frame = {0};
    memcpy(&frame, data, (size_t)length);
    if (frame.magic != LINK_MAGIC || frame.version != LINK_VERSION ||
        frame.pair_id != CONFIG_WIRELESS_DAP_PAIR_ID ||
        frame.payload_length > WIRELESS_LINK_UART_MTU ||
        (size_t)length != offsetof(link_frame_t, payload) + frame.payload_length ||
        frame.source_role == (uint8_t)s_role ||
        frame.crc32 != frame_crc32(&frame)) {
        return;
    }

    portENTER_CRITICAL(&s_state_lock);
    s_stats.rx_frames++;
    portEXIT_CRITICAL(&s_state_lock);

    if (frame.type == FRAME_HELLO || frame.type == FRAME_HELLO_ACK) {
#if CONFIG_WIRELESS_DAP_PROTOCOL_VERSION == WIRELESS_LINK_PROTOCOL_V2
        wireless_link_capabilities_t peer_capabilities = {0};
        if (frame.payload_length != sizeof(peer_capabilities)) {
            ESP_LOGW(TAG, "protocol v2 peer omitted capabilities");
            return;
        }
        memcpy(&peer_capabilities, frame.payload, sizeof(peer_capabilities));
        if (!capabilities_compatible(&peer_capabilities)) {
            ESP_LOGW(TAG, "incompatible protocol v2 capabilities");
            return;
        }
#else
        if (frame.payload_length != 0U) {
            return;
        }
#endif
        const bool accepted = set_connected_peer(source_mac);
        if (accepted) {
#if CONFIG_WIRELESS_DAP_PROTOCOL_VERSION == WIRELESS_LINK_PROTOCOL_V2
            portENTER_CRITICAL(&s_state_lock);
            s_peer_capabilities = peer_capabilities;
            s_peer_capabilities_valid = true;
            portEXIT_CRITICAL(&s_state_lock);
#endif
        }
        if (accepted && frame.type == FRAME_HELLO) {
#if CONFIG_WIRELESS_DAP_PROTOCOL_VERSION == WIRELESS_LINK_PROTOCOL_V2
            const wireless_link_capabilities_t local = local_capabilities();
            (void)queue_tx(false, FRAME_HELLO_ACK, 0, &local, sizeof(local), 0);
#else
            (void)queue_tx(false, FRAME_HELLO_ACK, 0, NULL, 0, 0);
#endif
        }
        return;
    }
    if (!peer_matches(source_mac)) {
        return;
    }

    portENTER_CRITICAL(&s_state_lock);
    s_last_rx_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_state_lock);
    if (frame.type == FRAME_HEARTBEAT) {
        return;
    }
    if (frame.type == FRAME_DAP_REQUEST) {
        handle_dap_request(&frame);
        return;
    }

    rx_item_t item = {
        .type = frame.type,
        .sequence = frame.sequence,
        .length = frame.payload_length,
    };
    if (frame.payload_length != 0U) {
        memcpy(item.data, frame.payload, frame.payload_length);
    }

    switch (frame.type) {
    case FRAME_DAP_RESPONSE:
        if (s_role == WIRELESS_LINK_ROLE_TRANSMITTER &&
            frame.payload_length == WIRELESS_LINK_DAP_PACKET_SIZE) {
            (void)xQueueSend(s_dap_response_queue, &item, 0);
        }
        break;
    case FRAME_UART_DATA:
        if (frame.payload_length != 0U) {
            (void)xQueueSend(s_uart_queue, &item, 0);
        }
        break;
    case FRAME_UART_CONFIG:
        if (s_role == WIRELESS_LINK_ROLE_RECEIVER &&
            frame.payload_length == sizeof(wireless_uart_config_t)) {
            (void)xQueueOverwrite(s_uart_config_queue, &item);
        }
        break;
    default:
        break;
    }
}

static void recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int length)
{
    if (info != NULL && info->rx_ctrl != NULL) {
        portENTER_CRITICAL(&s_state_lock);
        s_stats.last_rssi_dbm = info->rx_ctrl->rssi;
        portEXIT_CRITICAL(&s_state_lock);
    }
    handle_rx(info == NULL ? NULL : info->src_addr, data, length);
}

static void send_cb(const esp_now_send_info_t *info, esp_now_send_status_t status)
{
    (void)info;
    portENTER_CRITICAL_ISR(&s_state_lock);
    if (status == ESP_NOW_SEND_SUCCESS) {
        s_stats.tx_success++;
    } else {
        s_stats.tx_fail++;
    }
    portEXIT_CRITICAL_ISR(&s_state_lock);
}

static void tx_task(void *argument)
{
    (void)argument;
    tx_item_t item;
    link_frame_t frame;

    while (true) {
        if (xQueueReceive(s_dap_tx_queue, &item, 0) != pdTRUE &&
            xQueueReceive(s_control_tx_queue, &item, pdMS_TO_TICKS(1)) != pdTRUE) {
            continue;
        }

        memset(&frame, 0, sizeof(frame));
        frame.magic = LINK_MAGIC;
        frame.pair_id = CONFIG_WIRELESS_DAP_PAIR_ID;
        frame.sequence = item.sequence;
        frame.payload_length = item.length;
        frame.version = LINK_VERSION;
        frame.type = item.type;
        frame.source_role = (uint8_t)s_role;
        if (item.length != 0U) {
            memcpy(frame.payload, item.data, item.length);
        }
        frame.crc32 = frame_crc32(&frame);

        uint8_t destination[ESP_NOW_ETH_ALEN];
        if (item.broadcast) {
            memcpy(destination, s_broadcast_mac, sizeof(destination));
        } else {
            bool connected;
            portENTER_CRITICAL(&s_state_lock);
            connected = s_connected;
            memcpy(destination, s_peer_mac, sizeof(destination));
            portEXIT_CRITICAL(&s_state_lock);
            if (!connected) {
                continue;
            }
        }

        const size_t frame_length = offsetof(link_frame_t, payload) + item.length;
        esp_err_t ret = ESP_FAIL;
        TickType_t busy_backoff = 0;
        for (uint8_t attempt = 0; attempt < 4U; ++attempt) {
            ret = esp_now_send(destination, (const uint8_t *)&frame, frame_length);
            if (ret != ESP_ERR_ESPNOW_NO_MEM) {
                break;
            }
            portENTER_CRITICAL(&s_state_lock);
            s_stats.tx_busy++;
            portEXIT_CRITICAL(&s_state_lock);
            busy_backoff = pdMS_TO_TICKS(1U << attempt);
            vTaskDelay(busy_backoff);
        }
        if (ret != ESP_OK && ret != ESP_ERR_ESPNOW_NOT_FOUND) {
            ESP_LOGD(TAG, "send type %u failed: %s", item.type, esp_err_to_name(ret));
        }
#if CONFIG_WIRELESS_DAP_TX_DELAY_MS > 0
        if (ret == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(CONFIG_WIRELESS_DAP_TX_DELAY_MS));
        }
#endif
    }
}

static void supervision_task(void *argument)
{
    (void)argument;
    TickType_t last_hello = 0;
    TickType_t last_heartbeat = 0;
#if CONFIG_WIRELESS_DAP_DIAGNOSTICS
    TickType_t last_stats = 0;
#endif

    while (true) {
        TickType_t now = xTaskGetTickCount();
#if CONFIG_WIRELESS_DAP_DIAGNOSTICS
        if ((now - last_stats) >= pdMS_TO_TICKS(5000)) {
            wireless_link_stats_t stats;
            wireless_link_get_stats(&stats);
            ESP_LOGI(TAG, "stats tx_ok=%" PRIu32 " tx_fail=%" PRIu32
                          " busy=%" PRIu32 " drop=%" PRIu32
                          " rx=%" PRIu32 " dap=%" PRIu32 " retry=%" PRIu32
                          " timeout=%" PRIu32 " last_dap_us=%" PRIu32
                          " rssi=%d dBm",
                     stats.tx_success, stats.tx_fail, stats.tx_busy,
                     stats.tx_dropped, stats.rx_frames, stats.dap_requests,
                     stats.dap_retries, stats.dap_timeouts,
                     stats.last_dap_latency_us, stats.last_rssi_dbm);
            last_stats = now;
        }
#endif
        if (!wireless_link_is_connected()) {
            if ((now - last_hello) >= pdMS_TO_TICKS(LINK_HELLO_PERIOD_MS)) {
#if CONFIG_WIRELESS_DAP_PROTOCOL_VERSION == WIRELESS_LINK_PROTOCOL_V2
                const wireless_link_capabilities_t local = local_capabilities();
                (void)queue_tx(true, FRAME_HELLO, 0, &local, sizeof(local), 0);
#else
                (void)queue_tx(true, FRAME_HELLO, 0, NULL, 0, 0);
#endif
                last_hello = now;
            }
        } else {
            if ((now - last_heartbeat) >= pdMS_TO_TICKS(LINK_HEARTBEAT_PERIOD_MS)) {
                (void)queue_tx(false, FRAME_HEARTBEAT, 0, NULL, 0, 0);
                last_heartbeat = now;
            }

            int64_t last_rx_us;
            portENTER_CRITICAL(&s_state_lock);
            last_rx_us = s_last_rx_us;
            portEXIT_CRITICAL(&s_state_lock);
            if ((esp_timer_get_time() - last_rx_us) >
                ((int64_t)LINK_LOST_TIMEOUT_MS * 1000LL)) {
                uint8_t old_peer[ESP_NOW_ETH_ALEN];
                portENTER_CRITICAL(&s_state_lock);
                memcpy(old_peer, s_peer_mac, sizeof(old_peer));
                memset(s_peer_mac, 0, sizeof(s_peer_mac));
                s_connected = false;
                s_dap_inflight = false;
                s_dap_cache_valid = false;
#if CONFIG_WIRELESS_DAP_PROTOCOL_VERSION == WIRELESS_LINK_PROTOCOL_V2
                s_peer_capabilities_valid = false;
                memset(&s_peer_capabilities, 0, sizeof(s_peer_capabilities));
#endif
                portEXIT_CRITICAL(&s_state_lock);
                xQueueReset(s_dap_tx_queue);
                xQueueReset(s_control_tx_queue);
                xQueueReset(s_dap_request_queue);
                xQueueReset(s_dap_response_queue);
                xQueueReset(s_uart_queue);
                xQueueReset(s_uart_config_queue);
                (void)esp_now_del_peer(old_peer);
                ESP_LOGW(TAG, "peer timed out; discovery resumed");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

esp_err_t wireless_link_init(wireless_link_role_t role)
{
    ESP_RETURN_ON_FALSE(role == WIRELESS_LINK_ROLE_TRANSMITTER ||
                            role == WIRELESS_LINK_ROLE_RECEIVER,
                        ESP_ERR_INVALID_ARG, TAG, "invalid role");
    s_role = role;

    s_dap_tx_queue = xQueueCreate(8, sizeof(tx_item_t));
    s_control_tx_queue = xQueueCreate(LINK_TX_QUEUE_DEPTH, sizeof(tx_item_t));
    s_dap_request_queue = xQueueCreate(4, sizeof(rx_item_t));
    s_dap_response_queue = xQueueCreate(4, sizeof(rx_item_t));
    s_uart_queue = xQueueCreate(LINK_QUEUE_DEPTH, sizeof(rx_item_t));
    s_uart_config_queue = xQueueCreate(1, sizeof(rx_item_t));
    s_dap_exchange_mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_dap_tx_queue && s_control_tx_queue && s_dap_request_queue && s_dap_response_queue &&
                            s_uart_queue && s_uart_config_queue && s_dap_exchange_mutex,
                        ESP_ERR_NO_MEM, TAG, "queue allocation failed");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "NVS erase failed");
        ret = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "NVS init failed");
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init failed");
    ret = esp_event_loop_create_default();
    ESP_RETURN_ON_FALSE(ret == ESP_OK || ret == ESP_ERR_INVALID_STATE,
                        ret, TAG, "event loop init failed");

    wifi_init_config_t wifi_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&wifi_config), TAG, "Wi-Fi init failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "Wi-Fi storage failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "Wi-Fi mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "Wi-Fi start failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG, "disable power save failed");
    ESP_RETURN_ON_ERROR(
        esp_wifi_set_channel(CONFIG_WIRELESS_DAP_WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE),
        TAG, "set channel failed");

    ESP_RETURN_ON_ERROR(esp_now_init(), TAG, "ESP-NOW init failed");
    ESP_RETURN_ON_ERROR(add_peer(s_broadcast_mac), TAG, "add broadcast peer failed");
    ESP_RETURN_ON_ERROR(esp_now_register_recv_cb(recv_cb), TAG, "register receive callback failed");
    ESP_RETURN_ON_ERROR(esp_now_register_send_cb(send_cb), TAG, "register send callback failed");

    uint8_t local_mac[ESP_NOW_ETH_ALEN];
    ESP_RETURN_ON_ERROR(esp_wifi_get_mac(WIFI_IF_STA, local_mac), TAG, "read MAC failed");
    ESP_LOGI(TAG,
             "role=%s mac=%02x:%02x:%02x:%02x:%02x:%02x channel=%d pair=0x%08" PRIx32,
             role == WIRELESS_LINK_ROLE_TRANSMITTER ? "transmitter" : "receiver",
             local_mac[0], local_mac[1], local_mac[2],
             local_mac[3], local_mac[4], local_mac[5],
             CONFIG_WIRELESS_DAP_WIFI_CHANNEL, (uint32_t)CONFIG_WIRELESS_DAP_PAIR_ID);

    ESP_RETURN_ON_FALSE(
        xTaskCreate(tx_task, "wireless_tx", 4096, NULL, 12, NULL) == pdPASS,
        ESP_ERR_NO_MEM, TAG, "create TX task failed");
    ESP_RETURN_ON_FALSE(
        xTaskCreate(supervision_task, "wireless_link", 4096, NULL, 5, NULL) == pdPASS,
        ESP_ERR_NO_MEM, TAG, "create link task failed");
    return ESP_OK;
}

bool wireless_link_is_connected(void)
{
    bool connected;
    portENTER_CRITICAL(&s_state_lock);
#if CONFIG_WIRELESS_DAP_PROTOCOL_VERSION == WIRELESS_LINK_PROTOCOL_V2
    connected = s_connected && s_peer_capabilities_valid;
#else
    connected = s_connected;
#endif
    portEXIT_CRITICAL(&s_state_lock);
    return connected;
}

void wireless_link_get_peer_mac(uint8_t mac[6])
{
    if (mac == NULL) {
        return;
    }
    portENTER_CRITICAL(&s_state_lock);
    memcpy(mac, s_peer_mac, ESP_NOW_ETH_ALEN);
    portEXIT_CRITICAL(&s_state_lock);
}

bool wireless_link_get_peer_capabilities(wireless_link_capabilities_t *capabilities)
{
    if (capabilities == NULL) {
        return false;
    }
#if CONFIG_WIRELESS_DAP_PROTOCOL_VERSION == WIRELESS_LINK_PROTOCOL_V2
    bool valid;
    portENTER_CRITICAL(&s_state_lock);
    valid = s_peer_capabilities_valid;
    if (valid) {
        *capabilities = s_peer_capabilities;
    }
    portEXIT_CRITICAL(&s_state_lock);
    return valid;
#else
    (void)capabilities;
    return false;
#endif
}

esp_err_t wireless_link_dap_exchange(const uint8_t request[WIRELESS_LINK_DAP_PACKET_SIZE],
                                     uint8_t response[WIRELESS_LINK_DAP_PACKET_SIZE],
                                     TickType_t timeout)
{
    if (request == NULL || response == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_dap_exchange_mutex, timeout) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    xQueueReset(s_dap_response_queue);
    const uint16_t sequence = next_sequence();
    const int64_t start_us = esp_timer_get_time();
    uint32_t retries = 0;
    portENTER_CRITICAL(&s_state_lock);
    s_stats.dap_requests++;
    portEXIT_CRITICAL(&s_state_lock);
    esp_err_t result = ESP_ERR_TIMEOUT;
    rx_item_t item;

    const TickType_t start_tick = xTaskGetTickCount();
    while ((xTaskGetTickCount() - start_tick) < timeout) {
        if (!wireless_link_is_connected()) {
            result = ESP_ERR_INVALID_STATE;
            break;
        }
        if (queue_tx(false, FRAME_DAP_REQUEST, sequence, request,
                     WIRELESS_LINK_DAP_PACKET_SIZE, pdMS_TO_TICKS(10)) != ESP_OK) {
            result = ESP_ERR_TIMEOUT;
            break;
        }

        if (xQueueReceive(s_dap_response_queue, &item,
                          pdMS_TO_TICKS(CONFIG_WIRELESS_DAP_DAP_RETRY_MS)) == pdTRUE &&
            item.sequence == sequence &&
            item.length == WIRELESS_LINK_DAP_PACKET_SIZE) {
            memcpy(response, item.data, WIRELESS_LINK_DAP_PACKET_SIZE);
            result = ESP_OK;
            break;
        }
        retries++;
    }

    const uint32_t elapsed_us = (uint32_t)(esp_timer_get_time() - start_us);
    portENTER_CRITICAL(&s_state_lock);
    s_stats.dap_retries += retries;
    s_stats.last_dap_latency_us = elapsed_us;
    if (result != ESP_OK) {
        s_stats.dap_timeouts++;
    }
    portEXIT_CRITICAL(&s_state_lock);
#if CONFIG_WIRELESS_DAP_DIAGNOSTICS
    ESP_LOGI(TAG, "dap seq=%u result=%s latency=%" PRIu32 " us retries=%" PRIu32,
             sequence, esp_err_to_name(result), elapsed_us, retries);
#endif

    xSemaphoreGive(s_dap_exchange_mutex);
    return result;
}

void wireless_link_get_stats(wireless_link_stats_t *stats)
{
    if (stats == NULL) {
        return;
    }
    portENTER_CRITICAL(&s_state_lock);
    *stats = s_stats;
    portEXIT_CRITICAL(&s_state_lock);
}

void wireless_link_reset_stats(void)
{
    portENTER_CRITICAL(&s_state_lock);
    memset(&s_stats, 0, sizeof(s_stats));
    portEXIT_CRITICAL(&s_state_lock);
}

esp_err_t wireless_link_dap_receive(uint8_t request[WIRELESS_LINK_DAP_PACKET_SIZE],
                                    uint16_t *sequence, TickType_t timeout)
{
    if (request == NULL || sequence == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    rx_item_t item;
    if (xQueueReceive(s_dap_request_queue, &item, timeout) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    memcpy(request, item.data, WIRELESS_LINK_DAP_PACKET_SIZE);
    *sequence = item.sequence;
    return ESP_OK;
}

esp_err_t wireless_link_dap_reply(uint16_t sequence,
                                  const uint8_t response[WIRELESS_LINK_DAP_PACKET_SIZE])
{
    if (response == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    bool valid;
    portENTER_CRITICAL(&s_state_lock);
    valid = s_connected && s_dap_inflight && sequence == s_dap_inflight_sequence;
    if (valid) {
        memcpy(s_dap_cache_response, response, WIRELESS_LINK_DAP_PACKET_SIZE);
        s_dap_cache_sequence = sequence;
        s_dap_cache_valid = true;
        s_dap_inflight = false;
    }
    portEXIT_CRITICAL(&s_state_lock);
    if (!valid) {
        return ESP_ERR_INVALID_STATE;
    }
    return queue_tx(false, FRAME_DAP_RESPONSE, sequence, response,
                    WIRELESS_LINK_DAP_PACKET_SIZE, pdMS_TO_TICKS(20));
}

esp_err_t wireless_link_uart_send(const uint8_t *data, size_t length)
{
    if (data == NULL || length == 0U || length > WIRELESS_LINK_UART_MTU) {
        return ESP_ERR_INVALID_ARG;
    }
    return queue_tx(false, FRAME_UART_DATA, next_sequence(), data, length,
                    pdMS_TO_TICKS(10));
}

esp_err_t wireless_link_uart_receive(uint8_t *data, size_t capacity,
                                     size_t *length, TickType_t timeout)
{
    if (data == NULL || length == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    rx_item_t item;
    if (xQueueReceive(s_uart_queue, &item, timeout) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (item.length > capacity) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(data, item.data, item.length);
    *length = item.length;
    return ESP_OK;
}

esp_err_t wireless_link_uart_send_config(const wireless_uart_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = ESP_OK;
    const uint16_t sequence = next_sequence();
    for (uint8_t repeat = 0; repeat < 3U; ++repeat) {
        ret = queue_tx(false, FRAME_UART_CONFIG, sequence, config, sizeof(*config),
                       pdMS_TO_TICKS(10));
        if (ret != ESP_OK) {
            return ret;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return ret;
}

esp_err_t wireless_link_uart_receive_config(wireless_uart_config_t *config,
                                            TickType_t timeout)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    rx_item_t item;
    if (xQueueReceive(s_uart_config_queue, &item, timeout) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    memcpy(config, item.data, sizeof(*config));
    return ESP_OK;
}
