#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "dap_common.h"

void dap_protocol_init(void);
uint16_t dap_protocol_execute(const uint8_t *request, uint16_t request_length,
                              uint8_t *response);
void dap_protocol_abort_transfer(void);

#ifdef __cplusplus
}
#endif
