#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "gw_core/types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GW_ZIGBEE_ONOFF_CMD_OFF = 0,
    GW_ZIGBEE_ONOFF_CMD_ON = 1,
    GW_ZIGBEE_ONOFF_CMD_TOGGLE = 2,
} gw_zigbee_onoff_cmd_t;

typedef struct {
    uint8_t level;          // 0..254
    uint16_t transition_ms; // 0 = immediate
} gw_zigbee_level_t;

typedef struct {
    uint16_t x;             // 0..65535
    uint16_t y;             // 0..65535
    uint16_t transition_ms; // 0 = immediate
} gw_zigbee_color_xy_t;

typedef struct {
    uint16_t mireds;        // typical 153..500
    uint16_t transition_ms; // 0 = immediate
} gw_zigbee_color_temp_t;

// Allow new devices to join the network for `seconds`.
esp_err_t gw_zigbee_permit_join(uint8_t seconds);

// Called from Zigbee signal handler when a device announces itself (join/rejoin).
void gw_zigbee_on_device_annce(const uint8_t ieee_addr[8], uint16_t short_addr, uint8_t capability);

// Ask a device to leave the network (and optionally rejoin). Requires its IEEE and current short address.
esp_err_t gw_zigbee_device_leave(const gw_device_uid_t *uid, uint16_t short_addr, bool rejoin);

// If we receive messages from an unknown short address, trigger discovery (IEEE -> endpoints/clusters).
// Safe to call from any context; request is scheduled into Zigbee context.
esp_err_t gw_zigbee_discover_by_short(uint16_t short_addr);

// Send On/Off/Toggle to a device endpoint (action executor primitive for automations).
// Safe to call from any context; request is scheduled into Zigbee context.
esp_err_t gw_zigbee_onoff_cmd(const gw_device_uid_t *uid, uint8_t endpoint, gw_zigbee_onoff_cmd_t cmd);

// Send Level Control "move_to_level" (0..254).
esp_err_t gw_zigbee_level_move_to_level(const gw_device_uid_t *uid, uint8_t endpoint, gw_zigbee_level_t level);

// Send Color Control "move_to_color" (xy in 0..65535).
esp_err_t gw_zigbee_color_move_to_xy(const gw_device_uid_t *uid, uint8_t endpoint, gw_zigbee_color_xy_t color);

// Send Color Control "move_to_color_temperature" (mireds).
esp_err_t gw_zigbee_color_move_to_temp(const gw_device_uid_t *uid, uint8_t endpoint, gw_zigbee_color_temp_t temp);

#ifdef __cplusplus
}
#endif
