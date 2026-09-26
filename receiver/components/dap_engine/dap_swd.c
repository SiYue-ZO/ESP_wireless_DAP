#include "dap_swd.h"

#include "dap_gpio.h"

#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

static portMUX_TYPE s_swd_transfer_lock = portMUX_INITIALIZER_UNLOCKED;

/** @brief 在不改变 SWDIO 的情况下产生一个 SWCLK 周期。 */
static void IRAM_ATTR dap_swd_clock_cycle(void)
{
    dap_gpio_swclk_clear();
    dap_gpio_delay_half_period();
    dap_gpio_swclk_set();
    dap_gpio_delay_half_period();
}

/** @brief 写入一个 SWDIO 位，并通过时钟送入目标端。 */
static void IRAM_ATTR dap_swd_write_bit(uint32_t bit)
{
    dap_gpio_swdio_write(bit);
    dap_gpio_swclk_clear();
    dap_gpio_delay_half_period();
    dap_gpio_swclk_set();
    dap_gpio_delay_half_period();
}

/** @brief 在低电平阶段采样一个 SWDIO 位，并完成当前时钟。 */
static uint32_t IRAM_ATTR dap_swd_read_bit(void)
{
    dap_gpio_swclk_clear();
    dap_gpio_delay_half_period();
    const uint32_t bit = dap_gpio_swdio_read();
    dap_gpio_swclk_set();
    dap_gpio_delay_half_period();
    return bit & 1U;
}

/** @brief 产生已配置数量的 SWD turnaround 时钟。 */
static void IRAM_ATTR dap_swd_turnaround(const dap_transfer_config_t *config)
{
    for (uint32_t i = 0; i < config->turnaround_cycles; i++) {
        dap_swd_clock_cycle();
    }
}

/** @brief 传输结束后将 SWDIO 驱动为空闲高电平。 */
static void IRAM_ATTR dap_swd_drive_idle(void)
{
    dap_gpio_swdio_output_enable();
    dap_gpio_swdio_write(1U);
}

void IRAM_ATTR dap_swd_swj_sequence(uint32_t bit_count, const uint8_t *data)
{
    if (bit_count == 0U) {
        bit_count = 256U;
    }

    dap_gpio_swdio_output_enable();

    uint32_t bits_left_in_byte = 0;
    uint32_t value = 0;
    while (bit_count-- > 0U) {
        if (bits_left_in_byte == 0U) {
            value = *data++;
            bits_left_in_byte = 8U;
        }
        dap_gpio_swdio_write(value & 1U);
        dap_swd_clock_cycle();
        value >>= 1;
        bits_left_in_byte--;
    }
}

void IRAM_ATTR dap_swd_sequence(uint8_t info, const uint8_t *swdo, uint8_t *swdi)
{
    uint32_t bit_count = info & SWD_SEQUENCE_CLK;
    if (bit_count == 0U) {
        bit_count = 64U;
    }

    if ((info & SWD_SEQUENCE_DIN) != 0U) {
        dap_gpio_swdio_output_disable();
        while (bit_count > 0U) {
            uint32_t value = 0;
            uint32_t bits_in_byte = bit_count > 8U ? 8U : bit_count;
            for (uint32_t i = 0; i < bits_in_byte; i++) {
                value |= dap_swd_read_bit() << i;
            }
            *swdi++ = (uint8_t)value;
            bit_count -= bits_in_byte;
        }
    } else {
        dap_gpio_swdio_output_enable();
        while (bit_count > 0U) {
            uint32_t value = *swdo++;
            uint32_t bits_in_byte = bit_count > 8U ? 8U : bit_count;
            for (uint32_t i = 0; i < bits_in_byte; i++) {
                dap_swd_write_bit(value & 1U);
                value >>= 1;
            }
            bit_count -= bits_in_byte;
        }
    }
}

