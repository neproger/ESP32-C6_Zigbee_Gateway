#include "gw_core/rules_engine.h"

static bool s_inited;

esp_err_t gw_rules_init(void)
{
    s_inited = true;
    return ESP_OK;
}

esp_err_t gw_rules_handle_event(gw_event_id_t id, const void *data, size_t data_size)
{
    (void)id;
    (void)data;
    (void)data_size;
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    // MVP placeholder: no rules yet.
    return ESP_OK;
}

