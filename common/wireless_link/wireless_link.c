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
#define LINK_FRAME_HEADER_SIZE 20U
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
#ifndef CONFIG_WIRELESS_DAP_USB_BULK_V2
#define CONFIG_WIRELESS_DAP_USB_BULK_V2 0
#endif
#ifndef CONFIG_WIRELESS_DAP_HID_AGGREGATION
#define CONFIG_WIRELESS_DAP_HID_AGGREGATION 0
#endif

#define DAP_WINDOW_MAX WIRELESS_LINK_DAP_WINDOW_MAX
#define DAP_WINDOW_COUNT(meta) ((((meta) >> 4) & 0x0fU) + 1U)
#define DAP_WINDOW_INDEX(meta) ((meta) & 0x0fU)
#define DAP_WINDOW_META(count, index) \
    (uint8_t)((((count) - 1U) << 4) | (index))

#if CONFIG_WIRELESS_DAP_PROTOCOL_VERSION != WIRELESS_LINK_PROTOCOL_V1 && \
    CONFIG_WIRELESS_DAP_PROTOCOL_VERSION != WIRELESS_LINK_PROTOCOL_V2
#error "CONFIG_WIRELESS_DAP_PROTOCOL_VERSION must be 1 or 2"
#endif
#if WIRELESS_LINK_DAP_PACKET_SIZE != 64U && WIRELESS_LINK_DAP_PACKET_SIZE != 256U
#error "WIRELESS_LINK_DAP_PACKET_SIZE must be 64 or 256"
#endif
#if CONFIG_WIRELESS_DAP_PROTOCOL_VERSION == WIRELESS_LINK_PROTOCOL_V1 && \
    WIRELESS_LINK_DAP_PACKET_SIZE != 64U
#error "protocol v1 only supports 64-byte DAP packets"
#endif
#if CONFIG_WIRELESS_DAP_V2_PACKET_WINDOW < 1 || \
    CONFIG_WIRELESS_DAP_V2_PACKET_WINDOW > DAP_WINDOW_MAX
#error "CONFIG_WIRELESS_DAP_V2_PACKET_WINDOW must be 1 or 2"
#endif
#if CONFIG_WIRELESS_DAP_PROTOCOL_VERSION == WIRELESS_LINK_PROTOCOL_V1 && \
    CONFIG_WIRELESS_DAP_V2_PACKET_WINDOW != 1
#error "protocol v1 only supports packet window 1"
#endif
#if CONFIG_WIRELESS_DAP_HID_AGGREGATION && \
    CONFIG_WIRELESS_DAP_V2_PACKET_WINDOW != 1
#error "HID aggregation and packet window cannot be enabled together"
#endif

#define LINK_VERSION CONFIG_WIRELESS_DAP_PROTOCOL_VERSION

#if CONFIG_WIRELESS_DAP_PROTOCOL_VERSION == WIRELESS_LINK_PROTOCOL_V2
/* One byte beyond the v1 limit forces discovery onto an ESP-NOW v2 frame. */
#define LINK_DISCOVERY_PAYLOAD_SIZE \
    (ESP_NOW_MAX_DATA_LEN + 1U - LINK_FRAME_HEADER_SIZE)
#if WIRELESS_LINK_DAP_PACKET_SIZE > LINK_DISCOVERY_PAYLOAD_SIZE
#define LINK_FRAME_PAYLOAD_SIZE WIRELESS_LINK_DAP_PACKET_SIZE
#else
#define LINK_FRAME_PAYLOAD_SIZE LINK_DISCOVERY_PAYLOAD_SIZE
#endif
#else
#define LINK_FRAME_PAYLOAD_SIZE WIRELESS_LINK_UART_MTU
#endif

#if WIRELESS_LINK_DAP_PACKET_SIZE > WIRELESS_LINK_UART_MTU
#define LINK_APP_PAYLOAD_SIZE WIRELESS_LINK_DAP_PACKET_SIZE
#else
#define LINK_APP_PAYLOAD_SIZE WIRELESS_LINK_UART_MTU
#endif

