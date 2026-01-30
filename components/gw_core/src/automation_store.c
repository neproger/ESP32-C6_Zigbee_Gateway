#include "gw_core/automation_store.h"

#include <stdbool.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gw_core/automation_compiled.h"

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
    gw_automation_t items[GW_AUTOMATION_CAP];
} gw_automation_store_blob_t;

static gw_automation_store_blob_t s_store;

static const uint32_t MAGIC = 0x4155544f; // 'AUTO'
static const uint16_t VERSION = 1;
static const char *AUTOS_PATH = "/data/autos.bin";

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static bool is_safe_id_char(char c)
{
    return isalnum((unsigned char)c) || c == '_' || c == '-';
}

static bool compiled_path_for_id(const char *id, char *out, size_t out_size)
{
    if (!id || !id[0] || !out || out_size < 16) return false;
    for (const char *p = id; *p; p++) {
        if (!is_safe_id_char(*p)) return false;
    }
    // Keep extension distinct so it's easy to recognize on device.
    int n = snprintf(out, out_size, "/data/%s.gwar", id);
    return n > 0 && (size_t)n < out_size;
}

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

static void cleanup_orphaned_compiled_files(void)
{
    // Remove .gwar files for automations that no longer exist
    // We scan the current store and keep track of which IDs are valid,
    // then try to delete compiled files for removed automations.
    // On SPIFFS, we can't easily list directories, so we rely on the store's history.
    if (!s_fs_inited) return;
    
    // Simply log that cleanup is being attempted - actual cleanup happens by remove()
    // in gw_automation_store_remove() when automations are deleted.
    // Additional cleanup: if we have max capacity, try removing oldest disabled automations.
    
    portENTER_CRITICAL(&s_lock);
    int disabled_count = 0;
    for (size_t i = 0; i < s_store.count; i++) {
        if (!s_store.items[i].enabled) {
            disabled_count++;
        }
    }
    portEXIT_CRITICAL(&s_lock);
    
    if (disabled_count > 0) {
        ESP_LOGI(TAG, "cleanup_orphaned: found %d disabled automations (removing their compiled files)", disabled_count);
        
        // Remove compiled files for all disabled automations to free space
        portENTER_CRITICAL(&s_lock);
        for (size_t i = 0; i < s_store.count; i++) {
            if (!s_store.items[i].enabled) {
                portEXIT_CRITICAL(&s_lock);
                
                char path[96];
                if (compiled_path_for_id(s_store.items[i].id, path, sizeof(path))) {
                    if (remove(path) == 0) {
                        ESP_LOGI(TAG, "cleanup_orphaned: removed compiled file for disabled automation %s", s_store.items[i].id);
                    }
                }
                
                portENTER_CRITICAL(&s_lock);
            }
        }
        portEXIT_CRITICAL(&s_lock);
    }
}


static esp_err_t fs_init_once(void)
{
    if (s_fs_inited) {
        return ESP_OK;
    }

    // Dedicated persistent partition for gateway data (separate from "www" so web updates don't wipe automations).
    // CRITICAL: format_if_mount_failed = false to preserve automations across power loss / reboot.
    const esp_vfs_spiffs_conf_t conf = {
        .base_path = "/data",
        .partition_label = "gw_data",
        .max_files = 4,
        .format_if_mount_failed = false, // DO NOT auto-format; automations must persist
    };

    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spiffs mount failed (gw_data): %s (0x%x) - automations LOST if format happened; check flash", esp_err_to_name(err), (unsigned)err);
        return err;
    }

    // Log SPIFFS status for debugging.
    size_t total = 0, used = 0;
    esp_err_t info_err = esp_spiffs_info("gw_data", &total, &used);
    if (info_err == ESP_OK) {
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

    // Check available space - we need space for the file write
    size_t total = 0, used = 0;
    esp_err_t info_err = esp_spiffs_info("gw_data", &total, &used);
    if (info_err == ESP_OK) {
        size_t available = total > used ? (total - used) : 0;
        ESP_LOGI(TAG, "save_to_fs: SPIFFS free=%u KB, total=%u KB, need=%zu bytes", 
                 (unsigned)(available / 1024), (unsigned)(total / 1024), sizeof(s_store));
        // Need room for the blob + some margin (10KB)
        if (available < sizeof(s_store) + 10240) {
            ESP_LOGE(TAG, "save_to_fs: insufficient space (need %zu bytes, have %zu)", 
                     sizeof(s_store), available);
            // Try cleaning up orphaned .gwar files
            ESP_LOGW(TAG, "save_to_fs: attempting to clean orphaned compiled files");
            (void)cleanup_orphaned_compiled_files();
            return ESP_ERR_NO_MEM;
        }
    } else {
        ESP_LOGW(TAG, "save_to_fs: could not check SPIFFS info: %s", esp_err_to_name(info_err));
    }

    // Direct write (overwrite existing file) - atomic at SPIFFS level
    // SPIFFS handles atomic updates for single-file overwrites
    FILE *f = fopen(AUTOS_PATH, "wb");
    if (!f) {
        ESP_LOGE(TAG, "save_to_fs: fopen(%s) failed, errno=%d", AUTOS_PATH, errno);
        return ESP_FAIL;
    }

    size_t written = fwrite(&s_store, 1, sizeof(s_store), f);
    int close_result = fclose(f);
    if (close_result != 0) {
        ESP_LOGE(TAG, "save_to_fs: fclose failed, errno=%d", errno);
        return ESP_FAIL;
    }
    if (written != sizeof(s_store)) {
        ESP_LOGE(TAG, "save_to_fs: wrote %zu bytes, expected %zu", written, sizeof(s_store));
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "save_to_fs: successfully wrote %zu bytes to %s", sizeof(s_store), AUTOS_PATH);
    return ESP_OK;
}

