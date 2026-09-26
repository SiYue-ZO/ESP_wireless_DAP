#ifndef DAP_PROTOCOL_H
#define DAP_PROTOCOL_H

#include "dap_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 重置 CMSIS-DAP 协议状态，并初始化 GPIO 时序默认值。 */
void dap_protocol_init(void);

/** @brief 请求取消当前正在执行的 SWD 传输循环。 */
void dap_protocol_abort_transfer(void);

/** @brief 执行一个 CMSIS-DAP 请求包，并返回响应长度。 */
uint16_t dap_protocol_execute(const uint8_t *request, uint16_t request_len, uint8_t *response);

#ifdef __cplusplus
}
#endif

#endif
