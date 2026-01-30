#include "gw_core/automation_store.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

static const char *TAG = "gw_autos";

static bool s_inited;
static bool s_fs_inited;

#define GW_AUTOMATION_CAP 16

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    gw_automation_t items[GW_AUTOMATION_CAP];
} gw_automation_store_blob_t;

static gw_automation_store_blob_t s_store;

static const uint32_t MAGIC = 0x4155544f; // 'AUTO'
static const uint16_t VERSION = 1;
static const char *AUTOS_PATH = "/data/autos.bin";
static const char *AUTOS_TMP_PATH = "/data/autos.tmp";

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

    // Dedicated persistent partition for gateway data (separate from "www" so web updates don't wipe automations).
    const esp_vfs_spiffs_conf_t conf = {
        .base_path = "/data",
        .partition_label = "gw_data",
        .max_files = 4,
        .format_if_mount_failed = true, // non-prod: prefer recovering automatically
    };

    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spiffs mount failed (gw_data): %s (0x%x)", esp_err_to_name(err), (unsigned)err);
        return err;
    }

    s_fs_inited = true;
    return ESP_OK;
}

static esp_err_t save_to_fs(void)
{
    if (!s_fs_inited) {
        return ESP_ERR_INVALID_STATE;
    }

    // Write-then-rename to avoid partial writes on power loss.
    FILE *f = fopen(AUTOS_TMP_PATH, "wb");
    if (!f) {
        return ESP_FAIL;
    }

    size_t written = fwrite(&s_store, 1, sizeof(s_store), f);
    (void)fclose(f);
    if (written != sizeof(s_store)) {
        (void)remove(AUTOS_TMP_PATH);
        return ESP_FAIL;
    }

    (void)remove(AUTOS_PATH);
    if (rename(AUTOS_TMP_PATH, AUTOS_PATH) != 0) {
        // Best-effort fallback: keep tmp so user can inspect.
        return ESP_FAIL;
    }

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
                memset(tmp, 0, sizeof(*tmp));
                size_t read = fread(tmp, 1, sizeof(*tmp), f);
                if (read == sizeof(*tmp) && tmp->magic == MAGIC && tmp->version == VERSION && tmp->count <= GW_AUTOMATION_CAP) {
                    portENTER_CRITICAL(&s_lock);
                    s_store = *tmp;
                    portEXIT_CRITICAL(&s_lock);
                }
                free(tmp);
            }
            (void)fclose(f);
        }
    }

    s_inited = true;
    return ESP_OK;
}

size_t gw_automation_store_list(gw_automation_t *out, size_t max_out)
{
    if (!s_inited || !out || max_out == 0) return 0;
    portENTER_CRITICAL(&s_lock);
    size_t n = s_store.count < max_out ? s_store.count : max_out;
    memcpy(out, s_store.items, n * sizeof(gw_automation_t));
    portEXIT_CRITICAL(&s_lock);
    return n;
}

esp_err_t gw_automation_store_get(const char *id, gw_automation_t *out)
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

esp_err_t gw_automation_store_put(const gw_automation_t *a)
{
    if (!s_inited || !a || !a->id[0]) return ESP_ERR_INVALID_ARG;

    gw_automation_t normalized = *a;
    normalized.id[sizeof(normalized.id) - 1] = '\0';
    normalized.name[sizeof(normalized.name) - 1] = '\0';
    normalized.json[sizeof(normalized.json) - 1] = '\0';

    portENTER_CRITICAL(&s_lock);
    size_t idx = find_idx_locked(normalized.id);
    if (idx != (size_t)-1) {
        s_store.items[idx] = normalized;
    } else {
        if (s_store.count >= GW_AUTOMATION_CAP) {
            portEXIT_CRITICAL(&s_lock);
            return ESP_ERR_NO_MEM;
        }
        s_store.items[s_store.count++] = normalized;
    }
    portEXIT_CRITICAL(&s_lock);

    if (!s_fs_inited) {
        esp_err_t err = fs_init_once();
        if (err != ESP_OK) return err;
    }

    return save_to_fs();
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

    if (!s_fs_inited) {
        esp_err_t err = fs_init_once();
        if (err != ESP_OK) return err;
    }
    return save_to_fs();
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

    if (!s_fs_inited) {
        esp_err_t err = fs_init_once();
        if (err != ESP_OK) return err;
    }
    return save_to_fs();
}
