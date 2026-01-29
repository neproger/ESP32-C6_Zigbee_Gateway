#include "gw_zigbee/gw_zigbee.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#include "esp_zigbee_core.h"
#include "zcl/esp_zigbee_zcl_common.h"
#include "zcl/esp_zigbee_zcl_command.h"
#include "zcl/esp_zigbee_zcl_humidity_meas.h"
#include "zcl/esp_zigbee_zcl_power_config.h"
#include "zcl/esp_zigbee_zcl_temperature_meas.h"
#include "zdo/esp_zigbee_zdo_command.h"
#include "zdo/esp_zigbee_zdo_common.h"

#include "gw_core/event_bus.h"
#include "gw_core/device_registry.h"
#include "gw_core/zb_model.h"

static const char *TAG = "gw_zigbee";

// Keep in sync with main/esp_zigbee_gateway.h (ESP_ZB_GATEWAY_ENDPOINT).
#define GW_ZIGBEE_GATEWAY_ENDPOINT 1

// Fixed groups by device "type". Can be extended/configured later via UI.
#define GW_ZIGBEE_GROUP_SWITCHES 0x0002
#define GW_ZIGBEE_GROUP_LIGHTS   0x0003

static const int16_t s_report_change_temp_01c = 10;    // 0.10°C (temp is 0.01°C units)
static const uint16_t s_report_change_hum_01pct = 100; // 1.00%RH (humidity is 0.01% units)
static const uint8_t s_report_change_batt_halfpct = 2; // 1% (battery is 0.5% units)

static void ieee_to_uid_str(const uint8_t ieee_addr[8], char out[GW_DEVICE_UID_STRLEN])
{
    // Format: "0x00124B0012345678" + '\0' => 18 + 1 = 19
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v = (v << 8) | (uint64_t)ieee_addr[i];
    }
    (void)snprintf(out, GW_DEVICE_UID_STRLEN, "0x%016" PRIx64, v);
}

static bool cluster_list_has(const uint16_t *list, uint8_t count, uint16_t cluster_id)
{
    if (list == NULL || count == 0) {
        return false;
    }
    for (uint8_t i = 0; i < count; i++) {
        if (list[i] == cluster_id) {
            return true;
        }
    }
    return false;
}

typedef struct {
    esp_zb_ieee_addr_t ieee;
    uint16_t short_addr;
} gw_zb_discover_ctx_t;

typedef struct {
    esp_zb_ieee_addr_t ieee;
    uint16_t short_addr;
    uint8_t endpoint;
} gw_zb_simple_ctx_t;

typedef struct {
    gw_device_uid_t uid;
    uint16_t short_addr;
    uint8_t src_ep;
} gw_zb_bind_ctx_t;

static void bind_resp_cb(esp_zb_zdp_status_t zdo_status, void *user_ctx)
{
    gw_zb_bind_ctx_t *ctx = (gw_zb_bind_ctx_t *)user_ctx;
    if (ctx == NULL) {
        return;
    }

    char msg[64];
    (void)snprintf(msg, sizeof(msg), "status=0x%02x src_ep=%u", (unsigned)zdo_status, (unsigned)ctx->src_ep);
    gw_event_bus_publish((zdo_status == ESP_ZB_ZDP_STATUS_SUCCESS) ? "zigbee_bind_ok" : "zigbee_bind_failed",
                         "zigbee",
                         ctx->uid.uid,
                         ctx->short_addr,
                         msg);

    free(ctx);
}