typedef enum {
    FRAME_HELLO = 1,
    FRAME_HELLO_ACK,
    FRAME_HEARTBEAT,
    FRAME_DAP_REQUEST,
    FRAME_DAP_RESPONSE,
    FRAME_UART_DATA,
    FRAME_UART_CONFIG,
    FRAME_DAP_ABORT,
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
    uint8_t payload[LINK_FRAME_PAYLOAD_SIZE];
} __attribute__((packed)) link_frame_t;

_Static_assert(offsetof(link_frame_t, payload) == LINK_FRAME_HEADER_SIZE,
               "wireless frame header size changed");
_Static_assert(sizeof(link_frame_t) <= ESP_NOW_MAX_DATA_LEN_V2,
               "wireless frame exceeds ESP-NOW v2 limit");
#if CONFIG_WIRELESS_DAP_PROTOCOL_VERSION == WIRELESS_LINK_PROTOCOL_V2
_Static_assert(sizeof(link_frame_t) > ESP_NOW_MAX_DATA_LEN,
               "protocol v2 frame must exceed the ESP-NOW v1 limit");
#else
_Static_assert(sizeof(link_frame_t) <= ESP_NOW_MAX_DATA_LEN,
               "protocol v1 frame exceeds the ESP-NOW v1 limit");
#endif

typedef struct {
    uint8_t type;
    uint8_t window_meta;
    uint16_t sequence;
    uint16_t length;
    uint8_t data[LINK_APP_PAYLOAD_SIZE];
} rx_item_t;

