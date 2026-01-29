#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "gw_core/types.h"

#ifdef __cplusplus
extern "C" {
#endif

// In-memory normalized device state for automations/conditions.
// Keyed by (device_uid, key) where key is a stable string like:
// - "onoff" (bool)
// - "temperature_c" (float)
// - "humidity_pct" (float)
// - "battery_pct" (uint)
// - "last_seen_ms" (uint64)

#define GW_STATE_KEY_MAX 24
#define GW_STATE_MAX_ITEMS 128

typedef enum {
    GW_STATE_VALUE_BOOL = 1,
    GW_STATE_VALUE_F32 = 2,
    GW_STATE_VALUE_U32 = 3,
    GW_STATE_VALUE_U64 = 4,
} gw_state_value_type_t;

typedef struct {
    gw_device_uid_t uid;
    char key[GW_STATE_KEY_MAX];
    gw_state_value_type_t value_type;
    bool value_bool;
    float value_f32;
    uint32_t value_u32;
    uint64_t value_u64;
    uint64_t ts_ms;
} gw_state_item_t;

esp_err_t gw_state_store_init(void);

esp_err_t gw_state_store_set_bool(const gw_device_uid_t *uid, const char *key, bool value, uint64_t ts_ms);
esp_err_t gw_state_store_set_f32(const gw_device_uid_t *uid, const char *key, float value, uint64_t ts_ms);
esp_err_t gw_state_store_set_u32(const gw_device_uid_t *uid, const char *key, uint32_t value, uint64_t ts_ms);
esp_err_t gw_state_store_set_u64(const gw_device_uid_t *uid, const char *key, uint64_t value, uint64_t ts_ms);

esp_err_t gw_state_store_get(const gw_device_uid_t *uid, const char *key, gw_state_item_t *out);
size_t gw_state_store_list(const gw_device_uid_t *uid, gw_state_item_t *out, size_t max_out);

#ifdef __cplusplus
}
#endif

