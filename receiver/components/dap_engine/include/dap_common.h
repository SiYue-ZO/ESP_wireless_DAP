#ifndef DAP_COMMON_H
#define DAP_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "sdkconfig.h"

#define DAP_FW_VERSION "0.1.0-wireless"
#define DAP_SERIAL_NUMBER "ESP32S3-WDAP-0001"

#define DAP_PACKET_SIZE 64U
#define DAP_PACKET_QUEUE_COUNT 1U

#ifndef CONFIG_WIRELESS_DAP_SWD_DEFAULT_CLOCK_HZ
#define CONFIG_WIRELESS_DAP_SWD_DEFAULT_CLOCK_HZ 1000000
#endif
#define DAP_DEFAULT_SWJ_CLOCK_HZ CONFIG_WIRELESS_DAP_SWD_DEFAULT_CLOCK_HZ
#define DAP_TIMESTAMP_CLOCK_HZ 1000000U

#define PIN_DAP_SWCLK_TCK ((gpio_num_t)CONFIG_WIRELESS_DAP_SWD_CLK_GPIO)
#define PIN_DAP_SWDIO_TMS ((gpio_num_t)CONFIG_WIRELESS_DAP_SWD_IO_GPIO)
#define PIN_DAP_TDI GPIO_NUM_6
#define PIN_DAP_TDO GPIO_NUM_7
#define PIN_DAP_NRESET ((gpio_num_t)CONFIG_WIRELESS_DAP_RESET_GPIO)

#define ID_DAP_Info 0x00U
#define ID_DAP_HostStatus 0x01U
#define ID_DAP_Connect 0x02U
#define ID_DAP_Disconnect 0x03U
#define ID_DAP_TransferConfigure 0x04U
#define ID_DAP_Transfer 0x05U
#define ID_DAP_TransferBlock 0x06U
#define ID_DAP_TransferAbort 0x07U
#define ID_DAP_WriteABORT 0x08U
#define ID_DAP_Delay 0x09U
#define ID_DAP_ResetTarget 0x0AU
#define ID_DAP_SWJ_Pins 0x10U
#define ID_DAP_SWJ_Clock 0x11U
#define ID_DAP_SWJ_Sequence 0x12U
#define ID_DAP_SWD_Configure 0x13U
#define ID_DAP_JTAG_Sequence 0x14U
#define ID_DAP_JTAG_Configure 0x15U
#define ID_DAP_JTAG_IDCODE 0x16U
#define ID_DAP_SWD_Sequence 0x1DU
#define ID_DAP_QueueCommands 0x7EU
#define ID_DAP_ExecuteCommands 0x7FU
#define ID_DAP_Invalid 0xFFU

#define DAP_OK 0x00U
#define DAP_ERROR 0xFFU

#define DAP_ID_VENDOR 0x01U
#define DAP_ID_PRODUCT 0x02U
#define DAP_ID_SER_NUM 0x03U
#define DAP_ID_DAP_FW_VER 0x04U
#define DAP_ID_DEVICE_VENDOR 0x05U
#define DAP_ID_DEVICE_NAME 0x06U
#define DAP_ID_CAPABILITIES 0xF0U
#define DAP_ID_TIMESTAMP_CLOCK 0xF1U
#define DAP_ID_PACKET_COUNT 0xFEU
#define DAP_ID_PACKET_SIZE 0xFFU

#define DAP_PORT_AUTODETECT 0U
#define DAP_PORT_DISABLED 0U
#define DAP_PORT_SWD 1U
#define DAP_PORT_JTAG 2U

#define DAP_CAP_SWD (1U << 0)

#define DAP_SWJ_SWCLK_TCK 0U
#define DAP_SWJ_SWDIO_TMS 1U
#define DAP_SWJ_TDI 2U
#define DAP_SWJ_TDO 3U
#define DAP_SWJ_nTRST 5U
#define DAP_SWJ_nRESET 7U

#define DAP_TRANSFER_APnDP (1U << 0)
#define DAP_TRANSFER_RnW (1U << 1)
#define DAP_TRANSFER_A2 (1U << 2)
#define DAP_TRANSFER_A3 (1U << 3)
#define DAP_TRANSFER_MATCH_VALUE (1U << 4)
#define DAP_TRANSFER_MATCH_MASK (1U << 5)
#define DAP_TRANSFER_TIMESTAMP (1U << 7)

#define DAP_TRANSFER_OK (1U << 0)
#define DAP_TRANSFER_WAIT (1U << 1)
#define DAP_TRANSFER_FAULT (1U << 2)
#define DAP_TRANSFER_ERROR (1U << 3)
#define DAP_TRANSFER_MISMATCH (1U << 4)

#define DP_ABORT 0x00U
#define DP_RDBUFF 0x0CU

#define SWD_SEQUENCE_CLK 0x3FU
#define SWD_SEQUENCE_DIN 0x80U

typedef struct {
    uint16_t retry_count;
    uint16_t match_retry;
    uint8_t idle_cycles;
    uint8_t turnaround_cycles;
    bool always_generate_data_phase;
    uint32_t match_mask;
} dap_transfer_config_t;

typedef struct {
    uint8_t active_port;
    dap_transfer_config_t transfer;
} dap_state_t;

#endif
