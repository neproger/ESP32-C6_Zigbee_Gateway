#include "gw_core/sensor_store.h"

#include <stdbool.h>
#include <string.h>

static bool s_inited;
static gw_sensor_value_t s_vals[GW_SENSOR_MAX_VALUES];
static size_t s_val_count;

static bool uid_equals(const gw_device_uid_t *a, const gw_device_uid_t *b)
{
    if (a == NULL || b == NULL) {
        return false;
    }
    return strncmp(a->uid, b->uid, sizeof(a->uid)) == 0;
}

static bool key_equals(const gw_sensor_value_t *a, const gw_sensor_value_t *b)
{
    return uid_equals(&a->uid, &b->uid) && a->endpoint == b->endpoint && a->cluster_id == b->cluster_id && a->attr_id == b->attr_id;
}

esp_err_t gw_sensor_store_init(void)
{
    s_inited = true;
    s_val_count = 0;
    memset(s_vals, 0, sizeof(s_vals));
    return ESP_OK;
}

esp_err_t gw_sensor_store_upsert(const gw_sensor_value_t *v)
{
    if (!s_inited || v == NULL || v->uid.uid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < s_val_count; i++) {
        if (key_equals(&s_vals[i], v)) {
            s_vals[i] = *v;
            return ESP_OK;
        }
    }

    if (s_val_count >= (sizeof(s_vals) / sizeof(s_vals[0]))) {
        return ESP_ERR_NO_MEM;
    }

    s_vals[s_val_count++] = *v;
    return ESP_OK;
}

size_t gw_sensor_store_list(const gw_device_uid_t *uid, gw_sensor_value_t *out, size_t max_out)
{
    if (!s_inited || uid == NULL || out == NULL || max_out == 0) {
        return 0;
    }

    size_t written = 0;
    for (size_t i = 0; i < s_val_count && written < max_out; i++) {
        if (uid_equals(&s_vals[i].uid, uid)) {
            out[written++] = s_vals[i];
        }
    }
    return written;
}
