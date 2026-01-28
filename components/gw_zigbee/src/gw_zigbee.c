#include "gw_zigbee/gw_zigbee.h"

#include <inttypes.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "esp_zigbee_core.h"

#include "gw_core/device_registry.h"

static const char *TAG = "gw_zigbee";

static void ieee_to_uid_str(const uint8_t ieee_addr[8], char out[GW_DEVICE_UID_STRLEN])
{
    // Format: "0x00124B0012345678" + '\0' => 18 + 1 = 19
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v = (v << 8) | (uint64_t)ieee_addr[i];
    }
    (void)snprintf(out, GW_DEVICE_UID_STRLEN, "0x%016" PRIx64, v);
}

void gw_zigbee_on_device_annce(const uint8_t ieee_addr[8], uint16_t short_addr, uint8_t capability)
{
    if (ieee_addr == NULL) {
        return;
    }

    gw_device_t d = {0};
    ieee_to_uid_str(ieee_addr, d.device_uid.uid);
    d.short_addr = short_addr;
    d.last_seen_ms = (uint64_t)(esp_timer_get_time() / 1000);
    // Capabilities are application-level; keep defaults for now.
    // Name can be set later via API/UI once we have persistence.
    d.name[0] = '\0';
    d.has_onoff = false;
    d.has_button = false;

    esp_err_t err = gw_device_registry_upsert(&d);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "registry upsert failed for %s: %s", d.device_uid.uid, esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "Device announced: %s short=0x%04x cap=0x%02x", d.device_uid.uid, (unsigned)d.short_addr, (unsigned)capability);
}

static void permit_join_cb(uint8_t seconds)
{
    esp_err_t err = esp_zb_bdb_open_network(seconds);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_zb_bdb_open_network(%u) failed: %s", (unsigned)seconds, esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "permit_join enabled for %u seconds", (unsigned)seconds);
}

esp_err_t gw_zigbee_permit_join(uint8_t seconds)
{
    if (seconds == 0) {
        seconds = 180;
    }

    // Schedule into Zigbee context.
    esp_zb_scheduler_alarm(permit_join_cb, seconds, 0);
    return ESP_OK;
}
