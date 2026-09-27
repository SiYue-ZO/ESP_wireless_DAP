#include "dap_protocol.h"

#include <string.h>

#include "dap_app.h"
#include "dap_gpio.h"
#include "dap_swd.h"
#include "esp_rom_sys.h"

static dap_state_t s_dap;
static volatile uint32_t s_abort_generation;
static uint32_t s_active_transfer_generation;
static uint16_t s_packet_capacity = DAP_PACKET_SIZE;

#define DAP_WRITE_FINISH_RECOVERY_RETRIES 2U

static bool dap_transfer_aborted(void)
{
    return s_abort_generation != s_active_transfer_generation;
}

/** @brief 标记当前 USB/DAP 会话已锁定到选定包大小。 */
static void dap_lock_packet_size(void)
{
    dap_app_lock_packet_size();
}

/** @brief 主机正在访问目标端时，标记 GPIO 按键已锁定。 */
static void dap_lock_active_session(void)
{
    dap_app_lock_controls();
}

/** @brief 从 CMSIS-DAP payload 中读取小端 32 位值。 */
static uint32_t dap_read_u32(const uint8_t *data)
{
    return ((uint32_t)data[0] << 0) |
           ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

/** @brief 向 CMSIS-DAP payload 写入小端 32 位值。 */
static void dap_write_u32(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)(value >> 0);
    data[1] = (uint8_t)(value >> 8);
    data[2] = (uint8_t)(value >> 16);
    data[3] = (uint8_t)(value >> 24);
}

/** @brief 在响应缓冲区空间足够时追加小端 32 位值。 */
static bool dap_append_u32(uint8_t *response, uint16_t *offset, uint32_t value)
{
    if ((*offset + 4U) > s_packet_capacity) {
        return false;
    }
    dap_write_u32(&response[*offset], value);
    *offset += 4U;
    return true;
}

/** @brief 将短信息字符串复制到 DAP_Info 响应。 */
static uint16_t dap_make_info_string(uint8_t command_id, const char *text, uint8_t *response)
{
    const size_t len = strlen(text) + 1U;
    response[0] = command_id;
    response[1] = (uint8_t)len;
    memcpy(&response[2], text, len);
    return (uint16_t)(2U + len);
}

/** @brief 执行一次带 WAIT 重试处理的 SWD 传输。 */
static uint8_t dap_swd_transfer_retry(uint8_t request, uint32_t *data, uint32_t *timestamp)
{
    uint16_t retry = s_dap.transfer.retry_count;
    uint8_t ack;
    do {
        ack = dap_swd_transfer(request, data, &s_dap.transfer, timestamp);
    } while ((ack == DAP_TRANSFER_WAIT) && (retry-- != 0U) &&
             !dap_transfer_aborted());
    return ack;
}

/** @brief 读取 DP/AP 寄存器，并通过 DP_RDBUFF 取回 AP 延迟读结果。 */
static uint8_t dap_swd_read_register(uint8_t request, uint32_t *data, uint32_t *timestamp)
{
    uint8_t ack = dap_swd_transfer_retry(request, data, timestamp);
    if ((ack == DAP_TRANSFER_OK) && ((request & DAP_TRANSFER_APnDP) != 0U)) {
        ack = dap_swd_transfer_retry((uint8_t)(DP_RDBUFF | DAP_TRANSFER_RnW), data, NULL);
    }
    return ack;
}

/** @brief 通过读取 RDBUFF 等待最后一次 DP/AP 写入完成并报告延迟错误。 */
static uint8_t dap_swd_finish_write(void)
{
    uint8_t recovery_retries = DAP_WRITE_FINISH_RECOVERY_RETRIES;
    uint8_t ack;

    do {
        ack = dap_swd_transfer_retry((uint8_t)(DP_RDBUFF | DAP_TRANSFER_RnW), NULL, NULL);
        if (ack == DAP_TRANSFER_OK || ack == DAP_TRANSFER_WAIT || ack == DAP_TRANSFER_FAULT) {
            break;
        }
    } while (recovery_retries-- != 0U && !dap_transfer_aborted());

    return ack;
}

