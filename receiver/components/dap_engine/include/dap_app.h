#ifndef DAP_APP_H
#define DAP_APP_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 接收端没有 USB 协议栈。dap_protocol.c 仍会使用 USB 工程中的少量会话钩子，
 * 因此这个头文件只暴露这些钩子，由 dap_app_stub.c 提供接收端状态。
 */
uint16_t dap_app_get_packet_size(void);
void dap_app_lock_packet_size(void);
void dap_app_lock_controls(void);
void dap_app_unlock_controls(void);
void dap_app_prepare_next_session(void);
bool dap_app_is_packet_size_locked(void);
bool dap_app_are_controls_locked(void);

#ifdef __cplusplus
}
#endif

#endif
