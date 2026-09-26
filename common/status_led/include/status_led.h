#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    STATUS_LED_BOOTING = 0,
    STATUS_LED_WAITING,
    STATUS_LED_CONNECTED,
    STATUS_LED_ACTIVITY,
    STATUS_LED_ERROR,
} status_led_state_t;

typedef enum {
    STATUS_LED_ROLE_TRANSMITTER = 0,
    STATUS_LED_ROLE_RECEIVER,
} status_led_role_t;

esp_err_t status_led_init(status_led_role_t role);
void status_led_set(status_led_state_t state);
void status_led_pulse_activity(void);

#ifdef __cplusplus
}
#endif
