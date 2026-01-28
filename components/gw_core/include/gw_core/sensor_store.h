#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "gw_core/types.h"

#ifdef __cplusplus
extern "C" {
#endif

// In-memory latest sensor values for UI/debugging.
// Values are stored in raw ZCL units (usually 0.01 of unit, depending on cluster).

#define GW_SENSOR_MAX_VALUES 64

typedef enum {
    GW_SENSOR_VALUE_I32 = 1,
    GW_SENSOR_VALUE_U32 = 2,
} gw_sensor_value_type_t;

typedef struct {
    gw_device_uid_t uid;
    uint16_t short_addr;
    uint8_t endpoint;
    uint16_t cluster_id;
    uint16_t attr_id;
    gw_sensor_value_type_t value_type;
    int32_t value_i32;
    uint32_t value_u32;
    uint64_t ts_ms;
} gw_sensor_value_t;

esp_err_t gw_sensor_store_init(void);
esp_err_t gw_sensor_store_upsert(const gw_sensor_value_t *v);
size_t gw_sensor_store_list(const gw_device_uid_t *uid, gw_sensor_value_t *out, size_t max_out);

#ifdef __cplusplus
}
#endif

