#pragma once

#include "esp_err.h"
#include "gw_core/event_bus.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t gw_rules_init(void);
esp_err_t gw_rules_handle_event(gw_event_id_t id, const void *data, size_t data_size);

#ifdef __cplusplus
}
#endif