typedef struct {
    bool broadcast;
    uint8_t type;
    uint8_t window_meta;
    uint16_t sequence;
    uint16_t length;
    uint8_t data[LINK_APP_PAYLOAD_SIZE];
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
static QueueHandle_t s_dap_abort_queue;
static QueueHandle_t s_uart_queue;
static QueueHandle_t s_uart_config_queue;
static SemaphoreHandle_t s_dap_exchange_mutex;
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static uint8_t s_peer_mac[ESP_NOW_ETH_ALEN];
static volatile bool s_connected;
static volatile int64_t s_last_rx_us;
static uint16_t s_next_sequence;
static wireless_link_stats_t s_stats;

typedef enum {
    DAP_SLOT_EMPTY,
    DAP_SLOT_RECEIVED,
    DAP_SLOT_QUEUED,
    DAP_SLOT_EXECUTING,
    DAP_SLOT_COMPLETED,
} dap_slot_state_t;

typedef struct {
    dap_slot_state_t state;
    uint8_t window_meta;
    uint16_t sequence;
    uint16_t length;
    uint8_t request[WIRELESS_LINK_DAP_PACKET_SIZE];
    uint8_t response[WIRELESS_LINK_DAP_PACKET_SIZE];
} dap_slot_t;

static bool s_dap_window_active;
static bool s_dap_window_cancelled;
static uint16_t s_dap_window_base_sequence;
static uint8_t s_dap_window_count;
static dap_slot_t s_dap_slots[DAP_WINDOW_MAX];
static uint32_t s_dap_abort_generation;
#if CONFIG_WIRELESS_DAP_PROTOCOL_VERSION == WIRELESS_LINK_PROTOCOL_V2
static wireless_link_capabilities_t s_peer_capabilities;
static bool s_peer_capabilities_valid;

static wireless_link_capabilities_t local_capabilities(void)
{
    return (wireless_link_capabilities_t) {
        .max_dap_packet_size = WIRELESS_LINK_DAP_PACKET_SIZE,
        .max_payload_size = LINK_FRAME_PAYLOAD_SIZE,
        .packet_window = CONFIG_WIRELESS_DAP_V2_PACKET_WINDOW,
        .flags = WIRELESS_LINK_CAP_DAP64 | WIRELESS_LINK_CAP_ESPNOW_V2 |
#if WIRELESS_LINK_DAP_PACKET_SIZE == 256U
                 WIRELESS_LINK_CAP_DAP256 |
#endif
#if CONFIG_WIRELESS_DAP_USB_BULK_V2
                 WIRELESS_LINK_CAP_BULK_USB |
#endif
#if CONFIG_WIRELESS_DAP_HID_AGGREGATION
                 WIRELESS_LINK_CAP_HID_AGGREGATION |
#endif
#if CONFIG_WIRELESS_DAP_V2_PACKET_WINDOW > 1
                 WIRELESS_LINK_CAP_WINDOW |
#endif
                 0,
        .reserved = 0,
    };
}

static bool capabilities_compatible(const wireless_link_capabilities_t *peer)
{
    if (peer == NULL) {
        return false;
    }
    uint8_t required_flags = WIRELESS_LINK_CAP_DAP64 |
                             WIRELESS_LINK_CAP_ESPNOW_V2;
#if WIRELESS_LINK_DAP_PACKET_SIZE == 256U
    required_flags |= WIRELESS_LINK_CAP_DAP256;
#endif
#if CONFIG_WIRELESS_DAP_HID_AGGREGATION
    if (s_role == WIRELESS_LINK_ROLE_TRANSMITTER) {
        required_flags |= WIRELESS_LINK_CAP_HID_AGGREGATION;
    }
#endif
#if CONFIG_WIRELESS_DAP_V2_PACKET_WINDOW > 1
    if (s_role == WIRELESS_LINK_ROLE_TRANSMITTER) {
        required_flags |= WIRELESS_LINK_CAP_WINDOW;
    }
#endif
    const uint8_t required_window =
        s_role == WIRELESS_LINK_ROLE_TRANSMITTER
            ? CONFIG_WIRELESS_DAP_V2_PACKET_WINDOW
            : 1U;
    return peer->max_dap_packet_size == WIRELESS_LINK_DAP_PACKET_SIZE &&
           peer->max_payload_size >= LINK_FRAME_PAYLOAD_SIZE &&
           peer->packet_window >= required_window &&
           (peer->flags & required_flags) == required_flags;
}
#endif

static bool valid_dap_packet_length(size_t length)
{
    return length == WIRELESS_LINK_DAP_PACKET_SIZE_HID ||
           length == WIRELESS_LINK_DAP_PACKET_SIZE;
}

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

static uint16_t reserve_sequences(size_t count)
{
    uint16_t first;
    portENTER_CRITICAL(&s_state_lock);
    first = (uint16_t)(s_next_sequence + 1U);
    s_next_sequence = (uint16_t)(s_next_sequence + count);
    portEXIT_CRITICAL(&s_state_lock);
    return first;
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

static esp_err_t queue_tx_window(bool broadcast, frame_type_t type,
                                 uint16_t sequence, uint8_t window_meta,
                                 const void *data, size_t length,
                                 TickType_t timeout)
{
    if (length > LINK_APP_PAYLOAD_SIZE || (length != 0U && data == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!broadcast && !wireless_link_is_connected()) {
        return ESP_ERR_INVALID_STATE;
    }

    tx_item_t item = {
        .broadcast = broadcast,
        .type = (uint8_t)type,
        .window_meta = window_meta,
        .sequence = sequence,
        .length = (uint16_t)length,
    };
    if (length != 0U) {
        memcpy(item.data, data, length);
    }
    QueueHandle_t queue = type == FRAME_DAP_REQUEST ||
                                  type == FRAME_DAP_RESPONSE ||
                                  type == FRAME_DAP_ABORT
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

static esp_err_t queue_tx(bool broadcast, frame_type_t type, uint16_t sequence,
                          const void *data, size_t length, TickType_t timeout)
{
    return queue_tx_window(broadcast, type, sequence, 0U, data, length,
                           timeout);
}

static bool dap_window_completed_locked(void)
{
    if (!s_dap_window_active) {
        return true;
    }
    for (uint8_t i = 0; i < s_dap_window_count; ++i) {
        if (s_dap_slots[i].state != DAP_SLOT_COMPLETED) {
            return false;
        }
    }
    return true;
}

static bool dap_window_executing_locked(void)
{
    for (uint8_t i = 0; i < s_dap_window_count; ++i) {
        if (s_dap_slots[i].state == DAP_SLOT_EXECUTING) {
            return true;
        }
    }
    return false;
}

static void reset_dap_window_locked(void)
{
    s_dap_window_active = false;
    s_dap_window_cancelled = false;
    s_dap_window_base_sequence = 0U;
    s_dap_window_count = 0U;
    memset(s_dap_slots, 0, sizeof(s_dap_slots));
}

static bool valid_dap_window_meta(uint8_t meta, uint8_t *count,
                                  uint8_t *index)
{
    *count = DAP_WINDOW_COUNT(meta);
    *index = DAP_WINDOW_INDEX(meta);
#if CONFIG_WIRELESS_DAP_PROTOCOL_VERSION == WIRELESS_LINK_PROTOCOL_V1
    return meta == 0U;
#else
    return *count <= CONFIG_WIRELESS_DAP_V2_PACKET_WINDOW &&
           *count <= DAP_WINDOW_MAX && *index < *count;
#endif
}

static void try_enqueue_next_dap_slot(void)
{
    rx_item_t item = {0};
    dap_slot_t *selected = NULL;

    portENTER_CRITICAL(&s_state_lock);
    if (s_dap_window_active && !s_dap_window_cancelled) {
        for (uint8_t i = 0; i < s_dap_window_count; ++i) {
            dap_slot_t *slot = &s_dap_slots[i];
            if (slot->state == DAP_SLOT_COMPLETED) {
                continue;
            }
            if (slot->state == DAP_SLOT_RECEIVED) {
                slot->state = DAP_SLOT_QUEUED;
                selected = slot;
                item.type = FRAME_DAP_REQUEST;
                item.window_meta = slot->window_meta;
                item.sequence = slot->sequence;
                item.length = slot->length;
                memcpy(item.data, slot->request, slot->length);
            }
            break;
        }
    }
    portEXIT_CRITICAL(&s_state_lock);

    if (selected == NULL) {
        return;
    }
    if (xQueueSend(s_dap_request_queue, &item, 0) == pdTRUE) {
        return;
    }

    portENTER_CRITICAL(&s_state_lock);
    if (selected->state == DAP_SLOT_QUEUED &&
        selected->sequence == item.sequence) {
        selected->state = DAP_SLOT_RECEIVED;
    }
    portEXIT_CRITICAL(&s_state_lock);
}

static void handle_dap_request(const link_frame_t *frame)
{
    uint8_t count;
    uint8_t index;
    if (s_role != WIRELESS_LINK_ROLE_RECEIVER ||
        !valid_dap_packet_length(frame->payload_length) ||
        !valid_dap_window_meta(frame->reserved, &count, &index)) {
        return;
    }

    const uint16_t base_sequence = (uint16_t)(frame->sequence - index);
    bool cached = false;
    bool accepted = false;
    uint8_t cached_response[WIRELESS_LINK_DAP_PACKET_SIZE];
    uint16_t cached_length = 0U;

    portENTER_CRITICAL(&s_state_lock);
    const bool same_window = s_dap_window_active &&
                             s_dap_window_base_sequence == base_sequence &&
                             s_dap_window_count == count;
    if (!same_window &&
        (!s_dap_window_active || dap_window_completed_locked() ||
         (s_dap_window_cancelled && !dap_window_executing_locked()))) {
        reset_dap_window_locked();
        s_dap_window_active = true;
        s_dap_window_base_sequence = base_sequence;
        s_dap_window_count = count;
    }

    if (s_dap_window_active &&
        s_dap_window_base_sequence == base_sequence &&
        s_dap_window_count == count) {
        dap_slot_t *slot = &s_dap_slots[index];
        if (!s_dap_window_cancelled && slot->state == DAP_SLOT_EMPTY) {
            slot->state = DAP_SLOT_RECEIVED;
            slot->window_meta = frame->reserved;
            slot->sequence = frame->sequence;
            slot->length = frame->payload_length;
            memcpy(slot->request, frame->payload, frame->payload_length);
            accepted = true;
        } else if (slot->sequence == frame->sequence &&
                   slot->length == frame->payload_length &&
                   slot->window_meta == frame->reserved &&
                   memcmp(slot->request, frame->payload,
                          frame->payload_length) == 0) {
            if (slot->state == DAP_SLOT_COMPLETED) {
                accepted = true;
                cached = true;
                cached_length = slot->length;
                memcpy(cached_response, slot->response, slot->length);
            } else if (!s_dap_window_cancelled ||
                       slot->state == DAP_SLOT_EXECUTING) {
                accepted = true;
            }
        }
    }
    portEXIT_CRITICAL(&s_state_lock);

    if (!accepted) {
        return;
    }
    if (cached) {
        (void)queue_tx_window(false, FRAME_DAP_RESPONSE, frame->sequence,
                              frame->reserved, cached_response, cached_length, 0);
        return;
    }
    try_enqueue_next_dap_slot();
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
        frame.payload_length > LINK_FRAME_PAYLOAD_SIZE ||
        (size_t)length != offsetof(link_frame_t, payload) + frame.payload_length ||
        frame.source_role == (uint8_t)s_role ||
        frame.crc32 != frame_crc32(&frame)) {
        return;
    }
#if CONFIG_WIRELESS_DAP_PROTOCOL_VERSION == WIRELESS_LINK_PROTOCOL_V1
    if (frame.reserved != 0U) {
        return;
    }
#endif
    if (frame.type != FRAME_DAP_REQUEST &&
        frame.type != FRAME_DAP_RESPONSE && frame.reserved != 0U) {
        return;
    }

    portENTER_CRITICAL(&s_state_lock);
    s_stats.rx_frames++;
    portEXIT_CRITICAL(&s_state_lock);

    if (frame.type == FRAME_HELLO || frame.type == FRAME_HELLO_ACK) {
#if CONFIG_WIRELESS_DAP_PROTOCOL_VERSION == WIRELESS_LINK_PROTOCOL_V2
        wireless_link_capabilities_t peer_capabilities = {0};
        if (frame.payload_length != LINK_DISCOVERY_PAYLOAD_SIZE) {
            ESP_LOGW(TAG, "protocol v2 peer did not use ESP-NOW v2 discovery");
            return;
        }
        memcpy(&peer_capabilities, frame.payload, sizeof(peer_capabilities));
        for (size_t i = sizeof(peer_capabilities); i < frame.payload_length; ++i) {
            if (frame.payload[i] != 0U) {
                ESP_LOGW(TAG, "protocol v2 discovery padding is not zero");
                return;
            }
        }
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

    switch (frame.type) {
    case FRAME_DAP_ABORT: {
        if (s_role != WIRELESS_LINK_ROLE_RECEIVER || frame.payload_length != 0U) {
            break;
        }
        portENTER_CRITICAL(&s_state_lock);
        s_dap_window_cancelled = true;
        portEXIT_CRITICAL(&s_state_lock);
        xQueueReset(s_dap_request_queue);
        const uint8_t signal = 1U;
        (void)xQueueOverwrite(s_dap_abort_queue, &signal);
        break;
    }
    case FRAME_DAP_RESPONSE: {
        if (s_role != WIRELESS_LINK_ROLE_TRANSMITTER ||
            !valid_dap_packet_length(frame.payload_length)) {
            break;
        }
        rx_item_t item = {
            .type = frame.type,
            .window_meta = frame.reserved,
            .sequence = frame.sequence,
            .length = frame.payload_length,
        };
        memcpy(item.data, frame.payload, frame.payload_length);
        (void)xQueueSend(s_dap_response_queue, &item, 0);
        break;
    }
    case FRAME_UART_DATA: {
        if (frame.payload_length == 0U ||
            frame.payload_length > WIRELESS_LINK_UART_MTU) {
            break;
        }
        rx_item_t item = {
            .type = frame.type,
            .sequence = frame.sequence,
            .length = frame.payload_length,
        };
        memcpy(item.data, frame.payload, frame.payload_length);
        (void)xQueueSend(s_uart_queue, &item, 0);
        break;
    }
    case FRAME_UART_CONFIG: {
        if (s_role != WIRELESS_LINK_ROLE_RECEIVER ||
            frame.payload_length != sizeof(wireless_uart_config_t)) {
            break;
        }
        rx_item_t item = {
            .type = frame.type,
            .sequence = frame.sequence,
            .length = frame.payload_length,
        };
        memcpy(item.data, frame.payload, frame.payload_length);
        (void)xQueueOverwrite(s_uart_config_queue, &item);
        break;
    }
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
        size_t payload_length = item.length;
#if CONFIG_WIRELESS_DAP_PROTOCOL_VERSION == WIRELESS_LINK_PROTOCOL_V2
        if (item.type == FRAME_HELLO || item.type == FRAME_HELLO_ACK) {
            if (item.length != sizeof(wireless_link_capabilities_t)) {
                ESP_LOGE(TAG, "invalid protocol v2 discovery payload");
                continue;
            }
            payload_length = LINK_DISCOVERY_PAYLOAD_SIZE;
        }
#endif
        frame.payload_length = (uint16_t)payload_length;
        frame.version = LINK_VERSION;
        frame.type = item.type;
        frame.source_role = (uint8_t)s_role;
        frame.reserved = item.window_meta;
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

        const size_t frame_length = offsetof(link_frame_t, payload) + payload_length;
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
                reset_dap_window_locked();
#if CONFIG_WIRELESS_DAP_PROTOCOL_VERSION == WIRELESS_LINK_PROTOCOL_V2
                s_peer_capabilities_valid = false;
                memset(&s_peer_capabilities, 0, sizeof(s_peer_capabilities));
#endif
                portEXIT_CRITICAL(&s_state_lock);
                xQueueReset(s_dap_tx_queue);
                xQueueReset(s_control_tx_queue);
                xQueueReset(s_dap_request_queue);
                xQueueReset(s_dap_response_queue);
                xQueueReset(s_dap_abort_queue);
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
    s_dap_abort_queue = xQueueCreate(1, sizeof(uint8_t));
    s_uart_queue = xQueueCreate(LINK_QUEUE_DEPTH, sizeof(rx_item_t));
    s_uart_config_queue = xQueueCreate(1, sizeof(rx_item_t));
    s_dap_exchange_mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_dap_tx_queue && s_control_tx_queue && s_dap_request_queue &&
                            s_dap_response_queue && s_dap_abort_queue &&
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

esp_err_t wireless_link_dap_exchange(const uint8_t *request, size_t length,
                                     uint8_t *response,
                                     TickType_t timeout)
{
    wireless_dap_exchange_item_t item = {
        .request = request,
        .length = length,
        .response = response,
        .result = ESP_ERR_TIMEOUT,
    };
    return wireless_link_dap_exchange_window(&item, 1U, timeout);
}

esp_err_t wireless_link_dap_exchange_window(wireless_dap_exchange_item_t *items,
                                            size_t count,
                                            TickType_t timeout)
{
    if (items == NULL || count == 0U ||
        count > CONFIG_WIRELESS_DAP_V2_PACKET_WINDOW ||
        count > DAP_WINDOW_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < count; ++i) {
        if (items[i].request == NULL || items[i].response == NULL ||
            !valid_dap_packet_length(items[i].length)) {
            return ESP_ERR_INVALID_ARG;
        }
        items[i].result = ESP_ERR_TIMEOUT;
    }
    if (xSemaphoreTake(s_dap_exchange_mutex, timeout) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    xQueueReset(s_dap_response_queue);
    const uint16_t base_sequence = reserve_sequences(count);
    const int64_t start_us = esp_timer_get_time();
    const TickType_t start_tick = xTaskGetTickCount();
    uint32_t abort_generation;
    uint32_t retries = 0U;
    bool aborting = false;
    uint8_t abort_rounds_remaining = 0U;
    bool pending[DAP_WINDOW_MAX] = {false};
    size_t pending_count = count;
    portENTER_CRITICAL(&s_state_lock);
    abort_generation = s_dap_abort_generation;
    s_stats.dap_requests += count;
    portEXIT_CRITICAL(&s_state_lock);
    for (size_t i = 0; i < count; ++i) {
        pending[i] = true;
    }

    while (pending_count != 0U &&
           (timeout == portMAX_DELAY ||
            (xTaskGetTickCount() - start_tick) < timeout)) {
        esp_err_t stop_result = ESP_OK;
        portENTER_CRITICAL(&s_state_lock);
        if (s_dap_abort_generation != abort_generation) {
            abort_generation = s_dap_abort_generation;
            aborting = true;
            abort_rounds_remaining = 2U;
        }
        portEXIT_CRITICAL(&s_state_lock);
        if (stop_result == ESP_OK && !wireless_link_is_connected()) {
            stop_result = ESP_ERR_INVALID_STATE;
        }
        if (stop_result != ESP_OK) {
            for (size_t i = 0; i < count; ++i) {
                if (pending[i]) {
                    items[i].result = stop_result;
                }
            }
            break;
        }

        for (size_t i = 0; i < count; ++i) {
            if (!pending[i]) {
                continue;
            }
            const uint8_t meta = DAP_WINDOW_META(count, i);
            (void)queue_tx_window(false, FRAME_DAP_REQUEST,
                                  (uint16_t)(base_sequence + i), meta,
                                  items[i].request, items[i].length,
                                  pdMS_TO_TICKS(10));
        }

        const TickType_t round_start = xTaskGetTickCount();
        const TickType_t retry_ticks =
            pdMS_TO_TICKS(CONFIG_WIRELESS_DAP_DAP_RETRY_MS);
        while (pending_count != 0U) {
            portENTER_CRITICAL(&s_state_lock);
            const uint32_t current_abort_generation = s_dap_abort_generation;
            portEXIT_CRITICAL(&s_state_lock);
            if (current_abort_generation != abort_generation) {
                abort_generation = current_abort_generation;
                aborting = true;
                abort_rounds_remaining = 2U;
            }
            const TickType_t now = xTaskGetTickCount();
            const TickType_t round_elapsed = now - round_start;
            const TickType_t total_elapsed = now - start_tick;
            if (round_elapsed >= retry_ticks ||
                (timeout != portMAX_DELAY && total_elapsed >= timeout)) {
                break;
            }
            TickType_t wait = retry_ticks - round_elapsed;
            if (timeout != portMAX_DELAY && wait > timeout - total_elapsed) {
                wait = timeout - total_elapsed;
            }

            rx_item_t response_item;
            if (xQueueReceive(s_dap_response_queue, &response_item, wait) != pdTRUE) {
                break;
            }
            for (size_t i = 0; i < count; ++i) {
                if (!pending[i] ||
                    response_item.sequence != (uint16_t)(base_sequence + i) ||
                    response_item.length != items[i].length ||
                    response_item.window_meta != DAP_WINDOW_META(count, i)) {
                    continue;
                }
                memcpy(items[i].response, response_item.data, items[i].length);
                items[i].result = ESP_OK;
                pending[i] = false;
                pending_count--;
                break;
            }
        }

        if (aborting && pending_count != 0U) {
            if (abort_rounds_remaining != 0U) {
                abort_rounds_remaining--;
            }
            if (abort_rounds_remaining == 0U) {
                break;
            }
        }
        if (pending_count != 0U &&
            (timeout == portMAX_DELAY ||
             (xTaskGetTickCount() - start_tick) < timeout)) {
            retries += pending_count;
        }
    }

    const uint32_t elapsed_us = (uint32_t)(esp_timer_get_time() - start_us);
    uint32_t failures = 0U;
    esp_err_t result = ESP_OK;
    for (size_t i = 0; i < count; ++i) {
        if (items[i].result != ESP_OK) {
            failures++;
            if (result == ESP_OK) {
                result = items[i].result;
            }
        }
    }
    portENTER_CRITICAL(&s_state_lock);
    s_stats.dap_retries += retries;
    s_stats.dap_timeouts += failures;
    s_stats.last_dap_latency_us = elapsed_us;
    portEXIT_CRITICAL(&s_state_lock);
#if CONFIG_WIRELESS_DAP_DIAGNOSTICS
    ESP_LOGI(TAG, "dap base=%u count=%u result=%s latency=%" PRIu32
                  " us retries=%" PRIu32,
             base_sequence, (unsigned)count, esp_err_to_name(result),
             elapsed_us, retries);
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
                                    size_t *length, uint16_t *sequence,
                                    TickType_t timeout)
{
    if (request == NULL || length == NULL || sequence == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    rx_item_t item;
    if (xQueueReceive(s_dap_request_queue, &item, timeout) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (!valid_dap_packet_length(item.length)) {
        return ESP_ERR_INVALID_SIZE;
    }
    bool current = false;
    portENTER_CRITICAL(&s_state_lock);
    if (s_dap_window_active && !s_dap_window_cancelled) {
        const uint8_t index = DAP_WINDOW_INDEX(item.window_meta);
        if (index < s_dap_window_count) {
            dap_slot_t *slot = &s_dap_slots[index];
            current = slot->state == DAP_SLOT_QUEUED &&
                      slot->sequence == item.sequence &&
                      slot->length == item.length &&
                      slot->window_meta == item.window_meta;
            if (current) {
                slot->state = DAP_SLOT_EXECUTING;
            }
        }
    }
    portEXIT_CRITICAL(&s_state_lock);
    if (!current) {
        return ESP_ERR_INVALID_STATE;
    }
    memset(request, 0, WIRELESS_LINK_DAP_PACKET_SIZE);
    memcpy(request, item.data, item.length);
    *length = item.length;
    *sequence = item.sequence;
    return ESP_OK;
}

esp_err_t wireless_link_dap_reply(uint16_t sequence,
                                  const uint8_t *response, size_t length)
{
    if (response == NULL || !valid_dap_packet_length(length)) {
        return ESP_ERR_INVALID_ARG;
    }

    bool valid = false;
    uint8_t window_meta = 0U;
    portENTER_CRITICAL(&s_state_lock);
    if (s_connected && s_dap_window_active) {
        for (uint8_t i = 0; i < s_dap_window_count; ++i) {
            dap_slot_t *slot = &s_dap_slots[i];
            if (slot->state == DAP_SLOT_EXECUTING && slot->sequence == sequence &&
                slot->length == length) {
                memcpy(slot->response, response, length);
                slot->state = DAP_SLOT_COMPLETED;
                window_meta = slot->window_meta;
                valid = true;
                break;
            }
        }
    }
    portEXIT_CRITICAL(&s_state_lock);
    if (!valid) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t result = queue_tx_window(false, FRAME_DAP_RESPONSE, sequence,
                                       window_meta, response, length,
                                       pdMS_TO_TICKS(20));
    try_enqueue_next_dap_slot();
    return result;
}

esp_err_t wireless_link_dap_abort(void)
{
    if (s_role != WIRELESS_LINK_ROLE_TRANSMITTER) {
        return ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&s_state_lock);
    s_dap_abort_generation++;
    portEXIT_CRITICAL(&s_state_lock);
    const rx_item_t wake = {
        .type = FRAME_DAP_ABORT,
    };
    (void)xQueueSendToFront(s_dap_response_queue, &wake, 0);
    if (!wireless_link_is_connected()) {
        return ESP_ERR_INVALID_STATE;
    }
    const tx_item_t item = {
        .broadcast = false,
        .type = FRAME_DAP_ABORT,
        .sequence = next_sequence(),
        .length = 0U,
    };
    if (xQueueSendToFront(s_dap_tx_queue, &item, 0) == pdTRUE) {
        return ESP_OK;
    }
    portENTER_CRITICAL(&s_state_lock);
    s_stats.tx_dropped++;
    portEXIT_CRITICAL(&s_state_lock);
    return ESP_ERR_TIMEOUT;
}

esp_err_t wireless_link_dap_receive_abort(TickType_t timeout)
{
    if (s_role != WIRELESS_LINK_ROLE_RECEIVER) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t signal;
    return xQueueReceive(s_dap_abort_queue, &signal, timeout) == pdTRUE
               ? ESP_OK
               : ESP_ERR_TIMEOUT;
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
