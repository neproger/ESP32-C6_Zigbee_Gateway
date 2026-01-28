#include "gw_wifi.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_wifi.h"

#include "gw_core/event_bus.h"
#include "gw_http/gw_http.h"

#if defined(__has_include) && __has_include("wifi_aps_config.h")
#include "wifi_aps_config.h"
#else
#include <stddef.h>
typedef struct
{
    const char *ssid;
    const char *password;
} gw_wifi_ap_credential_t;
static const gw_wifi_ap_credential_t *GW_WIFI_APS = NULL;
static const size_t GW_WIFI_APS_COUNT = 0;
#endif

static const char *TAG = "gw_wifi";

#define GW_WIFI_CONNECTED_BIT BIT0
#define GW_WIFI_FAIL_BIT      BIT1

typedef struct
{
    EventGroupHandle_t event_group;
    int max_retries;
    int retries;
} gw_wifi_ctx_t;

static gw_wifi_ctx_t s_ctx;
static esp_netif_t *s_netif_sta;
static bool s_wifi_started;

static void gw_wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *disc = (const wifi_event_sta_disconnected_t *)event_data;
        char msg[96];
        if (s_ctx.retries < s_ctx.max_retries) {
            s_ctx.retries++;
            ESP_LOGW(TAG, "Wi-Fi disconnected, retry %d/%d", s_ctx.retries, s_ctx.max_retries);
            (void)snprintf(msg, sizeof(msg), "disconnected: reason=%u retry %d/%d", disc ? (unsigned)disc->reason : 0U, s_ctx.retries, s_ctx.max_retries);
            gw_event_bus_publish("wifi_disconnected", "wifi", "", 0, msg);
            esp_wifi_connect();
        } else {
            (void)snprintf(msg, sizeof(msg), "disconnected: reason=%u retries exhausted", disc ? (unsigned)disc->reason : 0U);
            gw_event_bus_publish("wifi_connect_failed", "wifi", "", 0, msg);
            xEventGroupSetBits(s_ctx.event_group, GW_WIFI_FAIL_BIT);
        }
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
        s_ctx.retries = 0;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));

        uint16_t port = gw_http_get_port();
        {
            char msg[128];
            if (port == 0 || port == 80) {
                (void)snprintf(msg, sizeof(msg), "ip=" IPSTR " web=http://" IPSTR "/", IP2STR(&event->ip_info.ip), IP2STR(&event->ip_info.ip));
            } else {
                (void)snprintf(msg, sizeof(msg), "ip=" IPSTR " web=http://" IPSTR ":%u/", IP2STR(&event->ip_info.ip), IP2STR(&event->ip_info.ip), (unsigned)port);
            }
            gw_event_bus_publish("wifi_got_ip", "wifi", "", 0, msg);
        }
        if (port == 0 || port == 80) {
            ESP_LOGI(TAG, "Web UI: http://" IPSTR "/", IP2STR(&event->ip_info.ip));
        } else {
            ESP_LOGI(TAG, "Web UI: http://" IPSTR ":%u/", IP2STR(&event->ip_info.ip), (unsigned)port);
        }

        xEventGroupSetBits(s_ctx.event_group, GW_WIFI_CONNECTED_BIT);
        return;
    }
}

static esp_err_t gw_wifi_init_once(void)
{
    if (s_ctx.event_group == NULL) {
        s_ctx.event_group = xEventGroupCreate();
        ESP_RETURN_ON_FALSE(s_ctx.event_group != NULL, ESP_ERR_NO_MEM, TAG, "Failed to create event group");
    }

    if (s_netif_sta == NULL) {
        s_netif_sta = esp_netif_create_default_wifi_sta();
        ESP_RETURN_ON_FALSE(s_netif_sta != NULL, ESP_FAIL, TAG, "Failed to create default Wi-Fi STA");
    }

    static bool s_handlers_registered;
    if (!s_handlers_registered) {
        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &gw_wifi_event_handler, NULL));
        ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &gw_wifi_event_handler, NULL));
        s_handlers_registered = true;
    }

    static bool s_wifi_inited;
    if (!s_wifi_inited) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "esp_wifi_init failed");
        ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "esp_wifi_set_storage failed");
        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "esp_wifi_set_mode failed");
        s_wifi_inited = true;
    }

    return ESP_OK;
}

