#include "dap_app.h"

#include "dap_common.h"

static volatile bool s_packet_size_locked;
static volatile bool s_controls_locked;

uint16_t dap_app_get_packet_size(void)
{
    return DAP_PACKET_SIZE;
}

void dap_app_lock_packet_size(void)
{
    s_packet_size_locked = true;
}

void dap_app_lock_controls(void)
{
    s_controls_locked = true;
}

void dap_app_unlock_controls(void)
{
    s_packet_size_locked = false;
    s_controls_locked = false;
}

void dap_app_prepare_next_session(void)
{
    dap_app_unlock_controls();
}

bool dap_app_is_packet_size_locked(void)
{
    return s_packet_size_locked;
}

bool dap_app_are_controls_locked(void)
{
    return s_controls_locked;
}
