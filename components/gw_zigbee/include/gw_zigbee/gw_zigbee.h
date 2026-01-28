#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "gw_core/types.h"

#ifdef __cplusplus
extern "C" {
#endif

// Allow new devices to join the network for `seconds`.
esp_err_t gw_zigbee_permit_join(uint8_t seconds);

// Called from Zigbee signal handler when a device announces itself (join/rejoin).
void gw_zigbee_on_device_annce(const uint8_t ieee_addr[8], uint16_t short_addr, uint8_t capability);

// Ask a device to leave the network (and optionally rejoin). Requires its IEEE and current short address.
esp_err_t gw_zigbee_device_leave(const gw_device_uid_t *uid, uint16_t short_addr, bool rejoin);

// If we receive messages from an unknown short address, trigger discovery (IEEE -> endpoints/clusters).
// Safe to call from any context; request is scheduled into Zigbee context.
esp_err_t gw_zigbee_discover_by_short(uint16_t short_addr);

#ifdef __cplusplus
}
#endif
