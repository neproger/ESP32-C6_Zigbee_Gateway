#include "gw_core/state_store.h"

#include <stdbool.h>
#include <string.h>

#include "freertos/FreeRTOS.h"

static bool s_inited;
static gw_state_item_t s_items[GW_STATE_MAX_ITEMS];
static size_t s_item_count;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static bool uid_equals(const gw_device_uid_t *a, const gw_device_uid_t *b)
{
    if (a == NULL || b == NULL) {
        return false;
    }
    return strncmp(a->uid, b->uid, sizeof(a->uid)) == 0;
}

static bool key_equals(const char *a, const char *b)
{
    if (a == NULL || b == NULL) {
        return false;
    }
    return strncmp(a, b, GW_STATE_KEY_MAX) == 0;
}

static size_t find_idx_locked(const gw_device_uid_t *uid, const char *key)
{
    if (uid == NULL || key == NULL || key[0] == '\0') {
        return (size_t)-1;
    }
    for (size_t i = 0; i < s_item_count; i++) {
        if (uid_equals(&s_items[i].uid, uid) && key_equals(s_items[i].key, key)) {
            return i;
        }
    }
    return (size_t)-1;
}

static size_t find_oldest_idx_locked(void)
{
    if (s_item_count == 0) {
        return (size_t)-1;
    }
    size_t oldest = 0;
    uint64_t oldest_ts = s_items[0].ts_ms;
    for (size_t i = 1; i < s_item_count; i++) {
        if (s_items[i].ts_ms < oldest_ts) {
            oldest = i;
            oldest_ts = s_items[i].ts_ms;
        }
    }
    return oldest;
}

esp_err_t gw_state_store_init(void)
{
    portENTER_CRITICAL(&s_lock);
    s_inited = true;
    s_item_count = 0;
    memset(s_items, 0, sizeof(s_items));
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

static esp_err_t upsert_item(const gw_state_item_t *item)
{
    if (!s_inited || item == NULL || item->uid.uid[0] == '\0' || item->key[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_lock);
    size_t idx = find_idx_locked(&item->uid, item->key);
    if (idx != (size_t)-1) {
        s_items[idx] = *item;
        portEXIT_CRITICAL(&s_lock);
        return ESP_OK;
    }

    if (s_item_count < (sizeof(s_items) / sizeof(s_items[0]))) {
        s_items[s_item_count++] = *item;
        portEXIT_CRITICAL(&s_lock);
        return ESP_OK;
    }

    // Evict oldest (bounded memory).
    idx = find_oldest_idx_locked();
    if (idx == (size_t)-1) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_NO_MEM;
    }
    s_items[idx] = *item;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t gw_state_store_set_bool(const gw_device_uid_t *uid, const char *key, bool value, uint64_t ts_ms)
{
    if (uid == NULL || key == NULL || key[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    gw_state_item_t item = {0};
    item.uid = *uid;
    strlcpy(item.key, key, sizeof(item.key));
    item.value_type = GW_STATE_VALUE_BOOL;
    item.value_bool = value;
    item.ts_ms = ts_ms;
    return upsert_item(&item);
}

esp_err_t gw_state_store_set_f32(const gw_device_uid_t *uid, const char *key, float value, uint64_t ts_ms)
{
    if (uid == NULL || key == NULL || key[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    gw_state_item_t item = {0};
    item.uid = *uid;
    strlcpy(item.key, key, sizeof(item.key));
    item.value_type = GW_STATE_VALUE_F32;
    item.value_f32 = value;
    item.ts_ms = ts_ms;
    return upsert_item(&item);
}

esp_err_t gw_state_store_set_u32(const gw_device_uid_t *uid, const char *key, uint32_t value, uint64_t ts_ms)
{
    if (uid == NULL || key == NULL || key[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    gw_state_item_t item = {0};
    item.uid = *uid;
    strlcpy(item.key, key, sizeof(item.key));
    item.value_type = GW_STATE_VALUE_U32;
    item.value_u32 = value;
    item.ts_ms = ts_ms;
    return upsert_item(&item);
}

esp_err_t gw_state_store_set_u64(const gw_device_uid_t *uid, const char *key, uint64_t value, uint64_t ts_ms)
{
    if (uid == NULL || key == NULL || key[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    gw_state_item_t item = {0};
    item.uid = *uid;
    strlcpy(item.key, key, sizeof(item.key));
    item.value_type = GW_STATE_VALUE_U64;
    item.value_u64 = value;
    item.ts_ms = ts_ms;
    return upsert_item(&item);
}

esp_err_t gw_state_store_get(const gw_device_uid_t *uid, const char *key, gw_state_item_t *out)
{
    if (!s_inited || uid == NULL || key == NULL || key[0] == '\0' || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_lock);
    size_t idx = find_idx_locked(uid, key);
    if (idx == (size_t)-1) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_NOT_FOUND;
    }
    *out = s_items[idx];
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

size_t gw_state_store_list(const gw_device_uid_t *uid, gw_state_item_t *out, size_t max_out)
{
    if (!s_inited || uid == NULL || out == NULL || max_out == 0) {
        return 0;
    }

    size_t written = 0;
    portENTER_CRITICAL(&s_lock);
    for (size_t i = 0; i < s_item_count && written < max_out; i++) {
        if (uid_equals(&s_items[i].uid, uid)) {
            out[written++] = s_items[i];
        }
    }
    portEXIT_CRITICAL(&s_lock);
    return written;
}

