#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "cJSON.h"

#include "gw_core/automation_compiled.h"
#include "gw_core/types.h"

#ifdef __cplusplus
extern "C" {
#endif

// Execute a single action object (as defined in docs/automation-design.md).
// Returns ESP_OK if action was accepted/scheduled.
esp_err_t gw_action_exec(cJSON *action, char *err, size_t err_size);

// Fast-path executor for compiled rules (avoids cJSON allocations at runtime).
// Currently supports the MVP zigbee commands used by Automations UI:
// - onoff.on / onoff.off / onoff.toggle
// - level.move_to_level (arg0_u32=level 0..254, arg1_u32=transition_ms)
esp_err_t gw_action_exec_compiled_zigbee(const char *cmd,
                                        const gw_device_uid_t *device_uid,
                                        uint8_t endpoint,
                                        uint32_t arg0_u32,
                                        uint32_t arg1_u32,
                                        uint32_t arg2_u32,
                                        char *err,
                                        size_t err_size);

// Execute a compiled action record from a `.gwar` file.
// This is the main runtime path for automations (no JSON parsing).
esp_err_t gw_action_exec_compiled(const gw_auto_compiled_t *compiled,
                                 const gw_auto_bin_action_v2_t *action,
                                 char *err,
                                 size_t err_size);

#ifdef __cplusplus
}
#endif