static void simple_desc_cb(esp_zb_zdp_status_t zdo_status, esp_zb_af_simple_desc_1_1_t *simple_desc, void *user_ctx)
{
    gw_zb_simple_ctx_t *ctx = (gw_zb_simple_ctx_t *)user_ctx;
    if (ctx == NULL) {
        return;
    }

    if (zdo_status != ESP_ZB_ZDP_STATUS_SUCCESS || simple_desc == NULL || simple_desc->app_cluster_list == NULL) {
        gw_event_bus_publish("zigbee_simple_desc_failed", "zigbee", "", ctx->short_addr, "simple desc request failed");
        free(ctx);
        return;
    }

    const uint16_t *in_clusters = &simple_desc->app_cluster_list[0];
    const uint16_t *out_clusters = &simple_desc->app_cluster_list[simple_desc->app_input_cluster_count];

    bool has_groups_srv = cluster_list_has(in_clusters, simple_desc->app_input_cluster_count, ESP_ZB_ZCL_CLUSTER_ID_GROUPS);
    bool has_onoff_srv = cluster_list_has(in_clusters, simple_desc->app_input_cluster_count, ESP_ZB_ZCL_CLUSTER_ID_ON_OFF);
    bool has_onoff_cli = cluster_list_has(out_clusters, simple_desc->app_output_cluster_count, ESP_ZB_ZCL_CLUSTER_ID_ON_OFF);

    const bool is_switch = has_onoff_cli;
    const bool is_light = (!is_switch && has_onoff_srv);

    char uid[GW_DEVICE_UID_STRLEN];
    ieee_to_uid_str(ctx->ieee, uid);

    // Store the discovered endpoint model for UI/debugging.
    {
        gw_zb_endpoint_t ep = {0};
        strlcpy(ep.uid.uid, uid, sizeof(ep.uid.uid));
        ep.short_addr = ctx->short_addr;
        ep.endpoint = simple_desc->endpoint;
        ep.profile_id = simple_desc->app_profile_id;
        ep.device_id = simple_desc->app_device_id;
        ep.in_cluster_count = (simple_desc->app_input_cluster_count > GW_ZB_MAX_CLUSTERS) ? GW_ZB_MAX_CLUSTERS : simple_desc->app_input_cluster_count;
        ep.out_cluster_count = (simple_desc->app_output_cluster_count > GW_ZB_MAX_CLUSTERS) ? GW_ZB_MAX_CLUSTERS : simple_desc->app_output_cluster_count;
        memcpy(ep.in_clusters, in_clusters, ep.in_cluster_count * sizeof(ep.in_clusters[0]));
        memcpy(ep.out_clusters, out_clusters, ep.out_cluster_count * sizeof(ep.out_clusters[0]));
        (void)gw_zb_model_upsert_endpoint(&ep);
    }

    char msg[128];
    (void)snprintf(msg,
                   sizeof(msg),
                   "ep=%u profile=0x%04x dev=0x%04x in=%u out=%u groups=%d onoff_srv=%d onoff_cli=%d",
                   (unsigned)simple_desc->endpoint,
                   (unsigned)simple_desc->app_profile_id,
                   (unsigned)simple_desc->app_device_id,
                   (unsigned)simple_desc->app_input_cluster_count,
                   (unsigned)simple_desc->app_output_cluster_count,
                   has_groups_srv ? 1 : 0,
                   has_onoff_srv ? 1 : 0,
                   has_onoff_cli ? 1 : 0);
    gw_event_bus_publish("zigbee_simple_desc", "zigbee", uid, ctx->short_addr, msg);

    // Update capabilities for UI.
    gw_device_uid_t duid = {0};
    strlcpy(duid.uid, uid, sizeof(duid.uid));

    gw_device_t d = {0};
    if (gw_device_registry_get(&duid, &d) == ESP_OK) {
        d.short_addr = ctx->short_addr;
        d.last_seen_ms = (uint64_t)(esp_timer_get_time() / 1000);
        if (is_switch) {
            d.has_button = true;
        }
        if (is_light) {
            d.has_onoff = true;
        }
        (void)gw_device_registry_upsert(&d);
    }

    // Auto-register into a type group if supported.
    if (has_groups_srv && (is_switch || is_light)) {
        const uint16_t group_id = is_switch ? GW_ZIGBEE_GROUP_SWITCHES : GW_ZIGBEE_GROUP_LIGHTS;

        esp_zb_zcl_groups_add_group_cmd_t cmd = {0};
        cmd.zcl_basic_cmd.dst_addr_u.addr_short = ctx->short_addr;
        cmd.zcl_basic_cmd.dst_endpoint = simple_desc->endpoint;
        cmd.zcl_basic_cmd.src_endpoint = GW_ZIGBEE_GATEWAY_ENDPOINT;
        cmd.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
        cmd.group_id = group_id;

        uint8_t tsn = esp_zb_zcl_groups_add_group_cmd_req(&cmd);
        (void)snprintf(msg, sizeof(msg), "add_group 0x%04x ep=%u tsn=%u", (unsigned)group_id, (unsigned)simple_desc->endpoint, (unsigned)tsn);
        gw_event_bus_publish("zigbee_group_add", "zigbee", uid, ctx->short_addr, msg);
    }

    // Sensors usually need reporting configured to get periodic updates.
    // We'll configure reporting for the most common attributes and do an initial read.
    const bool has_temp_meas_srv = cluster_list_has(in_clusters, simple_desc->app_input_cluster_count, ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT);
    const bool has_hum_meas_srv = cluster_list_has(in_clusters, simple_desc->app_input_cluster_count, ESP_ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT);
    const bool has_power_cfg_srv = cluster_list_has(in_clusters, simple_desc->app_input_cluster_count, ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG);

    if (has_temp_meas_srv) {
        esp_zb_zcl_config_report_record_t rec = {0};
        rec.direction = ESP_ZB_ZCL_REPORT_DIRECTION_SEND;
        rec.attributeID = ESP_ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_ID;
        rec.attrType = ESP_ZB_ZCL_ATTR_TYPE_S16;
        rec.min_interval = 5;
        rec.max_interval = 60;
        rec.reportable_change = (void *)&s_report_change_temp_01c;

        esp_zb_zcl_config_report_cmd_t cmd = {0};
        cmd.zcl_basic_cmd.dst_addr_u.addr_short = ctx->short_addr;
        cmd.zcl_basic_cmd.dst_endpoint = simple_desc->endpoint;
        cmd.zcl_basic_cmd.src_endpoint = GW_ZIGBEE_GATEWAY_ENDPOINT;
        cmd.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
        cmd.clusterID = ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT;
        cmd.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV;
        cmd.record_number = 1;
        cmd.record_field = &rec;

        uint8_t tsn = esp_zb_zcl_config_report_cmd_req(&cmd);
        (void)snprintf(msg, sizeof(msg), "config_report temp ep=%u tsn=%u", (unsigned)simple_desc->endpoint, (unsigned)tsn);
        gw_event_bus_publish("zigbee_config_report", "zigbee", uid, ctx->short_addr, msg);

        uint16_t attrs[] = {ESP_ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_ID};
        esp_zb_zcl_read_attr_cmd_t r = {0};
        r.zcl_basic_cmd.dst_addr_u.addr_short = ctx->short_addr;
        r.zcl_basic_cmd.dst_endpoint = simple_desc->endpoint;
        r.zcl_basic_cmd.src_endpoint = GW_ZIGBEE_GATEWAY_ENDPOINT;
        r.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
        r.clusterID = ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT;
        r.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV;
        r.attr_number = 1;
        r.attr_field = attrs;
        (void)esp_zb_zcl_read_attr_cmd_req(&r);
    }

    if (has_hum_meas_srv) {
        esp_zb_zcl_config_report_record_t rec = {0};
        rec.direction = ESP_ZB_ZCL_REPORT_DIRECTION_SEND;
        rec.attributeID = ESP_ZB_ZCL_ATTR_REL_HUMIDITY_MEASUREMENT_VALUE_ID;
        rec.attrType = ESP_ZB_ZCL_ATTR_TYPE_U16;
        rec.min_interval = 5;
        rec.max_interval = 60;
        rec.reportable_change = (void *)&s_report_change_hum_01pct;

        esp_zb_zcl_config_report_cmd_t cmd = {0};
        cmd.zcl_basic_cmd.dst_addr_u.addr_short = ctx->short_addr;
        cmd.zcl_basic_cmd.dst_endpoint = simple_desc->endpoint;
        cmd.zcl_basic_cmd.src_endpoint = GW_ZIGBEE_GATEWAY_ENDPOINT;
        cmd.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
        cmd.clusterID = ESP_ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT;
        cmd.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV;
        cmd.record_number = 1;
        cmd.record_field = &rec;

        uint8_t tsn = esp_zb_zcl_config_report_cmd_req(&cmd);
        (void)snprintf(msg, sizeof(msg), "config_report humidity ep=%u tsn=%u", (unsigned)simple_desc->endpoint, (unsigned)tsn);
        gw_event_bus_publish("zigbee_config_report", "zigbee", uid, ctx->short_addr, msg);

        uint16_t attrs[] = {ESP_ZB_ZCL_ATTR_REL_HUMIDITY_MEASUREMENT_VALUE_ID};
        esp_zb_zcl_read_attr_cmd_t r = {0};
        r.zcl_basic_cmd.dst_addr_u.addr_short = ctx->short_addr;
        r.zcl_basic_cmd.dst_endpoint = simple_desc->endpoint;
        r.zcl_basic_cmd.src_endpoint = GW_ZIGBEE_GATEWAY_ENDPOINT;
        r.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
        r.clusterID = ESP_ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT;
        r.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV;
        r.attr_number = 1;
        r.attr_field = attrs;
        (void)esp_zb_zcl_read_attr_cmd_req(&r);
    }

    if (has_power_cfg_srv) {
        esp_zb_zcl_config_report_record_t rec = {0};
        rec.direction = ESP_ZB_ZCL_REPORT_DIRECTION_SEND;
        rec.attributeID = ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID;
        rec.attrType = ESP_ZB_ZCL_ATTR_TYPE_U8;
        rec.min_interval = 300;
        rec.max_interval = 3600;
        rec.reportable_change = (void *)&s_report_change_batt_halfpct;

        esp_zb_zcl_config_report_cmd_t cmd = {0};
        cmd.zcl_basic_cmd.dst_addr_u.addr_short = ctx->short_addr;
        cmd.zcl_basic_cmd.dst_endpoint = simple_desc->endpoint;
        cmd.zcl_basic_cmd.src_endpoint = GW_ZIGBEE_GATEWAY_ENDPOINT;
        cmd.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
        cmd.clusterID = ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG;
        cmd.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV;
        cmd.record_number = 1;
        cmd.record_field = &rec;

        uint8_t tsn = esp_zb_zcl_config_report_cmd_req(&cmd);
        (void)snprintf(msg, sizeof(msg), "config_report battery ep=%u tsn=%u", (unsigned)simple_desc->endpoint, (unsigned)tsn);
        gw_event_bus_publish("zigbee_config_report", "zigbee", uid, ctx->short_addr, msg);

        uint16_t attrs[] = {ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID};
        esp_zb_zcl_read_attr_cmd_t r = {0};
        r.zcl_basic_cmd.dst_addr_u.addr_short = ctx->short_addr;
        r.zcl_basic_cmd.dst_endpoint = simple_desc->endpoint;
        r.zcl_basic_cmd.src_endpoint = GW_ZIGBEE_GATEWAY_ENDPOINT;
        r.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
        r.clusterID = ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG;
        r.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV;
        r.attr_number = 1;
        r.attr_field = attrs;
        (void)esp_zb_zcl_read_attr_cmd_req(&r);
    }

    // If it's a switch using APS binding table, bind its On/Off client to the gateway On/Off server.
    if (is_switch) {
        esp_zb_ieee_addr_t gw_ieee = {0};
        esp_zb_get_long_address(gw_ieee);

        gw_zb_bind_ctx_t *bctx = (gw_zb_bind_ctx_t *)calloc(1, sizeof(*bctx));
        if (bctx != NULL) {
            strlcpy(bctx->uid.uid, uid, sizeof(bctx->uid.uid));
            bctx->short_addr = ctx->short_addr;
            bctx->src_ep = simple_desc->endpoint;

            esp_zb_zdo_bind_req_param_t bind = {0};
            memcpy(bind.src_address, ctx->ieee, sizeof(bind.src_address));
            bind.src_endp = simple_desc->endpoint;
            bind.cluster_id = ESP_ZB_ZCL_CLUSTER_ID_ON_OFF;
            bind.dst_addr_mode = ESP_ZB_ZDO_BIND_DST_ADDR_MODE_64_BIT_EXTENDED;
            memcpy(bind.dst_address_u.addr_long, gw_ieee, sizeof(gw_ieee));
            bind.dst_endp = GW_ZIGBEE_GATEWAY_ENDPOINT;
            bind.req_dst_addr = ctx->short_addr;

            char msg[96];
            (void)snprintf(msg,
                           sizeof(msg),
                           "bind on_off src_ep=%u -> gw_ep=%u",
                           (unsigned)bind.src_endp,
                           (unsigned)bind.dst_endp);
            gw_event_bus_publish("zigbee_bind_requested", "zigbee", uid, ctx->short_addr, msg);

            esp_zb_zdo_device_bind_req(&bind, bind_resp_cb, bctx);
        } else {
            gw_event_bus_publish("zigbee_bind_failed", "zigbee", uid, ctx->short_addr, "no mem for bind ctx");
        }
    }

    free(ctx);
}

