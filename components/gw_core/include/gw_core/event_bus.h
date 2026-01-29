#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"

#include "gw_core/types.h"

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

typedef struct {
    uint8_t v; // event schema version (for clients)
    uint32_t id;
    uint64_t ts_ms;
    char type[32];
    char source[16];
    char device_uid[GW_DEVICE_UID_STRLEN];
    uint16_t short_addr;
    char msg[128];
    char payload_json[192]; // optional JSON object/array as string (unescaped)
} gw_event_t;

typedef void (*gw_event_bus_listener_t)(const gw_event_t *event, void *user_ctx);

esp_err_t gw_event_bus_init(void);
esp_err_t gw_event_bus_post(gw_event_id_t id, const void *data, size_t data_size, TickType_t ticks_to_wait);

// Lightweight, in-memory event log for UI/debugging.
uint32_t gw_event_bus_last_id(void);
void gw_event_bus_publish(const char *type, const char *source, const char *device_uid, uint16_t short_addr, const char *msg);
// Helper: publish a JSON payload string as msg (for normalized events).
void gw_event_bus_publish_json(const char *type, const char *source, const char *device_uid, uint16_t short_addr, const char *payload_json);
// Publish a single event with both a human-readable msg and a structured JSON payload (string).
void gw_event_bus_publish_ex(const char *type,
                            const char *source,
                            const char *device_uid,
                            uint16_t short_addr,
                            const char *msg,
                            const char *payload_json);
size_t gw_event_bus_list_since(uint32_t since_id, gw_event_t *out, size_t max_out, uint32_t *out_last_id);

// Optional listeners called for each gw_event_bus_publish(). Keep callbacks fast and non-blocking.
esp_err_t gw_event_bus_add_listener(gw_event_bus_listener_t cb, void *user_ctx);
esp_err_t gw_event_bus_remove_listener(gw_event_bus_listener_t cb, void *user_ctx);

#ifdef __cplusplus
}
#endif

