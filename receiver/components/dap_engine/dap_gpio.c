#include "dap_gpio.h"

#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_err.h"
#include "esp_rom_sys.h"
#include "sdkconfig.h"

static uint32_t s_half_period_cycles;
static dedic_gpio_bundle_handle_t s_swdio_bundle;
static dedic_gpio_bundle_handle_t s_swclk_bundle;

uint32_t g_dap_swclk_out_mask;
uint32_t g_dap_swdio_out_mask;
uint32_t g_dap_swdio_in_mask;
uint32_t g_dap_swdio_out_sel;

static void dap_gpio_delete_swd_bundles(void)
{
    if (s_swclk_bundle != NULL) {
        ESP_ERROR_CHECK(dedic_gpio_del_bundle(s_swclk_bundle));
        s_swclk_bundle = NULL;
    }
    if (s_swdio_bundle != NULL) {
        ESP_ERROR_CHECK(dedic_gpio_del_bundle(s_swdio_bundle));
        s_swdio_bundle = NULL;
    }

    g_dap_swclk_out_mask = 0U;
    g_dap_swdio_out_mask = 0U;
    g_dap_swdio_in_mask = 0U;
    g_dap_swdio_out_sel = 0U;
}

/** @brief 在当前 DAP 任务所在 CPU 核上绑定确定性 dedicated GPIO 通道。 */
static void dap_gpio_create_swd_bundles(void)
{
    int swdio_gpio[] = {PIN_DAP_SWDIO_TMS};
    const dedic_gpio_bundle_config_t swdio_config = {
        .gpio_array = swdio_gpio,
        .array_size = 1,
        .flags = {.in_en = 1, .out_en = 1},
    };
    ESP_ERROR_CHECK(dedic_gpio_new_bundle(&swdio_config, &s_swdio_bundle));
    ESP_ERROR_CHECK(dedic_gpio_get_out_mask(s_swdio_bundle, &g_dap_swdio_out_mask));
    ESP_ERROR_CHECK(dedic_gpio_get_in_mask(s_swdio_bundle, &g_dap_swdio_in_mask));

    int swclk_gpio[] = {PIN_DAP_SWCLK_TCK};
    const dedic_gpio_bundle_config_t swclk_config = {
        .gpio_array = swclk_gpio,
        .array_size = 1,
        .flags = {.out_en = 1},
    };
    ESP_ERROR_CHECK(dedic_gpio_new_bundle(&swclk_config, &s_swclk_bundle));
    ESP_ERROR_CHECK(dedic_gpio_get_out_mask(s_swclk_bundle, &g_dap_swclk_out_mask));

    g_dap_swdio_out_sel = REG_READ(GPIO_FUNC0_OUT_SEL_CFG_REG +
                                   (PIN_DAP_SWDIO_TMS * 4U));
}

/** @brief 返回用于周期计数延时的 CPU 频率。 */
static uint32_t dap_gpio_cpu_clock_hz(void)
{
#ifdef CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ
    return (uint32_t)CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ * 1000000U;
#else
    return 240000000U;
#endif
}

/** @brief 将单个 GPIO 配置为推挽输出。 */
static void dap_gpio_config_output(gpio_num_t gpio_num, int level)
{
    gpio_config_t config = {
        .pin_bit_mask = BIT64(gpio_num),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&config);
    gpio_set_level(gpio_num, level);
}