static void active_ep_cb(esp_zb_zdp_status_t zdo_status, uint8_t ep_count, uint8_t *ep_id_list, void *user_ctx)
{
    gw_zb_discover_ctx_t *ctx = (gw_zb_discover_ctx_t *)user_ctx;
    if (ctx == NULL) {
        return;
    }

    if (zdo_status != ESP_ZB_ZDP_STATUS_SUCCESS || ep_count == 0 || ep_id_list == NULL) {
        gw_event_bus_publish("zigbee_active_ep_failed", "zigbee", "", ctx->short_addr, "active ep request failed");
        free(ctx);
        return;
    }

    char uid[GW_DEVICE_UID_STRLEN];
    ieee_to_uid_str(ctx->ieee, uid);

    char msg[64];
    (void)snprintf(msg, sizeof(msg), "ep_count=%u", (unsigned)ep_count);
    gw_event_bus_publish("zigbee_active_ep", "zigbee", uid, ctx->short_addr, msg);

    for (uint8_t i = 0; i < ep_count; i++) {
        gw_zb_simple_ctx_t *sctx = (gw_zb_simple_ctx_t *)calloc(1, sizeof(*sctx));
        if (sctx == NULL) {
            gw_event_bus_publish("zigbee_simple_desc_failed", "zigbee", uid, ctx->short_addr, "no mem for simple ctx");
            continue;
        }
        memcpy(sctx->ieee, ctx->ieee, sizeof(sctx->ieee));
        sctx->short_addr = ctx->short_addr;
        sctx->endpoint = ep_id_list[i];

        esp_zb_zdo_simple_desc_req_param_t req = {
            .addr_of_interest = ctx->short_addr,
            .endpoint = sctx->endpoint,
        };
        esp_zb_zdo_simple_desc_req(&req, simple_desc_cb, sctx);
    }

    free(ctx);
}

