#include "gw_core/device_registry.h"

#include <string.h>

// MVP: in-memory fixed-size registry.
// Later: replace with proper map + persistence + iteration/filter APIs.
static bool s_inited;
static gw_device_t s_devices[32];
static size_t s_device_count;

esp_err_t gw_device_registry_init(void)
{
    s_inited = true;
    s_device_count = 0;
    memset(s_devices, 0, sizeof(s_devices));
    return ESP_OK;
}

static size_t find_device_index(const gw_device_uid_t *uid)
{
    for (size_t i = 0; i < s_device_count; i++) {
        if (strncmp(uid->uid, s_devices[i].device_uid.uid, sizeof(uid->uid)) == 0) {
            return i;
        }
    }
    return (size_t)-1;
}

esp_err_t gw_device_registry_upsert(const gw_device_t *device)
{
    if (!s_inited || device == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t idx = find_device_index(&device->device_uid);
    if (idx != (size_t)-1) {
        s_devices[idx] = *device;
        return ESP_OK;
    }

    if (s_device_count >= (sizeof(s_devices) / sizeof(s_devices[0]))) {
        return ESP_ERR_NO_MEM;
    }

    s_devices[s_device_count++] = *device;
    return ESP_OK;
}

esp_err_t gw_device_registry_get(const gw_device_uid_t *uid, gw_device_t *out_device)
{
    if (!s_inited || uid == NULL || out_device == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t idx = find_device_index(uid);
    if (idx == (size_t)-1) {
        return ESP_ERR_NOT_FOUND;
    }
    *out_device = s_devices[idx];
    return ESP_OK;
}

esp_err_t gw_device_registry_remove(const gw_device_uid_t *uid)
{
    if (!s_inited || uid == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t idx = find_device_index(uid);
    if (idx == (size_t)-1) {
        return ESP_ERR_NOT_FOUND;
    }

    // Shift down to keep array packed.
    for (size_t i = idx + 1; i < s_device_count; i++) {
        s_devices[i - 1] = s_devices[i];
    }
    s_device_count--;
    memset(&s_devices[s_device_count], 0, sizeof(s_devices[s_device_count]));
    return ESP_OK;
}

size_t gw_device_registry_list(gw_device_t *out_devices, size_t max_devices)
{
    if (!s_inited || out_devices == NULL || max_devices == 0) {
        return 0;
    }
    size_t count = s_device_count < max_devices ? s_device_count : max_devices;
    memcpy(out_devices, s_devices, count * sizeof(gw_device_t));
    return count;
}
