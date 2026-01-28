#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Allow new devices to join the network for `seconds`.
esp_err_t gw_zigbee_permit_join(uint8_t seconds);

// Called from Zigbee signal handler when a device announces itself (join/rejoin).
void gw_zigbee_on_device_annce(const uint8_t ieee_addr[8], uint16_t short_addr, uint8_t capability);

#ifdef __cplusplus
}
#endif
