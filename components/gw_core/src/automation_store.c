#include "gw_core/automation_store.h"

#include <stdbool.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gw_core/automation_compiled.h"
#include "gw_core/types.h" // Includes new definitions

#include "esp_log.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

static const char *TAG = "gw_autos";

static bool s_inited;
static bool s_fs_inited;

#define GW_AUTOMATION_CAP 32

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    gw_automation_entry_t items[GW_AUTOMATION_CAP]; // Use the new compiled entry struct
} gw_automation_store_blob_t;

static gw_automation_store_blob_t s_store;

static const uint32_t MAGIC = 0x4155544f; // 'AUTO'
static const uint16_t VERSION = 2; // Version bump for the new format
static const char *AUTOS_PATH = "/data/autos.bin";

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static size_t find_idx_locked(const char *id)
{
    if (!id || !id[0]) return (size_t)-1;
    for (size_t i = 0; i < s_store.count; i++) {
        if (strncmp(s_store.items[i].id, id, sizeof(s_store.items[i].id)) == 0) {
            return i;
        }
    }
    return (size_t)-1;
}

static esp_err_t fs_init_once(void)
{
    if (s_fs_inited) {
        return ESP_OK;
    }

    const esp_vfs_spiffs_conf_t conf = {
        .base_path = "/data",
        .partition_label = "gw_data",
        .max_files = 4,
        .format_if_mount_failed = false,
    };

    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spiffs mount failed (gw_data): %s (0x%x)", esp_err_to_name(err), (unsigned)err);
        return err;
    }

    size_t total = 0, used = 0;
    if (esp_spiffs_info("gw_data", &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "gw_data SPIFFS mounted: total=%u KB, used=%u KB", (unsigned)(total / 1024), (unsigned)(used / 1024));
    }

    s_fs_inited = true;
    return ESP_OK;
}

static esp_err_t save_to_fs(void)
{
    if (!s_fs_inited) {
        ESP_LOGE(TAG, "save_to_fs: FS not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    FILE *f = fopen(AUTOS_PATH, "wb");
    if (!f) {
        ESP_LOGE(TAG, "save_to_fs: fopen(%s) failed, errno=%d", AUTOS_PATH, errno);
        return ESP_FAIL;
    }

    size_t written = fwrite(&s_store, 1, sizeof(s_store), f);
    fclose(f);

    if (written != sizeof(s_store)) {
        ESP_LOGE(TAG, "save_to_fs: wrote %zu bytes, expected %zu", written, sizeof(s_store));
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "save_to_fs: successfully wrote %zu bytes to %s", sizeof(s_store), AUTOS_PATH);
    return ESP_OK;
}

esp_err_t gw_automation_store_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }

    portENTER_CRITICAL(&s_lock);
    memset(&s_store, 0, sizeof(s_store));
    s_store.magic = MAGIC;
    s_store.version = VERSION;
    portEXIT_CRITICAL(&s_lock);

    (void)fs_init_once();

    if (s_fs_inited) {
        FILE *f = fopen(AUTOS_PATH, "rb");
        if (f) {
            gw_automation_store_blob_t *tmp = (gw_automation_store_blob_t *)malloc(sizeof(*tmp));
            if (tmp) {
                size_t read = fread(tmp, 1, sizeof(*tmp), f);
                ESP_LOGI(TAG, "automation file size: %u bytes (expected %u)", (unsigned)read, (unsigned)sizeof(*tmp));
                
                if (read == sizeof(*tmp) && tmp->magic == MAGIC && tmp->version == VERSION && tmp->count <= GW_AUTOMATION_CAP) {
                    portENTER_CRITICAL(&s_lock);
                    s_store = *tmp;
                    portEXIT_CRITICAL(&s_lock);
                    ESP_LOGI(TAG, "successfully loaded %u automations from disk", (unsigned)s_store.count);
                } else if (tmp->magic != MAGIC) {
                    ESP_LOGW(TAG, "autos magic mismatch - corrupt or old format");
                } else if (tmp->version != VERSION) {
                    ESP_LOGW(TAG, "autos version mismatch (got %u, expected %u) - incompatible format", (unsigned)tmp->version, (unsigned)VERSION);
                } else {
                    ESP_LOGW(TAG, "autos file read incomplete or corrupt");
                }
                free(tmp);
            }
            fclose(f);
        } else {
            ESP_LOGI(TAG, "no existing automations file at %s - starting fresh", AUTOS_PATH);
        }
    }

    s_inited = true;
    ESP_LOGI(TAG, "automation store initialized");
    return ESP_OK;
}

size_t gw_automation_store_list(gw_automation_entry_t *out, size_t max_out)
{
    if (!s_inited || !out || max_out == 0) return 0;
    portENTER_CRITICAL(&s_lock);
    size_t n = s_store.count < max_out ? s_store.count : max_out;
    memcpy(out, s_store.items, n * sizeof(gw_automation_entry_t));
    portEXIT_CRITICAL(&s_lock);
    return n;
}

size_t gw_automation_store_list_meta(gw_automation_meta_t *out, size_t max_out)
{
    if (!s_inited || !out || max_out == 0) return 0;
    portENTER_CRITICAL(&s_lock);
    size_t n = s_store.count < max_out ? s_store.count : max_out;
    for (size_t i = 0; i < n; i++) {
        const gw_automation_entry_t *a = &s_store.items[i];
        gw_automation_meta_t *m = &out[i];
        strlcpy(m->id, a->id, sizeof(m->id));
        strlcpy(m->name, a->name, sizeof(m->name));
        m->enabled = a->enabled;
    }
    portEXIT_CRITICAL(&s_lock);
    return n;
}