static void gw_zigbee_start_discovery(const uint8_t ieee_addr[8], uint16_t short_addr)
{
    gw_zb_discover_ctx_t *ctx = (gw_zb_discover_ctx_t *)calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        gw_event_bus_publish("zigbee_discovery_failed", "zigbee", "", short_addr, "no mem for discovery ctx");
        return;
    }
    memcpy(ctx->ieee, ieee_addr, sizeof(ctx->ieee));
    ctx->short_addr = short_addr;

    esp_zb_zdo_active_ep_req_param_t req = {.addr_of_interest = short_addr};
    esp_zb_zdo_active_ep_req(&req, active_ep_cb, ctx);
}

static bool uid_str_to_ieee(const char *uid, esp_zb_ieee_addr_t out_ieee)
{
    if (uid == NULL || out_ieee == NULL) {
        return false;
    }
    if (strncmp(uid, "0x", 2) != 0 && strncmp(uid, "0X", 2) != 0) {
        return false;
    }

    char *end = NULL;
    unsigned long long v = strtoull(uid + 2, &end, 16);
    if (end == NULL || *end != '\0') {
        return false;
    }

    for (int i = 7; i >= 0; i--) {
        out_ieee[i] = (uint8_t)(v & 0xFFu);
        v >>= 8;
    }
    return true;
}