/** @brief 处理 DAP_Info。 */
static uint16_t dap_cmd_info(const uint8_t *request, uint8_t *response, uint16_t *consumed)
{
    *consumed = 2U;

    switch (request[1]) {
    case DAP_ID_VENDOR:
        return dap_make_info_string(ID_DAP_Info, "Custom", response);
    case DAP_ID_PRODUCT:
        return dap_make_info_string(ID_DAP_Info, "ESP32S3 CMSIS-DAP", response);
    case DAP_ID_SER_NUM:
        return dap_make_info_string(ID_DAP_Info, DAP_SERIAL_NUMBER, response);
    case DAP_ID_DAP_FW_VER:
        return dap_make_info_string(ID_DAP_Info, DAP_FW_VERSION, response);
    case DAP_ID_CAPABILITIES:
        response[0] = ID_DAP_Info;
        response[1] = 1U;
        response[2] = DAP_CAP_SWD;
        return 3U;
    case DAP_ID_TIMESTAMP_CLOCK:
        response[0] = ID_DAP_Info;
        response[1] = 4U;
        dap_write_u32(&response[2], DAP_TIMESTAMP_CLOCK_HZ);
        return 6U;
    case DAP_ID_PACKET_COUNT:
        response[0] = ID_DAP_Info;
        response[1] = 1U;
        response[2] = DAP_PACKET_QUEUE_COUNT;
        return 3U;
    case DAP_ID_PACKET_SIZE: {
        dap_lock_packet_size();
        const uint16_t packet_size = dap_app_get_packet_size();
        response[0] = ID_DAP_Info;
        response[1] = 2U;
        response[2] = (uint8_t)(packet_size >> 0);
        response[3] = (uint8_t)(packet_size >> 8);
        return 4U;
    }
    default:
        response[0] = ID_DAP_Info;
        response[1] = 0U;
        return 2U;
    }
}

/** @brief 处理 DAP_Connect，并只允许选择 SWD 端口。 */
static uint16_t dap_cmd_connect(const uint8_t *request, uint8_t *response, uint16_t *consumed)
{
    uint8_t port = request[1];
    if (port == DAP_PORT_AUTODETECT) {
        port = DAP_PORT_SWD;
    }

    response[0] = ID_DAP_Connect;
    if (port == DAP_PORT_SWD) {
        dap_lock_active_session();
        s_dap.active_port = DAP_PORT_SWD;
        dap_gpio_setup_swd();
        response[1] = DAP_PORT_SWD;
    } else {
        response[1] = DAP_PORT_DISABLED;
    }

    *consumed = 2U;
    return 2U;
}

/** @brief 处理 DAP_SWJ_Pins：写入选定引脚并读取全部引脚状态。 */
static uint16_t dap_cmd_swj_pins(const uint8_t *request, uint8_t *response, uint16_t *consumed)
{
    dap_lock_active_session();

    const uint8_t value = request[1];
    const uint8_t select = request[2];
    uint32_t wait_us = dap_read_u32(&request[3]);
    if (wait_us > 3000000U) {
        wait_us = 3000000U;
    }

    dap_gpio_write_swj_pins(value, select);
    if (wait_us != 0U) {
        esp_rom_delay_us(wait_us);
    }

    response[0] = ID_DAP_SWJ_Pins;
    response[1] = dap_gpio_read_swj_pins();
    *consumed = 7U;
    return 2U;
}

/** @brief 处理 DAP_SWJ_Sequence。 */
static uint16_t dap_cmd_swj_sequence(const uint8_t *request, uint8_t *response, uint16_t *consumed)
{
    dap_lock_active_session();

    uint32_t bit_count = request[1];
    if (bit_count == 0U) {
        bit_count = 256U;
    }
    const uint16_t byte_count = (uint16_t)((bit_count + 7U) / 8U);
    dap_swd_swj_sequence(bit_count, &request[2]);

    response[0] = ID_DAP_SWJ_Sequence;
    response[1] = DAP_OK;
    *consumed = (uint16_t)(2U + byte_count);
    return 2U;
}

/** @brief 处理 DAP_SWD_Sequence。 */
static uint16_t dap_cmd_swd_sequence(const uint8_t *request, uint8_t *response, uint16_t *consumed)
{
    dap_lock_active_session();

    uint16_t in = 2U;
    uint16_t out = 2U;
    uint8_t sequence_count = request[1];

    response[0] = ID_DAP_SWD_Sequence;
    response[1] = (s_dap.active_port == DAP_PORT_SWD) ? DAP_OK : DAP_ERROR;

    while ((sequence_count-- != 0U) && in < s_packet_capacity &&
           out < s_packet_capacity) {
        const uint8_t info = request[in++];
        uint32_t bit_count = info & SWD_SEQUENCE_CLK;
        if (bit_count == 0U) {
            bit_count = 64U;
        }
        const uint16_t byte_count = (uint16_t)((bit_count + 7U) / 8U);

        if ((info & SWD_SEQUENCE_DIN) != 0U) {
            if ((uint16_t)(out + byte_count) > s_packet_capacity) {
                break;  // 响应缓冲区不足，停止以避免越界写
            }
            dap_swd_sequence(info, NULL, &response[out]);
            out = (uint16_t)(out + byte_count);
        } else {
            if ((uint16_t)(in + byte_count) > s_packet_capacity) {
                break;  // 请求已耗尽，停止以避免越界读
            }
            dap_swd_sequence(info, &request[in], NULL);
            in = (uint16_t)(in + byte_count);
        }
    }

    dap_gpio_swdio_output_enable();
    dap_gpio_swdio_write(1U);
    *consumed = in;
    return out;
}

