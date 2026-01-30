#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GW_AUTOMATION_ID_MAX   32
#define GW_AUTOMATION_NAME_MAX 48
#define GW_AUTOMATION_JSON_MAX 4096

typedef struct {
    char id[GW_AUTOMATION_ID_MAX];       // stable id (string)
    char name[GW_AUTOMATION_NAME_MAX];   // user label
    bool enabled;
    char json[GW_AUTOMATION_JSON_MAX];   // opaque rule definition (JSON string)
} gw_automation_t;

// Lightweight view for UI/status/code that does not need the full JSON body.
typedef struct {
    char id[GW_AUTOMATION_ID_MAX];
    char name[GW_AUTOMATION_NAME_MAX];
    bool enabled;
} gw_automation_meta_t;

esp_err_t gw_automation_store_init(void);
size_t gw_automation_store_list(gw_automation_t *out, size_t max_out);
size_t gw_automation_store_list_meta(gw_automation_meta_t *out, size_t max_out);
esp_err_t gw_automation_store_get(const char *id, gw_automation_t *out);
esp_err_t gw_automation_store_put(const gw_automation_t *a);
esp_err_t gw_automation_store_remove(const char *id);
esp_err_t gw_automation_store_set_enabled(const char *id, bool enabled);

// Cleanup orphaned .gwar compiled files (best-effort, no error if fails)
void gw_automation_store_cleanup_orphaned(void);

#ifdef __cplusplus
}
#endif
