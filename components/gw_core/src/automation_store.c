#include "gw_core/automation_store.h"

#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"
#include "nvs_flash.h"

static bool s_inited;

#define GW_AUTOMATION_CAP 16

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    gw_automation_t items[GW_AUTOMATION_CAP];
} gw_automation_store_blob_t;

static gw_automation_store_blob_t s_store;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static const char *NVS_NS = "gw";
static const char *NVS_KEY = "autos";
static const uint32_t MAGIC = 0x4155544f; // 'AUTO'
static const uint16_t VERSION = 1;

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

static esp_err_t save_to_nvs(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    err = nvs_set_blob(h, NVS_KEY, &s_store, sizeof(s_store));
    // If space is tight/fragmented, try erasing the key and writing again.
    if (err == ESP_ERR_NVS_NOT_ENOUGH_SPACE || err == ESP_ERR_NVS_NO_FREE_PAGES) {
        (void)nvs_erase_key(h, NVS_KEY);
        err = nvs_set_blob(h, NVS_KEY, &s_store, sizeof(s_store));
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
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

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err == ESP_OK) {
        size_t sz = 0;
        err = nvs_get_blob(h, NVS_KEY, NULL, &sz);
        if (err == ESP_OK && sz == sizeof(s_store)) {
            gw_automation_store_blob_t *tmp = (gw_automation_store_blob_t *)malloc(sizeof(*tmp));
            if (tmp == NULL) {
                err = ESP_ERR_NO_MEM;
            } else {
                memset(tmp, 0, sizeof(*tmp));
                err = nvs_get_blob(h, NVS_KEY, tmp, &sz);
                if (err == ESP_OK && tmp->magic == MAGIC && tmp->version == VERSION && tmp->count <= GW_AUTOMATION_CAP) {
                    portENTER_CRITICAL(&s_lock);
                    s_store = *tmp;
                    portEXIT_CRITICAL(&s_lock);
                } else if (err == ESP_OK) {
                    err = ESP_ERR_INVALID_STATE;
                }
                free(tmp);
            }
        } else if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
        nvs_close(h);
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }

    s_inited = true;
    return (err == ESP_OK) ? ESP_OK : ESP_OK;
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

    esp_err_t err = ESP_OK;
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

    err = save_to_nvs();
    return err;
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

    return save_to_nvs();
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

    return save_to_nvs();
}