/** @brief 处理 DAP_TransferConfigure。 */
static uint16_t dap_cmd_transfer_configure(const uint8_t *request, uint8_t *response, uint16_t *consumed)
{
    dap_lock_active_session();

    s_dap.transfer.idle_cycles = request[1];
    s_dap.transfer.retry_count = (uint16_t)request[2] | ((uint16_t)request[3] << 8);
    s_dap.transfer.match_retry = (uint16_t)request[4] | ((uint16_t)request[5] << 8);

    response[0] = ID_DAP_TransferConfigure;
    response[1] = DAP_OK;
    *consumed = 6U;
    return 2U;
}

/** @brief 处理一个 CMSIS-DAP 传输请求项。 */
static uint8_t dap_process_transfer_item(uint8_t request_value, const uint8_t **request, uint8_t *response,
                                         uint16_t *out, bool *check_write)
{
    uint32_t data = 0;
    uint32_t timestamp = 0;
    uint8_t ack = DAP_TRANSFER_OK;

    if ((request_value & DAP_TRANSFER_RnW) != 0U) {
        *check_write = false;
        if ((request_value & DAP_TRANSFER_MATCH_VALUE) != 0U) {
            const uint32_t match_value = dap_read_u32(*request);
            *request += 4;
            uint16_t match_retry = s_dap.transfer.match_retry;
            do {
                ack = dap_swd_read_register(request_value, &data, &timestamp);
            } while ((ack == DAP_TRANSFER_OK) &&
                     ((data & s_dap.transfer.match_mask) != match_value) &&
                     (match_retry-- != 0U) &&
                     !dap_transfer_aborted());
            if ((ack == DAP_TRANSFER_OK) && ((data & s_dap.transfer.match_mask) != match_value)) {
                ack |= DAP_TRANSFER_MISMATCH;
            }
        } else {
            ack = dap_swd_read_register(request_value, &data, &timestamp);
            if (ack == DAP_TRANSFER_OK) {
                dap_append_u32(response, out, data);
            }
        }
    } else {
        data = dap_read_u32(*request);
        *request += 4;
        if ((request_value & DAP_TRANSFER_MATCH_MASK) != 0U) {
            s_dap.transfer.match_mask = data;
            ack = DAP_TRANSFER_OK;
        } else {
            ack = dap_swd_transfer_retry(request_value, &data, &timestamp);
            if (ack == DAP_TRANSFER_OK) {
                *check_write = true;
            }
        }
    }

    if ((ack == DAP_TRANSFER_OK) && ((request_value & DAP_TRANSFER_TIMESTAMP) != 0U)) {
        dap_append_u32(response, out, timestamp);
    }

    return ack;
}

/** @brief 处理面向 SWD 的 DAP_Transfer。 */
static uint16_t dap_cmd_transfer(const uint8_t *request, uint8_t *response, uint16_t *consumed)
{
    dap_lock_active_session();

    const uint8_t *cursor = &request[3];
    uint16_t out = 3U;
    uint8_t response_count = 0;
    uint8_t response_value = 0;
    uint8_t request_count = request[2];
    bool check_write = false;

    s_active_transfer_generation = s_abort_generation;
    response[0] = ID_DAP_Transfer;

    if (s_dap.active_port != DAP_PORT_SWD) {
        while (request_count-- != 0U) {
            const uint8_t request_value = *cursor++;
            if (((request_value & DAP_TRANSFER_RnW) == 0U) ||
                ((request_value & DAP_TRANSFER_MATCH_VALUE) != 0U)) {
                cursor += 4;
            }
        }
        response[1] = 0U;
        response[2] = 0U;
        *consumed = (uint16_t)(cursor - request);
        return 3U;
    }

    while ((request_count != 0U) && !dap_transfer_aborted() &&
           out < s_packet_capacity) {
        request_count--;
        const uint8_t request_value = *cursor++;
        response_value = dap_process_transfer_item(request_value, &cursor, response, &out, &check_write);
        if (response_value != DAP_TRANSFER_OK) {
            break;
        }
        response_count++;
    }

    while (request_count != 0U) {
        request_count--;
        const uint8_t request_value = *cursor++;
        if ((request_value & DAP_TRANSFER_RnW) != 0U) {
            if ((request_value & DAP_TRANSFER_MATCH_VALUE) != 0U) {
                cursor += 4;
            }
        } else {
            cursor += 4;
        }
    }

    if ((response_value == DAP_TRANSFER_OK) && check_write) {
        response_value = dap_swd_finish_write();
    }

    response[1] = response_count;
    response[2] = response_value;
    *consumed = (uint16_t)(cursor - request);
    return out;
}