typedef struct
{
    size_t ap_index;
    int rssi;
} gw_wifi_candidate_t;

static bool gw_wifi_ssid_equals_record(const char *ssid, const uint8_t record_ssid[32])
{
    if (ssid == NULL || record_ssid == NULL) {
        return false;
    }
    return strncmp(ssid, (const char *)record_ssid, 32) == 0;
}

static int gw_wifi_candidate_cmp_desc_rssi(const void *a, const void *b)
{
    const gw_wifi_candidate_t *ca = (const gw_wifi_candidate_t *)a;
    const gw_wifi_candidate_t *cb = (const gw_wifi_candidate_t *)b;
    return (cb->rssi - ca->rssi);
}

static void gw_wifi_log_scan_results(const wifi_ap_record_t *records, uint16_t count)
{
    if (records == NULL || count == 0) {
        return;
    }

    const uint16_t max_dump = (count > 50) ? 50 : count;
    ESP_LOGI(TAG, "Scan results (%u total, dumping %u):", (unsigned)count, (unsigned)max_dump);
    for (uint16_t i = 0; i < max_dump; i++) {
        char ssid[33];
        memcpy(ssid, records[i].ssid, 32);
        ssid[32] = '\0';
        ESP_LOGI(TAG, "  [%u] rssi=%d chan=%u ssid=\"%s\"", (unsigned)i, records[i].rssi, (unsigned)records[i].primary, ssid);
    }

    if (count > max_dump) {
        ESP_LOGI(TAG, "  ... (%u more)", (unsigned)(count - max_dump));
    }
}

