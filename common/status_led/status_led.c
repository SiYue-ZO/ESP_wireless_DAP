#include "status_led.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "sdkconfig.h"

static led_strip_handle_t s_strip;
static volatile status_led_state_t s_state = STATUS_LED_BOOTING;
static volatile TickType_t s_activity_until;
static status_led_role_t s_role;

static void set_rgb(uint8_t red, uint8_t green, uint8_t blue)
{
    if (s_strip == NULL) {
        return;
    }
    (void)led_strip_set_pixel(s_strip, 0, red, green, blue);
    (void)led_strip_refresh(s_strip);
}

static void led_task(void *argument)
{
    (void)argument;
    uint8_t phase = 0;

    while (true) {
        status_led_state_t state = s_state;
        if ((int32_t)(s_activity_until - xTaskGetTickCount()) > 0) {
            state = STATUS_LED_ACTIVITY;
        }

        switch (state) {
        case STATUS_LED_BOOTING:
            if (s_role == STATUS_LED_ROLE_TRANSMITTER) {
                set_rgb(0, 8, 24);
            } else {
                set_rgb(12, 0, 20);
            }
            break;
        case STATUS_LED_WAITING: {
            const uint8_t level = phase < 32U ? phase : (uint8_t)(63U - phase);
            if (s_role == STATUS_LED_ROLE_TRANSMITTER) {
                set_rgb((uint8_t)(level / 2U), (uint8_t)(level / 5U), 0);
            } else {
                set_rgb((uint8_t)(level / 3U), 0, (uint8_t)(level / 2U));
            }
            break;
        }
        case STATUS_LED_CONNECTED:
            if (s_role == STATUS_LED_ROLE_TRANSMITTER) {
                set_rgb(0, 14, 18);
            } else {
                set_rgb(0, 18, 2);
            }
            break;
        case STATUS_LED_ACTIVITY:
            set_rgb(0, 8, 30);
            break;
        case STATUS_LED_ERROR:
            set_rgb((phase & 0x10U) != 0U ? 35 : 0, 0, 0);
            break;
        }
        phase = (uint8_t)((phase + 2U) & 0x3fU);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

esp_err_t status_led_init(status_led_role_t role)
{
    s_role = role;
    const led_strip_config_t strip_config = {
        .strip_gpio_num = CONFIG_WIRELESS_DAP_LED_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags.invert_out = false,
    };
    const led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 64,
        .flags.with_dma = false,
    };
    esp_err_t ret = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip);
    if (ret != ESP_OK) {
        return ret;
    }
    if (xTaskCreate(led_task, "status_led", 2048, NULL, 3, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void status_led_set(status_led_state_t state)
{
    s_state = state;
}

void status_led_pulse_activity(void)
{
    s_activity_until = xTaskGetTickCount() + pdMS_TO_TICKS(80);
}