/** @brief 处理面向 SWD 的 DAP_TransferBlock。 */
static uint16_t dap_cmd_transfer_block(const uint8_t *request, uint16_t request_len,
                                       uint8_t *response, uint16_t *consumed)
{
    dap_lock_active_session();

    response[0] = ID_DAP_TransferBlock;
    if (request_len < 5U) {
        response[1] = 0U;
        response[2] = 0U;
        response[3] = DAP_TRANSFER_ERROR;
        *consumed = request_len;
        return 4U;
    }

    uint16_t request_count = (uint16_t)request[2] | ((uint16_t)request[3] << 8);
    const uint8_t request_value = request[4];
    if ((request_value & DAP_TRANSFER_RnW) == 0U &&
        request_count > (uint16_t)((request_len - 5U) / 4U)) {
        response[1] = 0U;
        response[2] = 0U;
        response[3] = DAP_TRANSFER_ERROR;
        *consumed = request_len;
        return 4U;
    }
    const uint8_t *cursor = &request[5];
    uint16_t response_count = 0;
    uint8_t response_value = 0;
    uint16_t out = 4U;

    s_active_transfer_generation = s_abort_generation;
    if (s_dap.active_port != DAP_PORT_SWD) {
        if ((request_value & DAP_TRANSFER_RnW) == 0U) {
            cursor += (uint32_t)request_count * 4U;
        }
        response[1] = 0U;
        response[2] = 0U;
        response[3] = 0U;
        *consumed = (uint16_t)(cursor - request);
        return 4U;
    }

    while ((request_count != 0U) && !dap_transfer_aborted() &&
           out < s_packet_capacity) {
        request_count--;
        uint32_t data = 0;
        uint32_t timestamp = 0;
        if ((request_value & DAP_TRANSFER_RnW) != 0U) {
            response_value = dap_swd_read_register(request_value, &data, &timestamp);
            if (response_value == DAP_TRANSFER_OK) {
                dap_append_u32(response, &out, data);
            }
        } else {
            data = dap_read_u32(cursor);
            cursor += 4;
            response_value = dap_swd_transfer_retry(request_value, &data, &timestamp);
        }

        if (response_value != DAP_TRANSFER_OK) {
            break;
        }
        response_count++;
    }

    if ((request_value & DAP_TRANSFER_RnW) == 0U) {
        cursor += (uint32_t)request_count * 4U;
    }

    if ((response_value == DAP_TRANSFER_OK) &&
        ((request_value & DAP_TRANSFER_RnW) == 0U) &&
        (response_count != 0U)) {
        response_value = dap_swd_finish_write();
    }

    response[1] = (uint8_t)(response_count >> 0);
    response[2] = (uint8_t)(response_count >> 8);
    response[3] = response_value;
    *consumed = (uint16_t)(cursor - request);
    return out;
}

