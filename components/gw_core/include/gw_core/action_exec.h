#pragma once

#include "esp_err.h"

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

// Execute a single action object (as defined in docs/automation-design.md).
// Returns ESP_OK if action was accepted/scheduled.
esp_err_t gw_action_exec(cJSON *action, char *err, size_t err_size);

#ifdef __cplusplus
}
#endif

