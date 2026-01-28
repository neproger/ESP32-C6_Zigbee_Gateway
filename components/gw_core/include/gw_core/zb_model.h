#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "gw_core/types.h"

#ifdef __cplusplus
extern "C" {
#endif

// Extremely small “Zigbee model” cache: endpoints + clusters discovered via ActiveEP/SimpleDesc.
// In-memory only, meant for UI/debugging.

#define GW_ZB_MAX_ENDPOINTS 64
#define GW_ZB_MAX_CLUSTERS  16

typedef struct {
    gw_device_uid_t uid;
    uint16_t short_addr;
    uint8_t endpoint;
    uint16_t profile_id;
    uint16_t device_id;
    uint8_t in_cluster_count;
    uint8_t out_cluster_count;
    uint16_t in_clusters[GW_ZB_MAX_CLUSTERS];
    uint16_t out_clusters[GW_ZB_MAX_CLUSTERS];
} gw_zb_endpoint_t;

esp_err_t gw_zb_model_init(void);
esp_err_t gw_zb_model_upsert_endpoint(const gw_zb_endpoint_t *ep);
size_t gw_zb_model_list_endpoints(const gw_device_uid_t *uid, gw_zb_endpoint_t *out_eps, size_t max_eps);
bool gw_zb_model_find_uid_by_short(uint16_t short_addr, gw_device_uid_t *out_uid);

#ifdef __cplusplus
}
#endif