/** @brief 处理一个非队列模式的 CMSIS-DAP 命令。 */
static uint16_t dap_process_command(const uint8_t *request, uint16_t request_len, uint8_t *response, uint16_t *consumed)
{
    if (request_len == 0U) {
        *consumed = 0U;
        return 0U;
    }

    switch (request[0]) {
    case ID_DAP_Info:
        return dap_cmd_info(request, response, consumed);
    case ID_DAP_HostStatus:
        response[0] = ID_DAP_HostStatus;
        response[1] = DAP_OK;
        *consumed = 3U;
        return 2U;
    case ID_DAP_Connect:
        return dap_cmd_connect(request, response, consumed);
    case ID_DAP_Disconnect:
        s_dap.active_port = DAP_PORT_DISABLED;
        dap_gpio_disable_port();
        dap_app_prepare_next_session();
        response[0] = ID_DAP_Disconnect;
        response[1] = DAP_OK;
        *consumed = 1U;
        return 2U;
    case ID_DAP_TransferConfigure:
        return dap_cmd_transfer_configure(request, response, consumed);
    case ID_DAP_Transfer:
        return dap_cmd_transfer(request, response, consumed);
    case ID_DAP_TransferBlock:
        return dap_cmd_transfer_block(request, request_len, response, consumed);
    case ID_DAP_WriteABORT: {
        dap_lock_active_session();
        const uint32_t value = dap_read_u32(&request[2]);
        dap_swd_write_abort(value, &s_dap.transfer);
        response[0] = ID_DAP_WriteABORT;
        response[1] = DAP_OK;
        *consumed = 6U;
        return 2U;
    }
    case ID_DAP_Delay: {
        dap_lock_active_session();
        const uint16_t delay_us = (uint16_t)request[1] | ((uint16_t)request[2] << 8);
        esp_rom_delay_us(delay_us);
        response[0] = ID_DAP_Delay;
        response[1] = DAP_OK;
        *consumed = 3U;
        return 2U;
    }
    case ID_DAP_ResetTarget:
        dap_lock_active_session();
        dap_gpio_reset_target();
        response[0] = ID_DAP_ResetTarget;
        response[1] = DAP_OK;
        response[2] = 1U;
        *consumed = 1U;
        return 3U;
    case ID_DAP_SWJ_Pins:
        return dap_cmd_swj_pins(request, response, consumed);
    case ID_DAP_SWJ_Clock:
        dap_lock_active_session();
        dap_gpio_set_swj_clock(dap_read_u32(&request[1]));
        response[0] = ID_DAP_SWJ_Clock;
        response[1] = DAP_OK;
        *consumed = 5U;
        return 2U;
    case ID_DAP_SWJ_Sequence:
        return dap_cmd_swj_sequence(request, response, consumed);
    case ID_DAP_SWD_Configure:
        dap_lock_active_session();
        s_dap.transfer.turnaround_cycles = (request[1] & 0x03U) + 1U;
        s_dap.transfer.always_generate_data_phase = (request[1] & 0x04U) != 0U;
        response[0] = ID_DAP_SWD_Configure;
        response[1] = DAP_OK;
        *consumed = 2U;
        return 2U;
    case ID_DAP_SWD_Sequence:
        return dap_cmd_swd_sequence(request, response, consumed);
    case ID_DAP_JTAG_Sequence:
    case ID_DAP_JTAG_Configure:
    case ID_DAP_JTAG_IDCODE:
        response[0] = request[0];
        response[1] = DAP_ERROR;
        *consumed = request_len;
        return 2U;
    default:
        response[0] = ID_DAP_Invalid;
        *consumed = 1U;
        return 1U;
    }
}

void dap_protocol_init(void)
{
    memset(&s_dap, 0, sizeof(s_dap));
    s_dap.transfer.retry_count = 100U;
    s_dap.transfer.match_retry = 0U;
    s_dap.transfer.match_mask = 0U;
    s_dap.transfer.turnaround_cycles = 1U;
    s_dap.transfer.always_generate_data_phase = false;
    s_dap.active_port = DAP_PORT_DISABLED;
    s_abort_generation = 0U;
    s_active_transfer_generation = 0U;

    dap_gpio_init();
}

void dap_protocol_abort_transfer(void)
{
    s_abort_generation++;
}

uint16_t dap_protocol_execute(const uint8_t *request, uint16_t request_len, uint8_t *response)
{
    if (request_len == 0U || request_len > DAP_PACKET_SIZE) {
        return 0U;
    }
    s_packet_capacity = request_len;

    if (request[0] != ID_DAP_ExecuteCommands) {
        uint16_t consumed = 0;
        return dap_process_command(request, request_len, response, &consumed);
    }

    response[0] = ID_DAP_ExecuteCommands;
    response[1] = 0U;

    uint16_t in = 2U;
    uint16_t out = 2U;
    uint8_t command_count = request[1];
    uint8_t executed_count = 0U;
    const uint32_t execute_abort_generation = s_abort_generation;
    while ((command_count-- != 0U) && in < request_len &&
           out < s_packet_capacity) {
        uint8_t command_response[DAP_PACKET_SIZE] = {0};
        uint16_t consumed = 0;
        const uint16_t remaining_request = (uint16_t)(request_len - in);
        const uint16_t written = dap_process_command(
            &request[in], remaining_request, command_response, &consumed);
        if (consumed == 0U || consumed > remaining_request || written == 0U ||
            written > (uint16_t)(s_packet_capacity - out)) {
            break;
        }
        memcpy(&response[out], command_response, written);
        in = (uint16_t)(in + consumed);
        out = (uint16_t)(out + written);
        executed_count++;
        if (s_abort_generation != execute_abort_generation) {
            break;
        }
    }
    response[1] = executed_count;

    return out;
}