static esp_err_t gw_wifi_scan_build_candidates(gw_wifi_candidate_t *candidates, size_t candidates_cap, size_t *out_count)
{
    ESP_RETURN_ON_FALSE(candidates != NULL && out_count != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid args");
    *out_count = 0;

    wifi_scan_config_t scan_cfg = {0};
    scan_cfg.show_hidden = true;

    ESP_RETURN_ON_ERROR(esp_wifi_scan_start(&scan_cfg, true), TAG, "esp_wifi_scan_start failed");

    uint16_t ap_num = 0;
    ESP_RETURN_ON_ERROR(esp_wifi_scan_get_ap_num(&ap_num), TAG, "esp_wifi_scan_get_ap_num failed");
    if (ap_num == 0) {
        ESP_LOGW(TAG, "Scan found 0 APs");
        gw_event_bus_publish("wifi_scan", "wifi", "", 0, "scan found 0 APs");
        return ESP_OK;
    }

    uint16_t fetch_num = ap_num;
    if (fetch_num > 200) {
        fetch_num = 200;
    }

    wifi_ap_record_t *records = (wifi_ap_record_t *)calloc(fetch_num, sizeof(wifi_ap_record_t));
    ESP_RETURN_ON_FALSE(records != NULL, ESP_ERR_NO_MEM, TAG, "No mem for scan results");

    esp_err_t err = esp_wifi_scan_get_ap_records(&fetch_num, records);
    if (err != ESP_OK) {
        free(records);
        ESP_RETURN_ON_ERROR(err, TAG, "esp_wifi_scan_get_ap_records failed");
    }

    // For each known AP, keep the best RSSI found in scan results.
    for (size_t i = 0; i < GW_WIFI_APS_COUNT && *out_count < candidates_cap; i++) {
        const gw_wifi_ap_credential_t *ap = &GW_WIFI_APS[i];
        if (ap->ssid == NULL || ap->ssid[0] == '\0') {
            continue;
        }

        int best_rssi = -1000;
        bool found = false;
        for (uint16_t r = 0; r < fetch_num; r++) {
            if (gw_wifi_ssid_equals_record(ap->ssid, records[r].ssid)) {
                found = true;
                if (records[r].rssi > best_rssi) {
                    best_rssi = records[r].rssi;
                }
            }
        }

        if (found) {
            candidates[*out_count].ap_index = i;
            candidates[*out_count].rssi = best_rssi;
            (*out_count)++;
        }
    }

    if (*out_count == 0) {
        ESP_LOGW(TAG, "No known SSIDs found in scan results (configured=%u, scanned=%u, fetched=%u)",
                 (unsigned)GW_WIFI_APS_COUNT, (unsigned)ap_num, (unsigned)fetch_num);
        gw_wifi_log_scan_results(records, fetch_num);
        gw_event_bus_publish("wifi_scan", "wifi", "", 0, "no known SSIDs found in scan results");
        free(records);
        return ESP_OK;
    }

    free(records);

    qsort(candidates, *out_count, sizeof(candidates[0]), gw_wifi_candidate_cmp_desc_rssi);
    return ESP_OK;
}

static esp_err_t gw_wifi_try_connect_one(const gw_wifi_ap_credential_t *ap, int max_retries, TickType_t timeout_ticks)
{
    ESP_RETURN_ON_FALSE(ap != NULL && ap->ssid != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid AP entry");

    wifi_config_t wifi_cfg = {0};
    strlcpy((char *)wifi_cfg.sta.ssid, ap->ssid, sizeof(wifi_cfg.sta.ssid));
    if (ap->password != NULL) {
        strlcpy((char *)wifi_cfg.sta.password, ap->password, sizeof(wifi_cfg.sta.password));
    }
    wifi_cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wifi_cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

    s_ctx.max_retries = max_retries;
    s_ctx.retries = 0;
    xEventGroupClearBits(s_ctx.event_group, GW_WIFI_CONNECTED_BIT | GW_WIFI_FAIL_BIT);

    ESP_LOGI(TAG, "Connecting to SSID: %s", ap->ssid);
    {
        char msg[96];
        (void)snprintf(msg, sizeof(msg), "ssid=%s", ap->ssid);
        gw_event_bus_publish("wifi_connecting", "wifi", "", 0, msg);
    }

    if (s_wifi_started) {
        (void)esp_wifi_disconnect();
    }
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg), TAG, "esp_wifi_set_config failed");

    if (!s_wifi_started) {
        ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start failed");
        s_wifi_started = true;
    }

    esp_err_t err = esp_wifi_connect();
    if (err == ESP_ERR_WIFI_STATE) {
        // Already connecting/connected; force a clean connect attempt for the new SSID.
        (void)esp_wifi_disconnect();
        err = esp_wifi_connect();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "esp_wifi_connect failed");

    EventBits_t bits = xEventGroupWaitBits(
        s_ctx.event_group,
        GW_WIFI_CONNECTED_BIT | GW_WIFI_FAIL_BIT,
        pdFALSE,
        pdFALSE,
        timeout_ticks);

    if (bits & GW_WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to %s", ap->ssid);
        {
            char msg[96];
            (void)snprintf(msg, sizeof(msg), "ssid=%s", ap->ssid);
            gw_event_bus_publish("wifi_connected", "wifi", "", 0, msg);
        }
        return ESP_OK;
    }

    ESP_LOGW(TAG, "Failed to connect to %s", ap->ssid);
    {
        char msg[96];
        (void)snprintf(msg, sizeof(msg), "ssid=%s", ap->ssid);
        gw_event_bus_publish("wifi_connect_failed", "wifi", "", 0, msg);
    }
    return ESP_FAIL;
}

esp_err_t gw_wifi_connect_multi(void)
{
    ESP_RETURN_ON_ERROR(gw_wifi_init_once(), TAG, "Wi-Fi init failed");

    ESP_RETURN_ON_FALSE(GW_WIFI_APS_COUNT > 0, ESP_ERR_INVALID_STATE, TAG, "No APs configured in wifi_aps_config.h");

    if (!s_wifi_started) {
        ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start failed");
        s_wifi_started = true;
    }

    gw_wifi_candidate_t candidates[16];
    size_t candidates_count = 0;
    ESP_RETURN_ON_ERROR(gw_wifi_scan_build_candidates(candidates, sizeof(candidates) / sizeof(candidates[0]), &candidates_count),
                        TAG,
                        "Wi-Fi scan failed");

    const int max_retries_per_ap = 3;
    const TickType_t timeout_ticks = pdMS_TO_TICKS(20 * 1000);

    for (size_t i = 0; i < candidates_count; i++) {
        const gw_wifi_ap_credential_t *ap = &GW_WIFI_APS[candidates[i].ap_index];
        ESP_LOGI(TAG, "Candidate: %s (rssi %d)", ap->ssid, candidates[i].rssi);

        esp_err_t err = gw_wifi_try_connect_one(ap, max_retries_per_ap, timeout_ticks);
        if (err == ESP_OK) {
            return ESP_OK;
        }
    }

    return ESP_FAIL;
}
