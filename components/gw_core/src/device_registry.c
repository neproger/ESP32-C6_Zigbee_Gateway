#include "gw_core/device_registry.h"

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "nvs.h"
#include "nvs_flash.h"

// Fixed-size registry with NVS persistence.
static bool s_inited;
static gw_device_t s_devices[32];
static size_t s_device_count;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static const char *NVS_NS = "gw";
static const char *NVS_KEY = "devices";
static const uint32_t MAGIC = 0x44564543; // 'DVEC'
static const uint16_t VERSION = 1;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    gw_device_t devices[32];
} gw_device_registry_blob_t;

static esp_err_t save_to_nvs(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    gw_device_registry_blob_t *blob = (gw_device_registry_blob_t *)calloc(1, sizeof(*blob));
    if (blob == NULL) {
        nvs_close(h);
        return ESP_ERR_NO_MEM;
    }
    blob->magic = MAGIC;
    blob->version = VERSION;

    portENTER_CRITICAL(&s_lock);
    blob->count = (uint16_t)s_device_count;
    memcpy(blob->devices, s_devices, sizeof(s_devices));
    portEXIT_CRITICAL(&s_lock);

    err = nvs_set_blob(h, NVS_KEY, blob, sizeof(*blob));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    free(blob);
    return err;
}

esp_err_t gw_device_registry_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }

    s_inited = true;

    portENTER_CRITICAL(&s_lock);
    s_device_count = 0;
    memset(s_devices, 0, sizeof(s_devices));
    portEXIT_CRITICAL(&s_lock);

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err == ESP_OK) {
        size_t sz = 0;
        err = nvs_get_blob(h, NVS_KEY, NULL, &sz);
        if (err == ESP_OK && sz == sizeof(gw_device_registry_blob_t)) {
            gw_device_registry_blob_t *blob = (gw_device_registry_blob_t *)calloc(1, sizeof(*blob));
            if (blob != NULL) {
                err = nvs_get_blob(h, NVS_KEY, blob, &sz);
            } else {
                err = ESP_ERR_NO_MEM;
            }
            if (err == ESP_OK && blob->magic == MAGIC && blob->version == VERSION && blob->count <= 32) {
                portENTER_CRITICAL(&s_lock);
                s_device_count = blob->count;
                memcpy(s_devices, blob->devices, sizeof(s_devices));
                portEXIT_CRITICAL(&s_lock);
            }
            free(blob);
        }
        nvs_close(h);
    }
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

    esp_err_t err = ESP_OK;

    portENTER_CRITICAL(&s_lock);
    size_t idx = find_device_index(&device->device_uid);
    if (idx != (size_t)-1) {
        s_devices[idx] = *device;
        portEXIT_CRITICAL(&s_lock);
        return save_to_nvs();
    }

    if (s_device_count >= (sizeof(s_devices) / sizeof(s_devices[0]))) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_NO_MEM;
    }

    s_devices[s_device_count++] = *device;
    portEXIT_CRITICAL(&s_lock);

    err = save_to_nvs();
    return err;
}

esp_err_t gw_device_registry_get(const gw_device_uid_t *uid, gw_device_t *out_device)
{
    if (!s_inited || uid == NULL || out_device == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_lock);
    size_t idx = find_device_index(uid);
    if (idx == (size_t)-1) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_NOT_FOUND;
    }
    *out_device = s_devices[idx];
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t gw_device_registry_set_name(const gw_device_uid_t *uid, const char *name)
{
    if (!s_inited || uid == NULL || uid->uid[0] == '\0' || name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_lock);
    size_t idx = find_device_index(uid);
    if (idx == (size_t)-1) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_NOT_FOUND;
    }

    strlcpy(s_devices[idx].name, name, sizeof(s_devices[idx].name));
    portEXIT_CRITICAL(&s_lock);

    return save_to_nvs();
}

esp_err_t gw_device_registry_remove(const gw_device_uid_t *uid)
{
    if (!s_inited || uid == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_lock);
    size_t idx = find_device_index(uid);
    if (idx == (size_t)-1) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_NOT_FOUND;
    }

    // Shift down to keep array packed.
    for (size_t i = idx + 1; i < s_device_count; i++) {
        s_devices[i - 1] = s_devices[i];
    }
    s_device_count--;
    memset(&s_devices[s_device_count], 0, sizeof(s_devices[s_device_count]));
    portEXIT_CRITICAL(&s_lock);

    return save_to_nvs();
}

size_t gw_device_registry_list(gw_device_t *out_devices, size_t max_devices)
{
    if (!s_inited || out_devices == NULL || max_devices == 0) {
        return 0;
    }

    portENTER_CRITICAL(&s_lock);
    size_t count = s_device_count < max_devices ? s_device_count : max_devices;
    memcpy(out_devices, s_devices, count * sizeof(gw_device_t));
    portEXIT_CRITICAL(&s_lock);
    return count;
}