/** @brief 执行一次不能被中断打断的完整 SWD request/response 事务。 */
static uint8_t IRAM_ATTR dap_swd_transfer_critical(uint8_t request, uint32_t *data,
                                                   const dap_transfer_config_t *config, uint32_t *timestamp)
{
    uint32_t parity = 0;

    dap_gpio_swdio_output_enable();
    dap_swd_write_bit(1U);

    for (uint32_t i = 0; i < 4U; i++) {
        const uint32_t bit = (request >> i) & 1U;
        dap_swd_write_bit(bit);
        parity += bit;
    }

    dap_swd_write_bit(parity & 1U);
    dap_swd_write_bit(0U);
    dap_swd_write_bit(1U);

    dap_gpio_swdio_output_disable();
    dap_swd_turnaround(config);

    uint32_t ack = 0;
    ack |= dap_swd_read_bit() << 0;
    ack |= dap_swd_read_bit() << 1;
    ack |= dap_swd_read_bit() << 2;

    if (ack == DAP_TRANSFER_OK) {
        if ((request & DAP_TRANSFER_RnW) != 0U) {
            uint32_t value = 0;
            parity = 0;
            for (uint32_t i = 0; i < 32U; i++) {
                const uint32_t bit = dap_swd_read_bit();
                value |= bit << i;
                parity += bit;
            }
            const uint32_t parity_bit = dap_swd_read_bit();
            if (((parity ^ parity_bit) & 1U) != 0U) {
                ack = DAP_TRANSFER_ERROR;
            }
            if (data) {
                *data = value;
            }
            dap_swd_turnaround(config);
            dap_swd_drive_idle();
        } else {
            dap_swd_turnaround(config);
            dap_gpio_swdio_output_enable();
            uint32_t value = data ? *data : 0U;
            parity = 0;
            for (uint32_t i = 0; i < 32U; i++) {
                const uint32_t bit = (value >> i) & 1U;
                dap_swd_write_bit(bit);
                parity += bit;
            }
            dap_swd_write_bit(parity & 1U);
        }

        if (timestamp) {
            *timestamp = (uint32_t)esp_timer_get_time();
        }

        dap_gpio_swdio_write(0U);
        for (uint32_t i = 0; i < config->idle_cycles; i++) {
            dap_swd_clock_cycle();
        }
        dap_gpio_swdio_write(1U);
        return (uint8_t)ack;
    }

    if (ack == DAP_TRANSFER_WAIT || ack == DAP_TRANSFER_FAULT) {
        if (config->always_generate_data_phase && ((request & DAP_TRANSFER_RnW) != 0U)) {
            for (uint32_t i = 0; i < 33U; i++) {
                dap_swd_clock_cycle();
            }
        }

        dap_swd_turnaround(config);
        dap_gpio_swdio_output_enable();

        if (config->always_generate_data_phase && ((request & DAP_TRANSFER_RnW) == 0U)) {
            dap_gpio_swdio_write(0U);
            for (uint32_t i = 0; i < 33U; i++) {
                dap_swd_clock_cycle();
            }
        }

        dap_gpio_swdio_write(1U);
        return (uint8_t)ack;
    }

    for (uint32_t i = 0; i < (uint32_t)config->turnaround_cycles + 33U; i++) {
        dap_swd_clock_cycle();
    }
    dap_swd_drive_idle();
    return (uint8_t)ack;
}

uint8_t IRAM_ATTR dap_swd_transfer(uint8_t request, uint32_t *data,
                                   const dap_transfer_config_t *config, uint32_t *timestamp)
{
    portENTER_CRITICAL(&s_swd_transfer_lock);
    const uint8_t ack = dap_swd_transfer_critical(request, data, config, timestamp);
    portEXIT_CRITICAL(&s_swd_transfer_lock);
    return ack;
}

uint8_t dap_swd_write_abort(uint32_t value, const dap_transfer_config_t *config)
{
    return dap_swd_transfer(DP_ABORT, &value, config, NULL);
}
