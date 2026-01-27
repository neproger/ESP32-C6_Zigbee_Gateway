#pragma once

#include "esp_err.h"
#include "esp_event.h"

#ifdef __cplusplus
extern "C" {
#endif

ESP_EVENT_DECLARE_BASE(GW_EVENT_BASE);

typedef enum {
    GW_EVENT_SYSTEM_BOOT = 1,
    GW_EVENT_API_REQUEST = 100,
    GW_EVENT_API_RESPONSE,
    GW_EVENT_ZIGBEE_RAW = 200,
    GW_EVENT_ZIGBEE_NORMALIZED,
    GW_EVENT_RULE_ACTION = 300,
    GW_EVENT_RULE_RESULT,
} gw_event_id_t;

esp_err_t gw_event_bus_init(void);
esp_err_t gw_event_bus_post(gw_event_id_t id, const void *data, size_t data_size, TickType_t ticks_to_wait);

#ifdef __cplusplus
}
#endif