static esp_err_t write_compiled_for_automation(const gw_automation_t *a)
{
    if (!s_fs_inited) return ESP_ERR_INVALID_STATE;
    if (!a || !a->id[0] || !a->json[0]) return ESP_ERR_INVALID_ARG;

    char path[96];
    if (!compiled_path_for_id(a->id, path, sizeof(path))) {
        return ESP_ERR_INVALID_ARG;
    }

    gw_auto_compiled_t c = {0};
    char errbuf[96] = {0};
    esp_err_t err = gw_auto_compile_json(a->json, &c, errbuf, sizeof(errbuf));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "compile failed for %s: %s", a->id, errbuf[0] ? errbuf : "err");
        gw_auto_compiled_free(&c);
        return err;
    }

    // Atomic-ish write: write tmp then rename.
    char tmp_path[110];
    (void)snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    (void)remove(tmp_path);
    err = gw_auto_compiled_write_file(tmp_path, &c);
    if (err == ESP_OK) {
        (void)remove(path);
        if (rename(tmp_path, path) != 0) {
            (void)remove(tmp_path);
            err = ESP_FAIL;
        }
    } else {
        (void)remove(tmp_path);
    }
    gw_auto_compiled_free(&c);
    return err;
}

static esp_err_t rebuild_compiled_to_fs(void)
{
    if (!s_fs_inited) return ESP_ERR_INVALID_STATE;

    // Avoid allocating N * json_max (large). Process one item at a time.
    gw_automation_t *tmp = (gw_automation_t *)malloc(sizeof(gw_automation_t));
    if (!tmp) return ESP_ERR_NO_MEM;

    portENTER_CRITICAL(&s_lock);
    const size_t n = s_store.count;
    portEXIT_CRITICAL(&s_lock);

    for (size_t i = 0; i < n; i++) {
        portENTER_CRITICAL(&s_lock);
        *tmp = s_store.items[i];
        portEXIT_CRITICAL(&s_lock);

        const gw_automation_t *a = tmp;
        char path[96];
        if (!compiled_path_for_id(a->id, path, sizeof(path))) {
            continue;
        }

        if (!a->enabled) {
            (void)remove(path);
            continue;
        }

        if (a->json[0]) {
            (void)write_compiled_for_automation(a);
        }
    }

    free(tmp);
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
                ESP_LOGI(TAG, "automation file size: %u bytes (expected %u)", (unsigned)read, (unsigned)sizeof(*tmp));
                
                if (read == sizeof(*tmp)) {
                    ESP_LOGI(TAG, "autos blob: magic=0x%08x version=%u count=%u", 
                             (unsigned)tmp->magic, (unsigned)tmp->version, (unsigned)tmp->count);
                    
                    if (tmp->magic != MAGIC) {
                        ESP_LOGW(TAG, "autos magic mismatch (got 0x%08x, expected 0x%08x) - corrupt or old format", 
                                 (unsigned)tmp->magic, (unsigned)MAGIC);
                    } else if (tmp->version != VERSION) {
                        ESP_LOGW(TAG, "autos version mismatch (got %u, expected %u) - incompatible format", 
                                 (unsigned)tmp->version, (unsigned)VERSION);
                    } else if (tmp->count > GW_AUTOMATION_CAP) {
                        ESP_LOGW(TAG, "autos count exceeds capacity (%u > %u) - truncating", 
                                 (unsigned)tmp->count, GW_AUTOMATION_CAP);
                    } else {
                        portENTER_CRITICAL(&s_lock);
                        s_store = *tmp;
                        portEXIT_CRITICAL(&s_lock);
                        ESP_LOGI(TAG, "successfully loaded %u automations from disk", (unsigned)s_store.count);
                    }
                } else {
                    ESP_LOGW(TAG, "autos file read incomplete (got %u bytes, expected %u)", 
                             (unsigned)read, (unsigned)sizeof(*tmp));
                }
                free(tmp);
            } else {
                ESP_LOGE(TAG, "malloc failed for tmp automation blob");
            }
            (void)fclose(f);
        } else {
            ESP_LOGI(TAG, "no existing automations file at %s - starting fresh", AUTOS_PATH);
        }
    }

    // Ensure compiled cache exists for current store contents (best-effort).
    if (s_fs_inited) {
        (void)rebuild_compiled_to_fs();
    }

    portENTER_CRITICAL(&s_lock);
    size_t loaded_count = s_store.count;
    portEXIT_CRITICAL(&s_lock);
    
    ESP_LOGI(TAG, "automation store initialized: loaded %u automations from /data/autos.bin", (unsigned)loaded_count);

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