/** @brief 配置双向 SWDIO；输入缓冲保持开启，周转时只切换输出驱动。 */
static void dap_gpio_config_swdio(void)
{
    gpio_config_t config = {
        .pin_bit_mask = BIT64(PIN_DAP_SWDIO_TMS),
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_set_level(PIN_DAP_SWDIO_TMS, 1);
    gpio_config(&config);
    gpio_ll_set_drive_capability(&GPIO, PIN_DAP_SWDIO_TMS, GPIO_DRIVE_CAP_2);
}

/** @brief 将单个 GPIO 配置为输入，并可选上拉。 */
static void dap_gpio_config_input(gpio_num_t gpio_num, bool pull_up)
{
    gpio_config_t config = {
        .pin_bit_mask = BIT64(gpio_num),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = pull_up ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&config);
}

void dap_gpio_set_swj_clock(uint32_t clock_hz)
{
    if (clock_hz == 0U) {
        clock_hz = DAP_DEFAULT_SWJ_CLOCK_HZ;
    }

    const uint64_t cpu_hz = dap_gpio_cpu_clock_hz();
    uint64_t cycles = cpu_hz / ((uint64_t)clock_hz * 2ULL);
    if (cycles < 1ULL) {
        cycles = 1ULL;
    }
    if (cycles > UINT32_MAX) {
        cycles = UINT32_MAX;
    }
    s_half_period_cycles = (uint32_t)cycles;
}

void IRAM_ATTR dap_gpio_delay_half_period(void)
{
    const uint32_t start = (uint32_t)esp_cpu_get_cycle_count();
    while (((uint32_t)esp_cpu_get_cycle_count() - start) < s_half_period_cycles) {
    }
}

void dap_gpio_init(void)
{
    dap_gpio_set_swj_clock(DAP_DEFAULT_SWJ_CLOCK_HZ);
    dap_gpio_config_input(PIN_DAP_TDO, false);
    dap_gpio_config_input(PIN_DAP_NRESET, true);
    dap_gpio_disable_port();
}

void dap_gpio_setup_swd(void)
{
    dap_gpio_delete_swd_bundles();
    dap_gpio_config_output(PIN_DAP_SWCLK_TCK, 1);
    gpio_ll_set_drive_capability(&GPIO, PIN_DAP_SWCLK_TCK, GPIO_DRIVE_CAP_2);
    dap_gpio_config_swdio();
    dap_gpio_create_swd_bundles();
    dap_gpio_swclk_set();
    dap_gpio_swdio_write(1U);
    dap_gpio_config_output(PIN_DAP_TDI, 1);
    dap_gpio_config_input(PIN_DAP_TDO, false);
}

void dap_gpio_disable_port(void)
{
    dap_gpio_delete_swd_bundles();
    dap_gpio_config_input(PIN_DAP_SWCLK_TCK, true);
    dap_gpio_config_input(PIN_DAP_SWDIO_TMS, true);
    dap_gpio_config_input(PIN_DAP_TDI, true);
    dap_gpio_config_input(PIN_DAP_TDO, false);
    dap_gpio_config_input(PIN_DAP_NRESET, true);
}

uint8_t dap_gpio_read_swj_pins(void)
{
    uint8_t value = 0;
    value |= (uint8_t)(gpio_ll_get_level(&GPIO, PIN_DAP_SWCLK_TCK) << DAP_SWJ_SWCLK_TCK);
    value |= (uint8_t)(gpio_ll_get_level(&GPIO, PIN_DAP_SWDIO_TMS) << DAP_SWJ_SWDIO_TMS);
    value |= (uint8_t)(gpio_ll_get_level(&GPIO, PIN_DAP_TDI) << DAP_SWJ_TDI);
    value |= (uint8_t)(gpio_ll_get_level(&GPIO, PIN_DAP_TDO) << DAP_SWJ_TDO);
    value |= (uint8_t)(1U << DAP_SWJ_nTRST);
    value |= (uint8_t)(gpio_ll_get_level(&GPIO, PIN_DAP_NRESET) << DAP_SWJ_nRESET);
    return value;
}

void dap_gpio_write_swj_pins(uint8_t value, uint8_t select)
{
    if ((select & (1U << DAP_SWJ_SWCLK_TCK)) != 0U) {
        gpio_ll_output_enable(&GPIO, PIN_DAP_SWCLK_TCK);
        gpio_ll_set_level(&GPIO, PIN_DAP_SWCLK_TCK, (value >> DAP_SWJ_SWCLK_TCK) & 1U);
    }
    if ((select & (1U << DAP_SWJ_SWDIO_TMS)) != 0U) {
        gpio_ll_output_enable(&GPIO, PIN_DAP_SWDIO_TMS);
        gpio_ll_set_level(&GPIO, PIN_DAP_SWDIO_TMS, (value >> DAP_SWJ_SWDIO_TMS) & 1U);
    }
    if ((select & (1U << DAP_SWJ_TDI)) != 0U) {
        gpio_ll_output_enable(&GPIO, PIN_DAP_TDI);
        gpio_ll_set_level(&GPIO, PIN_DAP_TDI, (value >> DAP_SWJ_TDI) & 1U);
    }
    if ((select & (1U << DAP_SWJ_nRESET)) != 0U) {
        if (((value >> DAP_SWJ_nRESET) & 1U) == 0U) {
            dap_gpio_config_output(PIN_DAP_NRESET, 0);
        } else {
            dap_gpio_config_input(PIN_DAP_NRESET, true);
        }
    }
}

void dap_gpio_reset_target(void)
{
    dap_gpio_config_output(PIN_DAP_NRESET, 0);
    esp_rom_delay_us(20000);
    dap_gpio_config_input(PIN_DAP_NRESET, true);
    esp_rom_delay_us(20000);
}
