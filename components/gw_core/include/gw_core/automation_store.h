#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "gw_core/types.h" // Include the new centralized types

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t gw_automation_store_init(void);

// List/get functions now operate on the new, compiled-in-memory entry structure
size_t gw_automation_store_list(gw_automation_entry_t *out, size_t max_out);
size_t gw_automation_store_list_meta(gw_automation_meta_t *out, size_t max_out);
esp_err_t gw_automation_store_get(const char *id, gw_automation_entry_t *out);

// The 'put' function now takes the raw JSON string to be compiled and stored.
// This is the new primary way to add or update an automation.
esp_err_t gw_automation_store_put(const char *id, const char *name, bool enabled, const char *json_str);

esp_err_t gw_automation_store_remove(const char *id);
esp_err_t gw_automation_store_set_enabled(const char *id, bool enabled);

// Note: gw_automation_store_cleanup_orphaned() is removed as it's no longer needed.

#ifdef __cplusplus
}
#endif
