#ifndef DAP_SWD_H
#define DAP_SWD_H

#include "dap_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 使用 SWCLK 在 SWDIO/TMS 上输出 SWJ 序列。 */
void dap_swd_swj_sequence(uint32_t bit_count, const uint8_t *data);

/** @brief 输出或采集原始 SWD 序列。 */
void dap_swd_sequence(uint8_t info, const uint8_t *swdo, uint8_t *swdi);

/** @brief 执行一次不带重试策略的 SWD DP/AP 寄存器传输。 */
uint8_t dap_swd_transfer(uint8_t request, uint32_t *data, const dap_transfer_config_t *config, uint32_t *timestamp);

/** @brief 写入 SWD ABORT 寄存器。 */
uint8_t dap_swd_write_abort(uint32_t value, const dap_transfer_config_t *config);

#ifdef __cplusplus
}
#endif

#endif
