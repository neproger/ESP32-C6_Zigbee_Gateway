#include "gw_core/event_bus.h"

#include "esp_err.h"

ESP_EVENT_DEFINE_BASE(GW_EVENT_BASE);

static bool s_inited;

esp_err_t gw_event_bus_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }
    // Uses the default event loop created by esp_event_loop_create_default()
    s_inited = true;
    return ESP_OK;
}

esp_err_t gw_event_bus_post(gw_event_id_t id, const void *data, size_t data_size, TickType_t ticks_to_wait)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_event_post(GW_EVENT_BASE, (int32_t)id, data, data_size, ticks_to_wait);
}