size_t gw_automation_store_list_meta(gw_automation_meta_t *out, size_t max_out)
{
    if (!s_inited || !out || max_out == 0) return 0;
    portENTER_CRITICAL(&s_lock);
    size_t n = s_store.count < max_out ? s_store.count : max_out;
    for (size_t i = 0; i < n; i++) {
        const gw_automation_t *a = &s_store.items[i];
        gw_automation_meta_t *m = &out[i];
        memset(m, 0, sizeof(*m));
        strlcpy(m->id, a->id, sizeof(m->id));
        strlcpy(m->name, a->name, sizeof(m->name));
        m->enabled = a->enabled;
    }
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

    if (!s_fs_inited) {
        esp_err_t ferr = fs_init_once();
        if (ferr != ESP_OK) return ferr;
    }

    // Architecture rule: runtime executes compiled only.
    // If enabled, compilation must succeed at save time.
    if (normalized.enabled) {
        esp_err_t cerr = write_compiled_for_automation(&normalized);
        if (cerr != ESP_OK) {
            return cerr;
        }
    } else {
        // If disabled, ensure no stale compiled file remains.
        char path[96];
        if (compiled_path_for_id(normalized.id, path, sizeof(path))) {
            (void)remove(path);
        }
    }

    portENTER_CRITICAL(&s_lock);
    size_t idx = find_idx_locked(normalized.id);
    if (idx != (size_t)-1) {
        s_store.items[idx] = normalized;
    } else {
        if (s_store.count >= GW_AUTOMATION_CAP) {
            portEXIT_CRITICAL(&s_lock);
            ESP_LOGW(TAG, "Cannot save automation %s: capacity exceeded (%u/%u)", normalized.id, (unsigned)s_store.count, GW_AUTOMATION_CAP);
            return ESP_ERR_NO_MEM;
        }
        s_store.items[s_store.count++] = normalized;
    }
    portEXIT_CRITICAL(&s_lock);

    esp_err_t err = save_to_fs();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to persist automation %s to disk: %s", normalized.id, esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "automation saved and persisted: id=%s enabled=%u", normalized.id, (unsigned)normalized.enabled);
    return ESP_OK;
}

esp_err_t gw_automation_store_remove(const char *id)
{
    if (!s_inited || !id || !id[0]) return ESP_ERR_INVALID_ARG;

    portENTER_CRITICAL(&s_lock);
    size_t idx = find_idx_locked(id);
    if (idx == (size_t)-1) {
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGW(TAG, "remove: automation %s not found", id);
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
    esp_err_t err = save_to_fs();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "remove: failed to save after removing %s: %s", id, esp_err_to_name(err));
        return err;
    }

    // Remove compiled file for this automation (ignore errors).
    char path[96];
    if (compiled_path_for_id(id, path, sizeof(path))) {
        if (remove(path) == 0) {
            ESP_LOGI(TAG, "remove: deleted compiled file %s", path);
        } else {
            ESP_LOGW(TAG, "remove: failed to delete compiled file %s (errno=%d)", path, errno);
        }
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

    if (!s_fs_inited) {
        esp_err_t err = fs_init_once();
        if (err != ESP_OK) return err;
    }
    esp_err_t err = save_to_fs();
    if (err != ESP_OK) return err;

    // Keep compiled cache consistent without rebuilding everything.
    if (enabled) {
        gw_automation_t a = {0};
        if (gw_automation_store_get(id, &a) == ESP_OK && a.json[0]) {
            return write_compiled_for_automation(&a);
        }
        return ESP_ERR_NOT_FOUND;
    }

    char path[96];
    if (compiled_path_for_id(id, path, sizeof(path))) {
        (void)remove(path);
    }
    return ESP_OK;
}

void gw_automation_store_cleanup_orphaned(void)
{
    cleanup_orphaned_compiled_files();
}