typedef struct {
    gw_device_uid_t uid;
    uint16_t short_addr;
    bool rejoin;
    esp_zb_zdo_mgmt_leave_req_param_t req;
} gw_zb_leave_ctx_t;

static gw_zb_leave_ctx_t *s_leave_ctx_by_token[256];
static uint8_t s_leave_token;
static portMUX_TYPE s_leave_lock = portMUX_INITIALIZER_UNLOCKED;

static void leave_resp_cb(esp_zb_zdp_status_t zdo_status, void *user_ctx)
{
    gw_zb_leave_ctx_t *ctx = (gw_zb_leave_ctx_t *)user_ctx;
    if (ctx == NULL) {
        return;
    }

    char msg[64];
    (void)snprintf(msg, sizeof(msg), "status=0x%02x rejoin=%u", (unsigned)zdo_status, ctx->rejoin ? 1U : 0U);
    gw_event_bus_publish((zdo_status == ESP_ZB_ZDP_STATUS_SUCCESS) ? "zigbee_leave_ok" : "zigbee_leave_failed",
                         "zigbee",
                         ctx->uid.uid,
                         ctx->short_addr,
                         msg);
    free(ctx);
}

static void leave_send_cb(uint8_t token)
{
    gw_zb_leave_ctx_t *ctx = NULL;

    portENTER_CRITICAL(&s_leave_lock);
    ctx = s_leave_ctx_by_token[token];
    s_leave_ctx_by_token[token] = NULL;
    portEXIT_CRITICAL(&s_leave_lock);

    if (ctx == NULL) {
        return;
    }

    esp_zb_zdo_device_leave_req(&ctx->req, leave_resp_cb, ctx);
}

