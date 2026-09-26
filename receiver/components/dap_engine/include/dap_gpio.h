#ifndef DAP_GPIO_H
#define DAP_GPIO_H

#include "dap_common.h"

#include "driver/dedic_gpio.h"
#include "driver/gpio.h"
#include "hal/dedic_gpio_cpu_ll.h"
#include "hal/gpio_ll.h"
#include "soc/gpio_reg.h"
#include "soc/gpio_struct.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 配置所有 CMSIS-DAP GPIO，并释放目标复位线。 */
void dap_gpio_init(void);

/** @brief 将 GPIO 配置为 SWD 模式。 */
void dap_gpio_setup_swd(void);

/** @brief 将 SWD/JTAG 引脚置为高阻输入模式。 */
void dap_gpio_disable_port(void);

/** @brief 更新 bit-bang 传输使用的 SWCLK 周期。 */
void dap_gpio_set_swj_clock(uint32_t clock_hz);

/** @brief 忙等一个已配置的 SWCLK 半周期。 */
void dap_gpio_delay_half_period(void);

/** @brief 读取所有 CMSIS-DAP SWJ 引脚的当前状态。 */
uint8_t dap_gpio_read_swj_pins(void);

/** @brief 按 CMSIS-DAP SWJ 写掩码更新引脚。 */
void dap_gpio_write_swj_pins(uint8_t value, uint8_t select);

/** @brief 拉低目标复位线后再释放。 */
void dap_gpio_reset_target(void);

extern uint32_t g_dap_swclk_out_mask;
extern uint32_t g_dap_swdio_out_mask;
extern uint32_t g_dap_swdio_in_mask;
extern uint32_t g_dap_swdio_out_sel;

/** @brief 将 SWCLK/TCK 置高。 */
static inline void dap_gpio_swclk_set(void)
{
    dedic_gpio_cpu_ll_write_mask(g_dap_swclk_out_mask, g_dap_swclk_out_mask);
}

/** @brief 将 SWCLK/TCK 置低。 */
static inline void dap_gpio_swclk_clear(void)
{
    dedic_gpio_cpu_ll_write_mask(g_dap_swclk_out_mask, 0);
}

/** @brief 读取 SWCLK/TCK 输入电平。 */
static inline uint32_t dap_gpio_swclk_read(void)
{
    return (uint32_t)gpio_ll_get_level(&GPIO, PIN_DAP_SWCLK_TCK);
}

/** @brief 设置 SWDIO/TMS 输出电平。 */
static inline void dap_gpio_swdio_write(uint32_t bit)
{
    dedic_gpio_cpu_ll_write_mask(g_dap_swdio_out_mask,
                                 (bit & 1U) ? g_dap_swdio_out_mask : 0U);
}

/** @brief 读取 SWDIO/TMS 输入电平。 */
static inline uint32_t dap_gpio_swdio_read(void)
{
    return (dedic_gpio_cpu_ll_read_in() & g_dap_swdio_in_mask) != 0U;
}

/** @brief 使能 SWDIO 输出模式。 */
static inline void dap_gpio_swdio_output_enable(void)
{
    REG_WRITE(GPIO_FUNC0_OUT_SEL_CFG_REG + (PIN_DAP_SWDIO_TMS * 4U),
              g_dap_swdio_out_sel);
    gpio_ll_output_enable(&GPIO, PIN_DAP_SWDIO_TMS);
}

/** @brief 关闭 SWDIO 输出模式，释放线路给目标端响应。 */
static inline void dap_gpio_swdio_output_disable(void)
{
    gpio_ll_output_disable(&GPIO, PIN_DAP_SWDIO_TMS);
    gpio_ll_input_enable(&GPIO, PIN_DAP_SWDIO_TMS);
}

/** @brief 设置 JTAG TDI 输出电平。 */
static inline void dap_gpio_tdi_write(uint32_t bit)
{
    gpio_ll_set_level(&GPIO, PIN_DAP_TDI, bit & 1U);
}

/** @brief 读取 JTAG TDO 输入电平。 */
static inline uint32_t dap_gpio_tdo_read(void)
{
    return (uint32_t)gpio_ll_get_level(&GPIO, PIN_DAP_TDO);
}

#ifdef __cplusplus
}
#endif

#endif
