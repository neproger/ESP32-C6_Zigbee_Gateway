#include "gw_core/device_registry.h"

#include <ctype.h>
#include <stdio.h>
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

static bool is_prefix_number_name(const char *name, const char *prefix, uint32_t *out_n)
{
    if (!name || !prefix || !prefix[0]) {
        return false;
    }

    const size_t pre_len = strlen(prefix);
    if (strncmp(name, prefix, pre_len) != 0) {
        return false;
    }

    const char *p = name + pre_len;
    if (*p == '\0') {
        return false;
    }

    uint32_t n = 0;
    while (*p) {
        if (!isdigit((unsigned char)*p)) {
            return false;
        }
        n = (n * 10u) + (uint32_t)(*p - '0');
        p++;
    }

    if (n == 0) {
        return false;
    }
    if (out_n) {
        *out_n = n;
    }
    return true;
}

static uint32_t next_name_index_for_prefix(const char *prefix)
{
    uint32_t max_n = 0;
    for (size_t i = 0; i < s_device_count; i++) {
        uint32_t n = 0;
        if (is_prefix_number_name(s_devices[i].name, prefix, &n)) {
            if (n > max_n) {
                max_n = n;
            }
        }
    }
    return max_n + 1;
}

static const char *pick_default_prefix(const gw_device_t *d)
{
    if (d && d->has_button) {
        return "switch";
    }
    if (d && d->has_onoff) {
        return "relay";
    }
    return "device";
}

static void assign_default_name_if_needed(gw_device_t *d)
{
    if (!d) {
        return;
    }

    // If we already have a user-provided name, keep it.
    if (d->name[0] != '\0') {
        // Optionally upgrade auto-generic deviceN -> relayN/switchN once capabilities are known.
        uint32_t n = 0;
        if ((d->has_button || d->has_onoff) && is_prefix_number_name(d->name, "device", &n)) {
            const char *prefix = pick_default_prefix(d);
            if (strcmp(prefix, "device") != 0) {
                uint32_t next = next_name_index_for_prefix(prefix);
                (void)snprintf(d->name, sizeof(d->name), "%s%u", prefix, (unsigned)next);
            }
        }
        return;
    }

    const char *prefix = pick_default_prefix(d);
    uint32_t next = next_name_index_for_prefix(prefix);
    (void)snprintf(d->name, sizeof(d->name), "%s%u", prefix, (unsigned)next);
}

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
        gw_device_t tmp = *device;
        if (tmp.name[0] == '\0') {
            // Preserve existing name unless caller explicitly sets it.
            strlcpy(tmp.name, s_devices[idx].name, sizeof(tmp.name));
        }

        // Assign a default name if missing (or upgrade generic deviceN to a typed name).
        assign_default_name_if_needed(&tmp);
        s_devices[idx] = tmp;
        portEXIT_CRITICAL(&s_lock);
        return save_to_nvs();
    }

    if (s_device_count >= (sizeof(s_devices) / sizeof(s_devices[0]))) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_NO_MEM;
    }

    gw_device_t tmp = *device;
    assign_default_name_if_needed(&tmp);
    s_devices[s_device_count++] = tmp;
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