typedef struct {
    uint16_t short_addr;
    esp_zb_zdo_ieee_addr_req_param_t req;
} gw_zb_ieee_lookup_ctx_t;

static gw_zb_ieee_lookup_ctx_t *s_ieee_ctx_by_token[256];
static uint8_t s_ieee_token;
static portMUX_TYPE s_ieee_lock = portMUX_INITIALIZER_UNLOCKED;

static bool should_throttle_discovery(uint16_t short_addr)
{
    typedef struct {
        uint16_t short_addr;
        uint64_t ts_ms;
    } slot_t;

    static slot_t s_slots[8];
    static size_t s_next;

    const uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000);
    for (size_t i = 0; i < sizeof(s_slots) / sizeof(s_slots[0]); i++) {
        if (s_slots[i].short_addr == short_addr) {
            if (now_ms - s_slots[i].ts_ms < 30 * 1000) {
                return true;
            }
            s_slots[i].ts_ms = now_ms;
            return false;
        }
    }

    s_slots[s_next].short_addr = short_addr;
    s_slots[s_next].ts_ms = now_ms;
    s_next = (s_next + 1) % (sizeof(s_slots) / sizeof(s_slots[0]));
    return false;
}

static void ieee_addr_cb(esp_zb_zdp_status_t zdo_status, esp_zb_zdo_ieee_addr_rsp_t *resp, void *user_ctx)
{
    gw_zb_ieee_lookup_ctx_t *ctx = (gw_zb_ieee_lookup_ctx_t *)user_ctx;
    if (ctx == NULL) {
        return;
    }

    if (zdo_status != ESP_ZB_ZDP_STATUS_SUCCESS || resp == NULL) {
        gw_event_bus_publish("zigbee_ieee_lookup_failed", "zigbee", "", ctx->short_addr, "ieee_addr_req failed");
        free(ctx);
        return;
    }

    char uid[GW_DEVICE_UID_STRLEN];
    ieee_to_uid_str(resp->ieee_addr, uid);

    // Ensure it's in device registry (even if DEVICE_ANNCE was missed).
    gw_device_t d = {0};
    strlcpy(d.device_uid.uid, uid, sizeof(d.device_uid.uid));
    d.short_addr = resp->nwk_addr;
    d.last_seen_ms = (uint64_t)(esp_timer_get_time() / 1000);
    (void)gw_device_registry_upsert(&d);

    gw_event_bus_publish("zigbee_ieee_lookup_ok", "zigbee", uid, resp->nwk_addr, "ieee resolved, starting discovery");
    gw_zigbee_start_discovery(resp->ieee_addr, resp->nwk_addr);

    free(ctx);
}

static void ieee_lookup_send_cb(uint8_t token)
{
    gw_zb_ieee_lookup_ctx_t *ctx = NULL;

    portENTER_CRITICAL(&s_ieee_lock);
    ctx = s_ieee_ctx_by_token[token];
    s_ieee_ctx_by_token[token] = NULL;
    portEXIT_CRITICAL(&s_ieee_lock);

    if (ctx == NULL) {
        return;
    }

    esp_zb_zdo_ieee_addr_req(&ctx->req, ieee_addr_cb, ctx);
}

esp_err_t gw_zigbee_discover_by_short(uint16_t short_addr)
{
    if (short_addr == 0 || short_addr == 0xFFFF) {
        return ESP_ERR_INVALID_ARG;
    }

    if (should_throttle_discovery(short_addr)) {
        return ESP_OK;
    }

    gw_zb_ieee_lookup_ctx_t *ctx = (gw_zb_ieee_lookup_ctx_t *)calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ctx->short_addr = short_addr;
    ctx->req.dst_nwk_addr = short_addr;
    ctx->req.addr_of_interest = short_addr;
    ctx->req.request_type = 0;
    ctx->req.start_index = 0;

    uint8_t token = 0;
    portENTER_CRITICAL(&s_ieee_lock);
    s_ieee_token++;
    if (s_ieee_token == 0) {
        s_ieee_token++;
    }
    token = s_ieee_token;
    if (s_ieee_ctx_by_token[token] != NULL) {
        portEXIT_CRITICAL(&s_ieee_lock);
        free(ctx);
        return ESP_FAIL;
    }
    s_ieee_ctx_by_token[token] = ctx;
    portEXIT_CRITICAL(&s_ieee_lock);

    gw_event_bus_publish("zigbee_ieee_lookup_requested", "zigbee", "", short_addr, "ieee_addr_req");

    // Schedule into Zigbee context.
    esp_zb_scheduler_alarm(ieee_lookup_send_cb, token, 0);
    return ESP_OK;
}