esp_err_t gw_automation_store_get(const char *id, gw_automation_entry_t *out)
{
    if (!s_inited || !id || !id[0] || !out) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_lock);
    size_t idx = find_idx_locked(id);
    if (idx == (size_t)-1) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_NOT_FOUND;
    }
    *out = s_store.items[idx];
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t gw_automation_store_put(const char *id, const char *name, bool enabled, const char *json_str)
{
    if (!s_inited || !id || !id[0] || !name || !json_str) return ESP_ERR_INVALID_ARG;

    if (!s_fs_inited) {
        fs_init_once();
    }

    gw_auto_compiled_t compiled_temp = {0};
    char err_buf[128] = {0};
    esp_err_t err = gw_auto_compile_json(json_str, &compiled_temp, err_buf, sizeof(err_buf));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Automation compile failed for %s: %s", id, err_buf);
        gw_auto_compiled_free(&compiled_temp);
        return err;
    }

    if (compiled_temp.hdr.trigger_count_total > GW_AUTO_MAX_TRIGGERS ||
        compiled_temp.hdr.condition_count_total > GW_AUTO_MAX_CONDITIONS ||
        compiled_temp.hdr.action_count_total > GW_AUTO_MAX_ACTIONS ||
        compiled_temp.hdr.strings_size > GW_AUTO_MAX_STRING_TABLE_BYTES) {
        ESP_LOGE(TAG, "Automation %s exceeds static limits (triggers: %u/%u, cond: %u/%u, act: %u/%u, strings: %u/%u)",
                 id,
                 compiled_temp.hdr.trigger_count_total, GW_AUTO_MAX_TRIGGERS,
                 compiled_temp.hdr.condition_count_total, GW_AUTO_MAX_CONDITIONS,
                 compiled_temp.hdr.action_count_total, GW_AUTO_MAX_ACTIONS,
                 compiled_temp.hdr.strings_size, GW_AUTO_MAX_STRING_TABLE_BYTES);
        gw_auto_compiled_free(&compiled_temp);
        return ESP_ERR_NO_MEM;
    }

    portENTER_CRITICAL(&s_lock);
    size_t idx = find_idx_locked(id);
    if (idx == (size_t)-1) {
        if (s_store.count >= GW_AUTOMATION_CAP) {
            portEXIT_CRITICAL(&s_lock);
            gw_auto_compiled_free(&compiled_temp);
            ESP_LOGW(TAG, "Cannot save automation %s: capacity exceeded", id);
            return ESP_ERR_NO_MEM;
        }
        idx = s_store.count++;
    }
    
    gw_automation_entry_t *entry = &s_store.items[idx];
    memset(entry, 0, sizeof(*entry));

    strlcpy(entry->id, id, sizeof(entry->id));
    strlcpy(entry->name, name, sizeof(entry->name));
    entry->enabled = enabled;

    entry->triggers_count = compiled_temp.hdr.trigger_count_total;
    if (entry->triggers_count > 0) {
        memcpy(entry->triggers, compiled_temp.triggers, entry->triggers_count * sizeof(gw_auto_bin_trigger_v2_t));
    }

    entry->conditions_count = compiled_temp.hdr.condition_count_total;
    if (entry->conditions_count > 0) {
        memcpy(entry->conditions, compiled_temp.conditions, entry->conditions_count * sizeof(gw_auto_bin_condition_v2_t));
    }

    entry->actions_count = compiled_temp.hdr.action_count_total;
    if (entry->actions_count > 0) {
        memcpy(entry->actions, compiled_temp.actions, entry->actions_count * sizeof(gw_auto_bin_action_v2_t));
    }

    entry->string_table_size = compiled_temp.hdr.strings_size;
    if (entry->string_table_size > 0) {
        memcpy(entry->string_table, compiled_temp.strings, entry->string_table_size);
    }
    
    portEXIT_CRITICAL(&s_lock);
    
    gw_auto_compiled_free(&compiled_temp);

    err = save_to_fs();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to persist automation %s to disk: %s", id, esp_err_to_name(err));
        // Consider rolling back the in-memory change here
        return err;
    }

    ESP_LOGI(TAG, "Automation saved and persisted: id=%s enabled=%u", id, (unsigned)enabled);
    return ESP_OK;
}

esp_err_t gw_automation_store_remove(const char *id)
{
    if (!s_inited || !id || !id[0]) return ESP_ERR_INVALID_ARG;

    portENTER_CRITICAL(&s_lock);
    size_t idx = find_idx_locked(id);
    if (idx == (size_t)-1) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_NOT_FOUND;
    }
    for (size_t i = idx + 1; i < s_store.count; i++) {
        s_store.items[i - 1] = s_store.items[i];
    }
    s_store.count--;
    memset(&s_store.items[s_store.count], 0, sizeof(s_store.items[s_store.count]));
    portEXIT_CRITICAL(&s_lock);

    esp_err_t err = save_to_fs();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "remove: failed to save after removing %s: %s", id, esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "automation removed and persisted: id=%s", id);
    return ESP_OK;
}

esp_err_t gw_automation_store_set_enabled(const char *id, bool enabled)
{
    if (!s_inited || !id || !id[0]) return ESP_ERR_INVALID_ARG;

    portENTER_CRITICAL(&s_lock);
    size_t idx = find_idx_locked(id);
    if (idx == (size_t)-1) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_NOT_FOUND;
    }
    s_store.items[idx].enabled = enabled;
    portEXIT_CRITICAL(&s_lock);

    return save_to_fs();
}