void gw_zigbee_on_device_annce(const uint8_t ieee_addr[8], uint16_t short_addr, uint8_t capability)
{
    if (ieee_addr == NULL) {
        return;
    }

    gw_device_t d = {0};
    ieee_to_uid_str(ieee_addr, d.device_uid.uid);

    // Preserve user-provided name and discovered capabilities across rejoin/announce.
    // Only refresh network-layer state here.
    {
        gw_device_t existing = {0};
        if (gw_device_registry_get(&d.device_uid, &existing) == ESP_OK) {
            d = existing;
        }
    }

    d.short_addr = short_addr;
    d.last_seen_ms = (uint64_t)(esp_timer_get_time() / 1000);

    esp_err_t err = gw_device_registry_upsert(&d);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "registry upsert failed for %s: %s", d.device_uid.uid, esp_err_to_name(err));
        gw_event_bus_publish("zigbee_device_annce_failed", "zigbee", d.device_uid.uid, d.short_addr, "device registry upsert failed");
        return;
    }

    ESP_LOGI(TAG, "Device announced: %s short=0x%04x cap=0x%02x", d.device_uid.uid, (unsigned)d.short_addr, (unsigned)capability);
    {
        char msg[64];
        (void)snprintf(msg, sizeof(msg), "cap=0x%02x", (unsigned)capability);
        gw_event_bus_publish("zigbee_device_annce", "zigbee", d.device_uid.uid, d.short_addr, msg);
    }

    // Discover device endpoints/clusters and auto-assign it to a type group.
    gw_zigbee_start_discovery(ieee_addr, short_addr);
}

esp_err_t gw_zigbee_device_leave(const gw_device_uid_t *uid, uint16_t short_addr, bool rejoin)
{
    if (uid == NULL || uid->uid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    gw_zb_leave_ctx_t *ctx = (gw_zb_leave_ctx_t *)calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ctx->uid = *uid;
    ctx->short_addr = short_addr;
    ctx->rejoin = rejoin;

    if (!uid_str_to_ieee(uid->uid, ctx->req.device_address)) {
        free(ctx);
        return ESP_ERR_INVALID_ARG;
    }
    ctx->req.dst_nwk_addr = short_addr;
    ctx->req.remove_children = 0;
    ctx->req.rejoin = rejoin ? 1 : 0;

    uint8_t token = 0;
    portENTER_CRITICAL(&s_leave_lock);
    s_leave_token++;
    if (s_leave_token == 0) {
        s_leave_token++;
    }
    token = s_leave_token;
    if (s_leave_ctx_by_token[token] != NULL) {
        portEXIT_CRITICAL(&s_leave_lock);
        free(ctx);
        return ESP_FAIL;
    }
    s_leave_ctx_by_token[token] = ctx;
    portEXIT_CRITICAL(&s_leave_lock);

    gw_event_bus_publish("zigbee_leave_requested", "zigbee", uid->uid, short_addr, rejoin ? "rejoin=1" : "rejoin=0");

    // Schedule into Zigbee context.
    esp_zb_scheduler_alarm(leave_send_cb, token, 0);
    return ESP_OK;
}

static void permit_join_cb(uint8_t seconds)
{
    esp_err_t err = esp_zb_bdb_open_network(seconds);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_zb_bdb_open_network(%u) failed: %s", (unsigned)seconds, esp_err_to_name(err));
        gw_event_bus_publish("zigbee_permit_join_failed", "zigbee", "", 0, "esp_zb_bdb_open_network failed");
        return;
    }
    ESP_LOGI(TAG, "permit_join enabled for %u seconds", (unsigned)seconds);
    {
        char msg[48];
        (void)snprintf(msg, sizeof(msg), "seconds=%u", (unsigned)seconds);
        gw_event_bus_publish("zigbee_permit_join_enabled", "zigbee", "", 0, msg);
    }
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
